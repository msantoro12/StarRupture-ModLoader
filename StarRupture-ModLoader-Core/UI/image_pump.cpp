#include "UI/image_pump.h"
#include "UI/image_shrink.h"

#include <algorithm>
#include <cstdio>

namespace ImagePump
{
	namespace
	{
		const char* PauseText(int pause)
		{
			static const char* const kText[] = { "no", "setting off", "no set in use", "reading the list", "image waiting on screen",
				"game holds loaded images", "video memory", "cache full" };
			return kText[pause];
		}
	}

	Pump::Pump(ImageLibrary::Library& library, Host& host, ImageLookup::ItemSource& items, ImageLookup::ItemSource& buildings)
		: m_library(library), m_host(host), m_items(items), m_buildings(buildings)
	{
	}

	void Pump::Tick()
	{
		if (m_stopped)
			return;

		const bool ready = m_host.GpuReady();
		if (!m_started)
		{
			if (!ready)
				return;
			Start();
		}
		else if (!ready && !m_gone)
		{
			GpuGone();
		}

		const uint64_t begin = m_host.NowUs();
		m_library.SetNow(begin / 1000);

		FreeDeferred();
		if (!m_gone)
		{
			Collect();
			CollectFiles();
		}
		if (m_saver)
			m_saver->Poll(begin / 1000);
		// The saved names and sets come in before anything is served, so no image is read from the
		// game that a file would have answered.
		const bool filesLoading = PollSource(begin / 1000);
		if (m_off)
			FailQueued();
		else if (!filesLoading)
		{
			uint32_t loads = 0;
			const bool idle = Serve(loads);
			const bool stepped = StepItems();
			Preload(idle, stepped, loads);
		}

		const uint64_t end = m_host.NowUs();
		m_stats.longestTickUs = std::max(m_stats.longestTickUs, end - begin);
		if (end / 1000 >= m_nextSummaryMs)
		{
			m_nextSummaryMs = end / 1000 + kSummaryIntervalMs;
			LogSummary();
		}
	}

	void Pump::Start()
	{
		m_started = true;
		m_library.SetCaps(ImageLibrary::ByteCapForBudget(m_host.LocalVideoBudget()), ImageLibrary::kCountCap);
	}

	// The device was lost or the overlay shut down under us. The texture API
	// releases every texture it holds when it tears down, so the pump frees none.
	void Pump::GpuGone()
	{
		m_gone = true;
		m_off  = true;
		if (m_saver)
			m_saver->Abandon();
		if (m_source)
			m_source->Abandon();

		size_t dropped = m_inFlight.size() + m_onDisk.size();
		for (const InFlight& read : m_inFlight)
			m_library.Fail(read.id);
		for (PluginImageId id : m_onDisk)
			m_library.Fail(id);
		m_stats.failed += m_inFlight.size() + m_onDisk.size();
		m_inFlight.clear();
		m_onDisk.clear();

		m_scratch.clear();
		m_library.ReleaseAll(m_scratch);
		dropped += m_scratch.size();
		dropped += m_toFree.size();
		m_toFree.clear();
		m_scratch.clear();

		m_host.Log("[Images] The GPU side is gone: " + std::to_string(dropped) + " images dropped, no more reads this session");
	}

	// The reads that finished on the GPU are published; the evicted copies they
	// push out wait for the next tick to be freed.
	void Pump::Collect()
	{
		for (size_t i = 0; i < m_inFlight.size();)
		{
			const InFlight read = m_inFlight[i];
			const CopyState state = m_host.StateOf(read.copy.texture);
			if (state == CopyState::Pending)
			{
				++i;
				continue;
			}

			Stored stored;
			StoreResult made = StoreResult::Failed;
			if (state == CopyState::Done)
				made = Store(read, stored);
			if (made == StoreResult::Later)
			{
				// The shrink has no room yet: the copy is kept and tried again next tick.
				++i;
				continue;
			}

			m_inFlight[i] = m_inFlight.back();
			m_inFlight.pop_back();

			if (made == StoreResult::Failed)
			{
				// A copy that is gone is not ours to free.
				if (state == CopyState::Done)
					m_host.Free(read.copy.texture);
				m_library.Fail(read.id);
				++m_stats.failed;
				continue;
			}

			if (PublishStored(read.id, stored.texture, stored.bytes))
				++m_stats.fromGame;
		}
	}

