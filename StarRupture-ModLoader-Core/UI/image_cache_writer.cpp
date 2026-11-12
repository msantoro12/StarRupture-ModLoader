#include "UI/image_cache_writer.h"
#include "UI/image_cache_care.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <cstdio>

namespace ImageCache
{
	namespace
	{
		// Creates the folder and every missing parent. Returns 0 or the Windows error.
		DWORD MakeFolders(const std::wstring& folder)
		{
			if (CreateDirectoryW(folder.c_str(), nullptr))
				return 0;
			DWORD error = GetLastError();
			if (error == ERROR_ALREADY_EXISTS)
				return 0;
			if (error != ERROR_PATH_NOT_FOUND)
				return error;

			const size_t cut = folder.find_last_of(L"\\/");
			if (cut == std::wstring::npos || cut == 0)
				return error;
			if (const DWORD parent = MakeFolders(folder.substr(0, cut)))
				return parent;
			if (CreateDirectoryW(folder.c_str(), nullptr))
				return 0;
			error = GetLastError();
			return error == ERROR_ALREADY_EXISTS ? 0 : error;
		}

		bool SameSizeExists(const std::wstring& path, uint64_t size)
		{
			WIN32_FILE_ATTRIBUTE_DATA data = {};
			if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) || (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
				return false;
			return ((static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow) == size;
		}

		// The errors that say the folder cannot be written to at all, as against one file that
		// could not be (a virus scanner holding it, say).
		bool FolderUnusable(DWORD error)
		{
			switch (error)
			{
			case ERROR_ACCESS_DENIED:
			case ERROR_WRITE_PROTECT:
			case ERROR_DISK_FULL:
			case ERROR_HANDLE_DISK_FULL:
			case ERROR_PATH_NOT_FOUND:
			case ERROR_INVALID_NAME:
			case ERROR_DIRECTORY:
			case ERROR_FILE_READ_ONLY:
				return true;
			default:
				return false;
			}
		}
	}

	Writer::Writer(Log log, size_t maxQueued)
		: m_log(std::move(log)), m_maxQueued(maxQueued)
	{
	}

	Writer::~Writer()
	{
		Stop();
	}

	bool Writer::Submit(Job&& job)
	{
		std::lock_guard<std::mutex> lock(m_lock);
		if (m_stop || m_off || m_queue.size() >= m_maxQueued || !job.make || !job.folder)
			return false;
		if (!m_thread.joinable())
			m_thread = std::thread(&Writer::Run, this);
		m_queue.push_back(std::move(job));
		m_wake.notify_one();
		return true;
	}

	bool Writer::Off() const
	{
		std::lock_guard<std::mutex> lock(m_lock);
		return m_off;
	}

	void Writer::Stop()
	{
		{
			std::lock_guard<std::mutex> lock(m_lock);
			m_stop = true;
			m_queue.clear();
			m_wake.notify_all();
		}
		if (m_thread.joinable())
			m_thread.join();
	}

	void Writer::Run()
	{
		for (;;)
		{
			Job job;
			{
				std::unique_lock<std::mutex> lock(m_lock);
				m_wake.wait(lock, [this] { return m_stop || !m_queue.empty(); });
				if (m_stop)
					return;
				job = std::move(m_queue.front());
				m_queue.pop_front();
			}
			if (!Write(job))
			{
				std::lock_guard<std::mutex> lock(m_lock);
				m_off = true;
				m_queue.clear();
			}
		}
	}

	void Writer::SwitchOff(const char* step, unsigned long error, const std::wstring& folder)
	{
		char line[512];
		snprintf(line, sizeof(line), "[Images] The image cache folder cannot be written (%s, error %lu, %ls): images are not saved this session",
			step, error, folder.c_str());
		if (m_log)
			m_log(line);
	}

	bool Writer::Write(const Job& job)
	{
		const std::vector<uint8_t> bytes = job.make();
		if (bytes.empty())
			return true;

		std::wstring name(job.name.begin(), job.name.end());
		const std::wstring folder = job.folder();
		std::wstring path = folder;
		if (!path.empty() && path.back() != L'\\' && path.back() != L'/')
			path += L'\\';
		path += name;

		if (!job.replace && SameSizeExists(path, bytes.size()))
		{
			Care::Touch(path);   // in use again: the size cap deletes the least recently used first
			return true;
		}

		if (const DWORD error = MakeFolders(folder))
		{
			SwitchOff("create the folder", error, folder);
			return false;
		}

		const std::wstring temp = path + L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
		HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE)
		{
			const DWORD error = GetLastError();
			if (!FolderUnusable(error))
				return true;
			SwitchOff("create a file", error, folder);
			return false;
		}

		DWORD wrote = 0;
		bool ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &wrote, nullptr)
			&& wrote == bytes.size() && FlushFileBuffers(file);
		DWORD error = ok ? 0 : GetLastError();
		CloseHandle(file);
		if (ok && !MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
		{
			ok = false;
			error = GetLastError();
		}
		if (ok)
			return true;

		DeleteFileW(temp.c_str());
		if (!FolderUnusable(error))
			return true;
		SwitchOff("write a file", error, folder);
		return false;
	}
}
