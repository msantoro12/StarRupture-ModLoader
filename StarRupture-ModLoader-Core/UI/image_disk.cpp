#include "UI/image_disk.h"
#include "UI/image_shrink.h"

#include <algorithm>
#include <memory>

namespace ImageDisk
{
	DiskSaver::DiskSaver(ImageLibrary::Library& library, Host& host, ImageCache::Sink& sink)
		: m_library(library), m_host(host), m_sink(sink)
	{
	}

	void DiskSaver::Begin(const Started& started)
	{
		// No ticket: every readback buffer was taken, and this image stays in memory only.
		if (started.ticket == 0 || m_gone || m_stopped)
			return;
		m_pending.push_back(started);
	}

	void DiskSaver::Poll(uint64_t nowMs)
	{
		if (m_gone || m_stopped)
			return;

		for (size_t i = 0; i < m_pending.size();)
		{
			const Started& started = m_pending[i];
			const size_t bytes = static_cast<size_t>(ImageShrink::Bytes({ started.storedWidth, started.storedHeight }, started.levels));

			// With the disk part off the readback is only freed.
			std::vector<uint8_t> pixels;
			if (!m_sink.Off())
				pixels.resize(bytes);

			const Readback state = m_host.Take(started.ticket, pixels.empty() ? nullptr : pixels.data(), bytes);
			if (state == Readback::Pending)
			{
				++i;
				continue;
			}
			if (state == Readback::Done && !pixels.empty())
				Finish(started, std::move(pixels));
			m_pending.erase(m_pending.begin() + static_cast<std::ptrdiff_t>(i));
		}

		if (nowMs >= m_nextSetCheckMs)
		{
			m_nextSetCheckMs = nowMs + kSetListIntervalMs;
			WriteSetList();
			WriteNameList();
		}
	}

	void DiskSaver::Name(const std::string& name, const std::string& path)
	{
		if (m_stopped || name.empty() || path.empty())
			return;
		std::string& kept = m_names[name];
		if (kept != path)
		{
			kept = path;
			m_namesChanged = true;
		}
	}

	void DiskSaver::Finish(const Started& started, std::vector<uint8_t>&& pixels)
	{
		const std::string key = ImageLibrary::NormalizeObjectPath(started.path.c_str());
		if (key.empty())
			return;

		ImageCache::Info info;
		info.sourceFormat = started.sourceFormat;
		info.sourceWidth  = started.sourceWidth;
		info.sourceHeight = started.sourceHeight;
		info.storedWidth  = started.storedWidth;
		info.storedHeight = started.storedHeight;
		info.levels       = started.levels;
		info.path         = started.path;

		const auto data = std::make_shared<std::vector<uint8_t>>(std::move(pixels));
		ImageCache::Job job;
		job.folder = m_host.CacheFolder();
		job.name   = ImageCache::FileName(key);
		job.make   = [info, data] { return ImageCache::Serialize(info, data->data(), data->size()); };
		m_sink.Submit(std::move(job));
	}

	// Writes the list of images in each set when it is not the one last written.
	void DiskSaver::WriteSetList()
	{
		std::vector<ImageLibrary::SetMember> members;
		m_library.CollectSets(members);

		ImageCache::SetList sets;
		for (uint32_t bit = 0; bit < 32; ++bit)
		{
			ImageCache::SetEntry entry;
			entry.set = 1u << bit;
			for (const ImageLibrary::SetMember& member : members)
				if (member.sets & entry.set)
					entry.paths.push_back(member.path);
			if (entry.paths.empty())
				continue;
			std::sort(entry.paths.begin(), entry.paths.end());
			entry.paths.erase(std::unique(entry.paths.begin(), entry.paths.end()), entry.paths.end());
			sets.push_back(std::move(entry));
		}
		if (sets.empty() || sets == m_lastSets)
			return;

		ImageCache::Job job;
		job.folder  = m_host.CacheFolder();
		job.name    = ImageCache::SetListFileName();
		job.replace = true;
		job.make    = [sets] { return ImageCache::SerializeSets(sets); };
		if (m_sink.Submit(std::move(job)))
			m_lastSets = std::move(sets);
	}

	// Writes the item names that resolved to a texture when there are new ones.
	void DiskSaver::WriteNameList()
	{
		if (!m_namesChanged)
			return;

		ImageCache::NameList names;
		names.reserve(m_names.size());
		for (const auto& entry : m_names)
			names.push_back({ entry.first, entry.second });

		ImageCache::Job job;
		job.folder  = m_host.CacheFolder();
		job.name    = ImageCache::NameListFileName();
		job.replace = true;
		job.make    = [names] { return ImageCache::SerializeNames(names); };
		if (m_sink.Submit(std::move(job)))
			m_namesChanged = false;
	}

	void DiskSaver::Abandon()
	{
		m_gone = true;
		m_pending.clear();
	}

	void DiskSaver::Stop()
	{
		m_stopped = true;
		m_pending.clear();
		m_sink.Stop();
	}
}
