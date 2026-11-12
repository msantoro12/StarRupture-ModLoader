#include "pch.h"
#include "UI/image_disk.h"

#ifdef MODLOADER_CLIENT_BUILD

#include "UI/image_cache_care.h"
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
		// Where every game build's folder goes, under the loader's own data folder.
		std::wstring CacheRoot()
		{
			return GetExeDir() + L"ModLoader\\image-cache\\";
		}

		void LogLine(const std::string& line)
		{
			LogToFile::Info("%s", line.c_str());
		}

		// The folder the files of this game build go in:
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
			return CacheRoot() + ImageCache::FolderName(key);
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
				const uint64_t pakHash = MountedPakHash();
				return [pakHash] { return FolderFor(pakHash); };
			}
		};
	}

	namespace
	{
		// Never destroyed: the pump can still be asked at process exit.
		ImageCache::Writer& EngineWriter()
		{
			static ImageCache::Writer* const writer = new ImageCache::Writer([](const std::string& line) { LogToFile::Warn("%s", line.c_str()); });
			return *writer;
		}
	}

	Saver* EngineSaver()
	{
		static EngineHost* const host = new EngineHost();
		static DiskSaver* const saver = [&]
		{
			ImageCache::Writer& writer = EngineWriter();
			// Old folders, leftovers and the size cap are looked after once, as the first job.
			writer.Submit(ImageCache::Care::TidyJob(&CacheRoot, host->CacheFolder(), &LogLine));
			return new DiskSaver(ImageLibrary::Instance(), *host, writer);
		}();
		return saver;
	}

	bool ClearCache()
	{
		if (EngineWriter().Submit(ImageCache::Care::ClearJob(&CacheRoot, &LogLine)))
			return true;
		LogToFile::Info("[Images] Image cache not cleared: the saver is busy or off");
		return false;
	}
}

#endif // MODLOADER_CLIENT_BUILD