	// The stored copy goes into the library, whether it came from a shrink or from a file. True when
	// it is kept.
	bool Pump::PublishStored(PluginImageId id, PluginTextureHandle texture, uint64_t bytes)
	{
		m_scratch.clear();
		const bool kept = m_library.Publish(id, texture, bytes, m_scratch);
		if (kept)
			++m_stats.published;
		else
		{
			// Refused, or evicted again at once (then it is among the evicted).
			const bool evictedAgain = std::any_of(m_scratch.begin(), m_scratch.end(),
				[&](const ImageLibrary::Released& r) { return r.handle == texture; });
			if (!evictedAgain)
			{
				m_host.Free(texture);
				++m_stats.failed;
			}
		}
		for (const ImageLibrary::Released& released : m_scratch)
			m_toFree.push_back(released.handle);
		m_stats.evicted += m_scratch.size();
		return kept;
	}

	// The files the source has finished with. A good one is published; one that failed leaves the
	// image queued, and the game is read for it (the source no longer lists its file).
	void Pump::CollectFiles()
	{
		if (!m_source)
			return;
		m_files.clear();
		m_source->Collect(m_files);
		for (const ImageDisk::FileResult& result : m_files)
		{
			m_onDisk.erase(result.id);
			if (result.outcome != ImageDisk::FileOutcome::Texture)
			{
				m_library.Requeue(result.id);
				continue;
			}
			if (PublishStored(result.id, result.texture, result.bytes))
				++m_stats.fromDisk;
		}
	}

	// Gives the source its turn. True while the folder is being listed: nothing is served then.
	// Once it is listed, its saved names and set lists are taken in.
	bool Pump::PollSource(uint64_t nowMs)
	{
		if (!m_source || m_gone)
			return false;
		const ImageDisk::Source::State state = m_source->Poll(nowMs);
		if (state == ImageDisk::Source::State::Loading)
			return true;
		m_sourceOff = state == ImageDisk::Source::State::Off;
		if (state == ImageDisk::Source::State::Ready && !m_listsTaken)
		{
			ImageDisk::Lists lists;
			if (m_source->TakeLists(lists))
			{
				m_listsTaken = true;
				ApplyLists(std::move(lists));
			}
		}
		return false;
	}

	// Ties each saved item name to its texture path. An item that is already being read is left to
	// the index.
	void Pump::ApplyLists(ImageDisk::Lists&& lists)
	{
		size_t tied = 0;
		for (const ImageCache::NamePair& pair : lists.names)
		{
			const PluginImageId item  = m_library.Item(pair.name.c_str());
			const PluginImageId image = m_library.Path(pair.path.c_str());
			if (item == 0 || image == 0)
				continue;
			tied += m_library.Alias(item, image);
			if (m_saver)
				m_saver->Name(pair.name, pair.path);
		}
		m_fileSets = std::move(lists.sets);
		if (tied != 0 || !m_fileSets.empty())
			m_host.Log("[Images] Cache folder: " + std::to_string(tied) + " item names and " + std::to_string(m_fileSets.size()) + " set lists read");
	}

	// The hand-over from the read to the library. The read made one level of the
	// game image in a temporary texture; the shrink draws it at the stored size with
	// every smaller level, and the temporary texture is let go at once (the host
	// keeps it until the shrink has run). Later: the shrink has no room, the copy
	// stays as it is. Failed: the image cannot be made, and the caller frees the copy.
	Pump::StoreResult Pump::Store(const InFlight& read, Stored& out)
	{
		if (!read.copy.texture || read.copy.width <= 0 || read.copy.height <= 0)
			return StoreResult::Failed;

		Shrunk shrunk;
		switch (m_host.Shrink(read.copy.texture, shrunk))
		{
		case ShrinkResult::Started:
			break;
		case ShrinkResult::Busy:
			return StoreResult::Later;
		default:
			return StoreResult::Failed;
		}

		m_host.Free(read.copy.texture);
		if (m_saver)
			m_saver->Begin({ read.path, read.copy.sourceFormat, static_cast<uint32_t>(read.copy.sourceWidth), static_cast<uint32_t>(read.copy.sourceHeight),
				static_cast<uint32_t>(shrunk.width), static_cast<uint32_t>(shrunk.height), shrunk.levels, shrunk.readback });
		out.texture = shrunk.texture;
		out.bytes   = ImageShrink::Bytes({ static_cast<uint32_t>(shrunk.width), static_cast<uint32_t>(shrunk.height) }, shrunk.levels);
		return StoreResult::Stored;
	}

