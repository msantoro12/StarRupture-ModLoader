#include "pch.h"
#include "UI/image_disk.h"

#ifdef MODLOADER_CLIENT_BUILD

#include "UI/image_cache_reader.h"
#include "UI/imgui_backend.h"
#include "core/startup_utils.h"
#include "core/version_check.h"
#include "logging/log.h"
#include "memory_scanner/image_info.h"
#include "memory_scanner/scan_cache.h"
#include "plugins/pak_registry.h"

#include <mutex>

namespace ImageDisk
{
	namespace
	{
		// The folder the files of this game build go in, under the loader's own data folder:
		// image-cache\<build, pak set, revision, stored size>. Made by the worker.
		std::wstring FolderFor(uint64_t pakHash)
		{
			// Reads the exe's version resource, so it runs on the worker only.
			static std::wstring section;
			static std::once_flag once;
			std::call_once(once, []
			{
				const Scanner::ImageInfo image = Scanner::GetImageInfo(GetModuleHandleW(nullptr));
				section = ScanCache::SectionName(GetGameVersionString(), image.stamp, static_cast<uint32_t>(image.size));
			});

			ImageCache::FolderKey key;
			key.section    = section;
			key.pakHash    = pakHash;
			key.storedSize = ImageLibrary::kStoredSize;
			return GetExeDir() + L"ModLoader\\image-cache\\" + ImageCache::FolderName(key);
		}

		// The mounted pak set as it is now. Paks mounted later give later files another folder.
		uint64_t MountedPakHash()
		{
			IPluginPak* const pak = PakRegistry::GetInterface();
			if (!pak)
				return 0;
			const int total = pak->GetMountedInto(nullptr, nullptr, 0);
			if (total <= 0)
				return 0;
			std::vector<PluginPakInfo> infos(static_cast<size_t>(total));
			const int filled = pak->GetMountedInto(nullptr, infos.data(), total);

			std::vector<ImageCache::PakEntry> paks;
			for (int i = 0; i < filled && i < total; ++i)
				paks.push_back({ infos[static_cast<size_t>(i)].pakPath, infos[static_cast<size_t>(i)].mountPoint });
			return ImageCache::HashPakSet(std::move(paks));
		}

		// The folder of this session. The key of the mounted pak set is taken once, when the disk part
		// is first used (the first tick the GPU side is up, after the plugins' paks are mounted at
		// start), and every file read or written goes by it. It is looked at again at most every
		// kPakCheckMs; if the pak set has changed, the disk part goes off for the rest of the session:
		// images read from the game after a pak was mounted might differ from what the key stands for,
		// and must never be saved under it. Files written before the change were read under the set
		// they are filed under. Game thread only.
		class Session
		{
		public:
			static constexpr ULONGLONG kPakCheckMs = 250;

			// The function that gives the folder, or an empty one when the disk part is off.
			std::function<std::wstring()> Folder()
			{
				if (m_off)
					return {};
				const ULONGLONG now = GetTickCount64();
				if (!m_taken)
				{
					m_taken     = true;
					m_checkedMs = now;
					m_pakHash   = MountedPakHash();
					const uint64_t pakHash = m_pakHash;
					m_folder = [pakHash] { return FolderFor(pakHash); };
				}
				else if (now - m_checkedMs >= kPakCheckMs)
				{
					m_checkedMs = now;
					if (MountedPakHash() != m_pakHash)
					{
						m_off = true;
						LogToFile::Warn("[Images] The mounted paks changed: the image cache is not used for the rest of this session");
						return {};
					}
				}
				return m_folder;
			}

		private:
			bool                          m_taken = false;
			bool                          m_off   = false;
			uint64_t                      m_pakHash = 0;
			ULONGLONG                     m_checkedMs = 0;
			std::function<std::wstring()> m_folder;
		};

		Session& TheSession()
		{
			static Session* const session = new Session();
			return *session;
		}

		class EngineUploader final : public ImageCache::Uploader
		{
		public:
			bool Upload(uint32_t width, uint32_t height, uint32_t levels, const uint8_t* pixels, size_t bytes, PluginTextureHandle& texture) override
			{
				ImGuiHostUploadOut out = {};
				if (UI::ImGuiBackend::UploadTexture(width, height, levels, pixels, bytes, &out) != ImGuiHostUpload_Started)
					return false;
				texture = out.texture;
				return true;
			}
		};

		class EngineHost final : public Host
		{
		public:
			Readback Take(uint32_t ticket, uint8_t* dest, size_t bytes) override
			{
				switch (UI::ImGuiBackend::TakeReadback(ticket, dest, bytes))
				{
				case 1:  return Readback::Done;
				case 0:  return Readback::Pending;
				default: return Readback::Gone;
				}
			}

			std::function<std::wstring()> CacheFolder() override
			{
				return TheSession().Folder();
			}
		};
	}

	// Never destroyed: the pump can still be asked at process exit.
	Saver* EngineSaver()
	{
		static EngineHost* const host = new EngineHost();
		static ImageCache::Writer* const writer = new ImageCache::Writer([](const std::string& line) { LogToFile::Warn("%s", line.c_str()); });
		static DiskSaver* const saver = new DiskSaver(ImageLibrary::Instance(), *host, *writer);
		return saver;
	}

	Source* EngineSource()
	{
		static EngineUploader* const uploader = new EngineUploader();
		static ImageCache::Reader* const reader = new ImageCache::Reader(
			[] { return TheSession().Folder(); }, *uploader,
			[](const std::string& line) { LogToFile::Info("%s", line.c_str()); });
		return reader;
	}
}

#endif // MODLOADER_CLIENT_BUILD
