#include "UI/image_pump.h"
#include "UI/image_shrink.h"

#include <algorithm>
#include <cstdio>

namespace ImagePump
{
	Pump::Pump(ImageLibrary::Library& library, Host& host, ImageLookup::ItemSource& items)
		: m_library(library), m_host(host), m_items(items)
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
			Collect();
		if (m_off)
			FailQueued();
		else
		{
			Serve();
			StepItems();
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

		size_t dropped = m_inFlight.size();
		for (const InFlight& read : m_inFlight)
			m_library.Fail(read.id);
		m_stats.failed += m_inFlight.size();
		m_inFlight.clear();

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

			m_scratch.clear();
			const bool kept = m_library.Publish(read.id, stored.texture, stored.bytes, m_scratch);
			if (kept)
				++m_stats.published;
			else
			{
				// Refused, or evicted again at once (then it is among the evicted).
				const bool evictedAgain = std::any_of(m_scratch.begin(), m_scratch.end(),
					[&](const ImageLibrary::Released& r) { return r.handle == stored.texture; });
				if (!evictedAgain)
				{
					m_host.Free(stored.texture);
					++m_stats.failed;
				}
			}
			for (const ImageLibrary::Released& released : m_scratch)
				m_toFree.push_back(released.handle);
			m_stats.evicted += m_scratch.size();
		}
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
		out.texture = shrunk.texture;
		out.bytes   = ImageShrink::Bytes({ static_cast<uint32_t>(shrunk.width), static_cast<uint32_t>(shrunk.height) }, shrunk.levels);
		return StoreResult::Stored;
	}

	void Pump::Serve()
	{
		uint32_t reads = 0, loads = 0, attempts = 0;
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
	}

	// Reads the next slice of the item index while an item is waiting, and
	// answers the waiting items it now knows.
	void Pump::StepItems()
	{
		if (m_held.empty())
			return;

		ResolveHeld();
		if (m_held.empty())
			return;

		const ImageLookup::ItemIndex::State before = m_items.GetState();
		m_items.Step(ImageLookup::kItemBudgetUs, [this] { return m_host.NowUs(); });
		if (before != ImageLookup::ItemIndex::State::Failed && m_items.GetState() == ImageLookup::ItemIndex::State::Failed)
			m_host.Log("[Images] No item can be read on this build or in this session: item images stay unavailable");
		ResolveHeld();
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
		return true;
	}

	Pump::Outcome Pump::Attempt(const ImageLibrary::Request& request, uint32_t& loads)
	{
		if (ImageLookup::IsPlaceholderImage(request.name.c_str()))
		{
			m_library.PublishNoImage(request.id);
			++m_stats.noImage;
			return Outcome::Done;
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
			m_inFlight.push_back({ request.id, copy });
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
		const bool changed = m_stats.published != m_lastLogged.published || m_stats.evicted != m_lastLogged.evicted
			|| m_stats.failed != m_lastLogged.failed || m_stats.noImage != m_lastLogged.noImage;
		if (!changed)
			return;
		const uint64_t longestUs = m_stats.longestTickUs;
		m_stats.longestTickUs = 0;
		m_lastLogged = m_stats;

		char line[256];
		snprintf(line, sizeof(line),
			"[Images] requests=%u published=%llu evicted=%llu failed=%llu noimage=%llu kept=%u (%.1f MB) longest pump tick=%.3f ms",
			m_library.ImageCount(),
			static_cast<unsigned long long>(m_stats.published), static_cast<unsigned long long>(m_stats.evicted),
			static_cast<unsigned long long>(m_stats.failed), static_cast<unsigned long long>(m_stats.noImage),
			m_library.CountKept(), static_cast<double>(m_library.BytesKept()) / (1024.0 * 1024.0),
			static_cast<double>(longestUs) / 1000.0);
		m_host.Log(line);
	}
}