	// True when no on-demand request was looked at, so the tick is free for preload.
	bool Pump::Serve(uint32_t& loads)
	{
		uint32_t reads = 0, attempts = 0;
		m_later.clear();

		ImageLibrary::Request request;
		while (reads < kReadsPerTick && attempts < kAttemptsPerTick && m_library.TakeNext(request))
		{
			// An item waits for the index; it costs no read, so it is no attempt.
			if (request.kind == ImageLibrary::Kind::Item)
			{
				m_held.push_back({ request.id, ImageLibrary::NormalizeItemName(request.name.c_str()) });
				continue;
			}

			++attempts;
			const Outcome outcome = Attempt(request, loads);
			if (outcome == Outcome::Started)
				++reads;
			else if (outcome == Outcome::Later || outcome == Outcome::Busy)
				m_later.push_back(request.id);

			if (outcome == Outcome::Busy || outcome == Outcome::Off)
				break;
		}

		// Only now: a request put back at once would be handed out again.
		for (PluginImageId id : m_later)
			m_library.Requeue(id);
		if (m_off)
			FailQueued();
		return attempts == 0;
	}

	// Reads the next slice of the item index while an item is waiting, and
	// answers the waiting items it now knows. True when it read a slice.
	bool Pump::StepItems()
	{
		if (m_held.empty())
			return false;

		ResolveHeld();
		if (m_held.empty())
			return false;

		const ImageLookup::ItemIndex::State before = m_items.GetState();
		m_items.Step(ImageLookup::kItemBudgetUs, [this] { return m_host.NowUs(); });
		if (before != ImageLookup::ItemIndex::State::Failed && m_items.GetState() == ImageLookup::ItemIndex::State::Failed)
			m_host.Log("[Images] No item can be read on this build or in this session: item images stay unavailable");
		ResolveHeld();
		return true;
	}

	// ---- Preload ---------------------------------------------------------------

	// One tick of preload: the lists of the sets in use are read (a slice a tick,
	// one list at a time), and then, on one tick in kPreloadTickInterval and only
	// while every rule holds, the next member of a set is read.
	void Pump::Preload(bool idle, bool stepped, uint32_t& loads)
	{
		if (!m_host.PreloadEnabled())
		{
			m_pause = Pause::Off;
			return;
		}
		const uint32_t used = m_library.UsedSets();
		if (used == 0)
		{
			m_pause = Pause::NoSets;
			return;
		}

		StepSets(used, stepped);

		if (++m_preloadTick % kPreloadTickInterval != 0)
			return;
		if (!idle || m_library.SeenWaiting())
			m_pause = Pause::Screen;
		else if (!m_library.PreloadHasRoom())
			m_pause = Pause::Full;
		else if (VideoFull())
			m_pause = Pause::VideoMemory;
		else
		{
			// What the game holds loaded is the game's own memory: it limits the reads of the game, not
			// the files, which load nothing.
			const bool holds = GameHoldsTooMuch();
			for (uint32_t taken = 0; taken < kDiskPreloadPerTick; ++taken)
			{
				ImageLibrary::Request request;
				if (taken != 0 && !m_library.PreloadHasRoom())
					break;
				if (!m_library.TakePreload(request))
				{
					if (taken != 0)
						break;
					const bool unlisted = ((used & PLUGIN_IMAGE_SET_ITEMS) && !m_itemsListed)
						|| ((used & PLUGIN_IMAGE_SET_BUILDINGS) && !m_buildingsListed);
					m_pause = unlisted ? Pause::Listing : Pause::Running;
					return;
				}

				std::string key;
				if (holds && !FileFor(request, key))
				{
					m_library.Defer(request.id);
					m_pause = Pause::GameHolds;
					return;
				}

				m_pause = Pause::Running;
				const Outcome outcome = Attempt(request, loads);
				if (outcome == Outcome::Later || outcome == Outcome::Busy)
					m_library.Defer(request.id);
				if (outcome != Outcome::Disk)
					break;
			}
		}
	}

	// Reads a slice of the list of one set that is in use and not listed yet. A
	// slice is read for one set a tick, and not on a tick the item index has
	// already been stepped for an item that is waiting.
	void Pump::StepSets(uint32_t used, bool stepped)
	{
		if ((used & PLUGIN_IMAGE_SET_ITEMS) && !m_itemsListed)
			stepped = StepSet(m_items, PLUGIN_IMAGE_SET_ITEMS, m_itemsListed, stepped, "item") || stepped;
		if ((used & PLUGIN_IMAGE_SET_BUILDINGS) && !m_buildingsListed)
			StepSet(m_buildings, PLUGIN_IMAGE_SET_BUILDINGS, m_buildingsListed, stepped, "building");
	}

	// True when it read a slice. When the list is whole its images are added to
	// the library as members of the set; when it cannot be made the set is left
	// empty for the session.
	bool Pump::StepSet(ImageLookup::ItemIndex& index, uint32_t set, bool& listed, bool stepped, const char* what)
	{
		using State = ImageLookup::ItemIndex::State;
		bool read = false;
		if (ApplyFileSet(set, listed, what))
			return false;
		if (!stepped && (index.GetState() == State::Idle || index.GetState() == State::Scanning))
		{
			index.Step(ImageLookup::kItemBudgetUs, [this] { return m_host.NowUs(); });
			read = true;
		}

		if (index.GetState() == State::Done)
		{
			size_t members = 0;
			for (const std::string& path : index.TexturePaths())
				members += m_library.PathInSet(path.c_str(), set) != 0;
			listed = true;
			m_host.Log(std::string("[Images] Preload set: ") + std::to_string(members) + " " + what + " images");
		}
		else if (index.GetState() == State::Failed)
		{
			listed = true;
			m_host.Log(std::string("[Images] The ") + what + " list cannot be read here: " + what + " images are not preloaded");
		}
		return read;
	}

	// Fills the set from the saved set list, when the folder had one for it: the game's list is not
	// read, and no package is loaded to build it. True when it did.
	bool Pump::ApplyFileSet(uint32_t set, bool& listed, const char* what)
	{
		const auto saved = std::find_if(m_fileSets.begin(), m_fileSets.end(),
			[set](const ImageCache::SetEntry& entry) { return entry.set == set && !entry.paths.empty(); });
		if (saved == m_fileSets.end())
			return false;

		size_t members = 0;
		for (const std::string& path : saved->paths)
			members += m_library.PathInSet(path.c_str(), set) != 0;
		m_fileSets.erase(saved);
		listed = true;
		m_host.Log(std::string("[Images] Preload set: ") + std::to_string(members) + " " + what + " images (from the cache folder)");
		return true;
	}

	// Whether the full-size images the library made the game load, and the game
	// is taken to hold still, have reached the limit.
	bool Pump::GameHoldsTooMuch()
	{
		const uint64_t nowMs = m_host.NowUs() / 1000;
		m_heldLoads.erase(std::remove_if(m_heldLoads.begin(), m_heldLoads.end(),
			[&](const HeldLoad& load) { return nowMs - load.atMs >= kPreloadHeldWindowMs; }), m_heldLoads.end());
		uint64_t total = 0;
		for (const HeldLoad& load : m_heldLoads)
			total += load.bytes;
		return total >= kPreloadHeldLimitBytes;
	}

	// Whether the adapter's local video memory in use is at the share of its
	// budget that preload leaves alone. Asked once in kVideoCheckIntervalMs. An
	// adapter that cannot be asked is taken to have room: the byte cap still holds.
	bool Pump::VideoFull()
	{
		const uint64_t nowMs = m_host.NowUs() / 1000;
		if (nowMs >= m_nextVideoCheckMs)
		{
			m_nextVideoCheckMs = nowMs + kVideoCheckIntervalMs;
			uint64_t usage = 0, budget = 0;
			m_videoFull = m_host.VideoMemory(usage, budget) && budget > 0 && usage * 100 >= budget * kPreloadVideoPercent;
		}
		return m_videoFull;
	}

	void Pump::ResolveHeld()
	{
		m_held.erase(std::remove_if(m_held.begin(), m_held.end(), [this](const HeldItem& item) { return Resolved(item); }),
			m_held.end());
	}

	// True when the item is answered. An item whose icon is a texture becomes
	// the image of that texture's path: one id's worth of copy for both.
	bool Pump::Resolved(const HeldItem& item)
	{
		std::string path;
		if (!m_items.Find(item.key, path))
		{
			switch (m_items.GetState())
			{
			case ImageLookup::ItemIndex::State::Done:   // there is no item of that name
				m_library.Fail(item.id);
				++m_stats.failed;
				return true;
			case ImageLookup::ItemIndex::State::Failed:
				m_library.Fail(item.id);
				++m_stats.failed;
				return true;
			default:
				return false;
			}
		}

		if (path.empty() || ImageLookup::IsPlaceholderImage(path.c_str()))
		{
			m_library.PublishNoImage(item.id);
			++m_stats.noImage;
			return true;
		}

		const PluginImageId image = m_library.Path(path.c_str());
		if (image == 0)   // the table is full
		{
			m_library.Fail(item.id);
			++m_stats.failed;
			return true;
		}
		m_library.Requeue(item.id);
		m_library.Alias(item.id, image);
		if (m_saver)
			m_saver->Name(item.key, path);
		return true;
	}

	// The normalised path of the file this request would be read from, when the source has one.
	bool Pump::FileFor(const ImageLibrary::Request& request, std::string& key)
	{
		if (!m_source || m_sourceOff)
			return false;
		key =ImageLibrary::NormalizeObjectPath(request.name.c_str());
		return !key.empty() && m_source->Has(key);
	}

	Pump::Outcome Pump::Attempt(const ImageLibrary::Request& request, uint32_t& loads)
	{
		if (ImageLookup::IsPlaceholderImage(request.name.c_str()))
		{
			m_library.PublishNoImage(request.id);
			++m_stats.noImage;
			return Outcome::Done;
		}

		// A file first: no game image is read and no package is loaded for it.
		std::string key;
		if (FileFor(request, key))
		{
			if (!m_source->Request(request.id, key))
				return Outcome::Later;
			m_onDisk.insert(request.id);
			return Outcome::Disk;
		}

		Copy copy;
		ReadResult result = m_host.Read(request.name, copy);

		if (result == ReadResult::NotLoaded)
		{
			if (loads >= kLoadsPerTick)
				return Outcome::Later;
			++loads;
			switch (m_host.Load(request.name))
			{
			case LoadResult::Loaded:
				m_causedLoad.insert(request.id);
				result = m_host.Read(request.name, copy);
				break;
			case LoadResult::Missing:
				m_library.PublishNoImage(request.id);
				++m_stats.noImage;
				return Outcome::Done;
			case LoadResult::Fault:
			case LoadResult::Unsupported:
				m_library.Fail(request.id);
				++m_stats.failed;
				return Outcome::Done;
			}
		}

		switch (result)
		{
		case ReadResult::Started:
			m_inFlight.push_back({ request.id, copy, request.name });
			if (m_causedLoad.erase(request.id))
			{
				const uint64_t width  = static_cast<uint64_t>(copy.sourceWidth > 0 ? copy.sourceWidth : copy.width);
				const uint64_t height = static_cast<uint64_t>(copy.sourceHeight > 0 ? copy.sourceHeight : copy.height);
				m_heldLoads.push_back({ m_host.NowUs() / 1000, width * height * ImageShrink::kBytesPerPixel });
			}
			return Outcome::Started;
		case ReadResult::NotYet:
			return Outcome::Later;
		case ReadResult::Busy:
			return Outcome::Busy;
		case ReadResult::Unavailable:
			m_off = true;
			m_library.Fail(request.id);
			++m_stats.failed;
			m_host.Log("[Images] No image can be read on this build or in this session: images stay unavailable");
			return Outcome::Off;
		case ReadResult::NotLoaded:   // loaded a moment ago and not found: no texture there
			m_library.PublishNoImage(request.id);
			++m_stats.noImage;
			return Outcome::Done;
		case ReadResult::Failed:
		default:
			m_library.Fail(request.id);
			++m_stats.failed;
			return Outcome::Done;
		}
	}

	// No read can run: every request ends as NONE instead of waiting for ever.
	void Pump::FailQueued()
	{
		FailHeld();
		ImageLibrary::Request request;
		while (m_library.TakeNext(request))
		{
			m_library.Fail(request.id);
			++m_stats.failed;
		}
	}

	void Pump::FailHeld()
	{
		for (const HeldItem& item : m_held)
			m_library.Fail(item.id);
		m_stats.failed += m_held.size();
		m_held.clear();
	}

	void Pump::FreeDeferred()
	{
		if (!m_gone)
			for (PluginTextureHandle texture : m_toFree)
				m_host.Free(texture);
		m_toFree.clear();
	}

	void Pump::Shutdown()
	{
		if (m_stopped)
			return;
		m_stopped = true;
		if (m_saver)
			m_saver->Stop();
		if (m_source)
		{
			// What the worker finished and nobody collected is freed, and what it never got to fails.
			m_source->Stop();
			m_files.clear();
			m_source->Collect(m_files);
			for (const ImageDisk::FileResult& result : m_files)
				if (!m_gone && result.texture)
					m_host.Free(result.texture);
			m_files.clear();
			for (PluginImageId id : m_onDisk)
				m_library.Fail(id);
			m_onDisk.clear();
		}
		LogSummary();
		FailHeld();

		for (const InFlight& read : m_inFlight)
		{
			m_library.Fail(read.id);
			if (!m_gone)
				m_host.Free(read.copy.texture);
		}
		m_inFlight.clear();

		m_scratch.clear();
		m_library.ReleaseAll(m_scratch);
		if (!m_gone)
			for (const ImageLibrary::Released& released : m_scratch)
				m_host.Free(released.handle);
		m_scratch.clear();

		FreeDeferred();
	}

	void Pump::LogSummary()
	{
		const ImageLibrary::Library::PreloadProgress preload = m_library.GetPreloadProgress();
		m_stats.written  = m_saver ? m_saver->Written() : 0;
		m_stats.rejected = m_source ? m_source->Rejected() : 0;
		const bool changed = m_stats.published != m_lastLogged.published || m_stats.evicted != m_lastLogged.evicted
			|| m_stats.fromDisk != m_lastLogged.fromDisk || m_stats.fromGame != m_lastLogged.fromGame
			|| m_stats.written != m_lastLogged.written || m_stats.rejected != m_lastLogged.rejected
			|| m_stats.failed != m_lastLogged.failed || m_stats.noImage != m_lastLogged.noImage
			|| preload.queued != m_lastPreload.queued || preload.done != m_lastPreload.done || m_pause != m_lastPreload.pause;
		if (!changed)
			return;
		const uint64_t longestUs = m_stats.longestTickUs;
		m_stats.longestTickUs = 0;
		m_lastLogged = m_stats;
		m_lastPreload = { preload.queued, preload.done, m_pause };

		char line[512];
		snprintf(line, sizeof(line),
			"[Images] requests=%u published=%llu evicted=%llu failed=%llu noimage=%llu kept=%u (%.1f MB) longest pump tick=%.3f ms"
			" preload queued=%u done=%u paused=%s"
			" cache: from disk=%llu from game=%llu written=%llu rejected=%llu",
			m_library.ImageCount(),
			static_cast<unsigned long long>(m_stats.published), static_cast<unsigned long long>(m_stats.evicted),
			static_cast<unsigned long long>(m_stats.failed), static_cast<unsigned long long>(m_stats.noImage),
			m_library.CountKept(), static_cast<double>(m_library.BytesKept()) / (1024.0 * 1024.0),
			static_cast<double>(longestUs) / 1000.0,
			preload.queued, preload.done, PauseText(static_cast<int>(m_pause)),
			static_cast<unsigned long long>(m_stats.fromDisk), static_cast<unsigned long long>(m_stats.fromGame),
			static_cast<unsigned long long>(m_stats.written), static_cast<unsigned long long>(m_stats.rejected));
		m_host.Log(line);
	}
}
