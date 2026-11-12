#include "UI/image_library.h"

#include <algorithm>
#include <cstring>

namespace ImageLibrary
{
	namespace
	{
		constexpr size_t   kMaxNameLength = 256;
		constexpr size_t   kMaxPathLength = 1024;
		constexpr uint32_t kKnownSets     = PLUGIN_IMAGE_SET_ITEMS | PLUGIN_IMAGE_SET_BUILDINGS;

		bool IsSpace(char c)
		{
			return c == ' ' || c == '\t' || c == '\r' || c == '\n';
		}

		char Lower(char c)
		{
			return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
		}

		std::string Trimmed(const char* s)
		{
			if (!s)
				return {};
			size_t begin = 0;
			size_t end = strlen(s);
			while (begin < end && IsSpace(s[begin]))
				++begin;
			while (end > begin && IsSpace(s[end - 1]))
				--end;
			return std::string(s + begin, end - begin);
		}

		bool HasControlChars(const std::string& s)
		{
			return std::any_of(s.begin(), s.end(), [](char c) { return static_cast<unsigned char>(c) < 0x20; });
		}

		void FoldCase(std::string& s)
		{
			for (char& c : s)
				c = Lower(c);
		}

		std::string CanonicalName(const char* name)
		{
			std::string s = Trimmed(name);
			if (s.size() > kMaxNameLength || HasControlChars(s))
				return {};
			return s;
		}

		// "/Game/UI/T_X" and "Texture2D'/Game/UI/T_X.T_X'" both become
		// "/Game/UI/T_X.T_X". Case is kept.
		std::string CanonicalPath(const char* path)
		{
			std::string s = Trimmed(path);

			const size_t open = s.find('\'');
			if (open != std::string::npos)
			{
				const size_t close = s.rfind('\'');
				if (close <= open)
					return {};
				s = s.substr(open + 1, close - open - 1);
			}

			if (s.size() < 2 || s.size() > kMaxPathLength || s[0] != '/' || HasControlChars(s))
				return {};

			const size_t slash = s.rfind('/');
			if (slash + 1 >= s.size())
				return {};
			if (s.find('.', slash) == std::string::npos)
				s += '.' + s.substr(slash + 1);
			return s;
		}

		int PublicState(int32_t state)
		{
			switch (state)
			{
			case 1:  return PLUGIN_IMAGE_WAITING;
			case 2:  return PLUGIN_IMAGE_READY;
			case 3:  return PLUGIN_IMAGE_NO_IMAGE;
			default: return PLUGIN_IMAGE_NONE;
			}
		}
	}

	uint64_t ByteCapForBudget(uint64_t localBudgetBytes)
	{
		if (localBudgetBytes == 0)
			return kByteCapMax;
		return (std::min)(kByteCapMax, localBudgetBytes / 100 * kByteCapBudgetPercent);
	}

	std::string NormalizeItemName(const char* name)
	{
		std::string s = CanonicalName(name);
		FoldCase(s);
		return s;
	}

	std::string NormalizeObjectPath(const char* path)
	{
		std::string s = CanonicalPath(path);
		FoldCase(s);
		return s;
	}

	Library::Library()
	{
		for (auto& chunk : m_chunks)
			chunk.store(nullptr, std::memory_order_relaxed);
	}

	Library::~Library()
	{
		for (auto& chunk : m_chunks)
			delete chunk.load(std::memory_order_relaxed);
	}

	Library::Entry* Library::Find(PluginImageId id)
	{
		if (id == 0 || id > m_count.load(std::memory_order_acquire))
			return nullptr;
		const uint32_t index = id - 1;
		Chunk* chunk = m_chunks[index / kIdsPerChunk].load(std::memory_order_acquire);
		return chunk ? &chunk->entries[index % kIdsPerChunk] : nullptr;
	}

	const Library::Entry* Library::Find(PluginImageId id) const
	{
		return const_cast<Library*>(this)->Find(id);
	}

	Library::Entry* Library::Resolve(Entry* e)
	{
		const PluginImageId target = e->target.load(std::memory_order_acquire);
		return target ? Find(target) : e;
	}

	const Library::Entry* Library::Resolve(const Entry* e) const
	{
		return const_cast<Library*>(this)->Resolve(const_cast<Entry*>(e));
	}

	PluginImageId Library::IdFor(Kind kind, const std::string& key, const char* name)
	{
		if (key.empty())
			return 0;

		std::lock_guard<std::mutex> lock(m_lock);

		const std::string mapKey = (kind == Kind::Item ? "i:" : "p:") + key;
		const auto found = m_ids.find(mapKey);
		if (found != m_ids.end())
			return found->second;

		const uint32_t index = m_count.load(std::memory_order_relaxed);
		if (index >= kMaxIds)
			return 0;
		Chunk* chunk = m_chunks[index / kIdsPerChunk].load(std::memory_order_relaxed);
		if (!chunk)
		{
			chunk = new Chunk();
			m_chunks[index / kIdsPerChunk].store(chunk, std::memory_order_release);
		}

		const PluginImageId id = index + 1;
		Entry& e = chunk->entries[index % kIdsPerChunk];
		e.kind = kind;
		e.name = kind == Kind::Item ? CanonicalName(name) : CanonicalPath(name);
		e.lastSeen.store(m_now.load(std::memory_order_relaxed), std::memory_order_relaxed);
		e.state.store(kWaiting, std::memory_order_relaxed);
		e.queued = true;
		m_queue.push_back(id);
		m_ids.emplace(mapKey, id);
		m_count.store(id, std::memory_order_release);
		return id;
	}

	PluginImageId Library::Item(const char* uniqueItemName)
	{
		return IdFor(Kind::Item, NormalizeItemName(uniqueItemName), uniqueItemName);
	}

	PluginImageId Library::Path(const char* textureObjectPath)
	{
		return IdFor(Kind::Path, NormalizeObjectPath(textureObjectPath), textureObjectPath);
	}

	PluginTextureHandle Library::Get(PluginImageId id)
	{
		Entry* e = Find(id);
		if (!e)
			return nullptr;
		e = Resolve(e);
		if (!e)
			return nullptr;

		e->lastSeen.store(m_now.load(std::memory_order_relaxed), std::memory_order_relaxed);

		int32_t state = e->state.load(std::memory_order_acquire);
		if (state == kReady)
			return e->handle.load(std::memory_order_acquire);
		if (state == kIdle && e->state.compare_exchange_strong(state, kWaiting, std::memory_order_acq_rel))
			m_askPending.store(true, std::memory_order_release);
		return nullptr;
	}

	int Library::State(PluginImageId id) const
	{
		const Entry* e = Find(id);
		if (!e)
			return PLUGIN_IMAGE_NONE;
		e = Resolve(e);
		return e ? PublicState(e->state.load(std::memory_order_acquire)) : PLUGIN_IMAGE_NONE;
	}

	void Library::UseSets(const IPluginSelf* self, uint32_t sets)
	{
		if (!self)
			return;
		sets &= kKnownSets;
		std::lock_guard<std::mutex> lock(m_lock);
		if (sets == 0)
			m_used.erase(self);
		else
			m_used[self] = sets;
	}

	void Library::ForgetPlugin(const IPluginSelf* self)
	{
		std::lock_guard<std::mutex> lock(m_lock);
		m_used.erase(self);
	}

	void Library::SetNow(uint64_t nowMs)
	{
		m_now.store(nowMs, std::memory_order_relaxed);
	}

	bool Library::InSetUse(const Entry& e) const
	{
		uint32_t used = 0;
		for (const auto& use : m_used)
			used |= use.second;
		return (e.sets & used) != 0;
	}

	void Library::Ask(Entry& e, PluginImageId id)
	{
		int32_t idle = kIdle;
		if (e.state.compare_exchange_strong(idle, kWaiting, std::memory_order_acq_rel) && !e.queued)
		{
			e.queued = true;
			m_queue.push_back(id);
		}
	}

	// Images that Get found not kept and moved to WAITING without the lock.
	void Library::Drain()
	{
		if (!m_askPending.exchange(false, std::memory_order_acq_rel))
			return;
		const uint32_t count = m_count.load(std::memory_order_relaxed);
		for (PluginImageId id = 1; id <= count; ++id)
		{
			Entry* e = Find(id);
			if (e->target.load(std::memory_order_relaxed) == 0
				&& e->state.load(std::memory_order_acquire) == kWaiting && !e->queued && !e->taken)
			{
				e->queued = true;
				m_queue.push_back(id);
			}
		}
	}

	bool Library::TakeNext(Request& out)
	{
		std::lock_guard<std::mutex> lock(m_lock);
		Drain();

		const uint64_t now = m_now.load(std::memory_order_relaxed);
		struct Best { size_t slot; bool lately; uint64_t seen; PluginImageId id; };
		bool haveBest = false;
		Best best{};

		for (size_t i = 0; i < m_queue.size();)
		{
			const PluginImageId id = m_queue[i];
			Entry* e = Find(id);
			const uint64_t seen = e->lastSeen.load(std::memory_order_relaxed);
			const bool lately = seen >= now || now - seen <= kSeenLatelyMs;

			if (!lately && !InSetUse(*e))
			{
				e->queued = false;
				e->state.store(kIdle, std::memory_order_release);
				m_queue[i] = m_queue.back();
				m_queue.pop_back();
				continue;
			}

			bool better;
			if (!haveBest)
				better = true;
			else if (lately != best.lately)
				better = lately;
			else if (seen != best.seen)
				better = seen > best.seen;
			else
				better = id < best.id;
			if (better)
			{
				best = { i, lately, seen, id };
				haveBest = true;
			}
			++i;
		}

		if (!haveBest)
			return false;

		Entry* e = Find(best.id);
		m_queue[best.slot] = m_queue.back();
		m_queue.pop_back();
		e->queued = false;
		e->taken = true;
		out.id = best.id;
		out.kind = e->kind;
		out.name = e->name;
		return true;
	}

	bool Library::Publish(PluginImageId id, PluginTextureHandle handle, uint64_t bytes,
		std::vector<Released>& evicted)
	{
		std::lock_guard<std::mutex> lock(m_lock);
		Entry* e = Find(id);
		if (!e || !e->taken)
			return false;
		e->taken = false;

		if (!handle || bytes > m_byteCap)
		{
			e->state.store(kFailed, std::memory_order_release);
			return false;
		}

		e->bytes = bytes;
		e->handle.store(handle, std::memory_order_relaxed);
		e->state.store(kReady, std::memory_order_release);
		m_bytes += bytes;
		++m_kept;

		Trim(evicted);
		return e->state.load(std::memory_order_relaxed) == kReady;
	}

	void Library::Trim(std::vector<Released>& evicted)
	{
		if (m_bytes <= m_byteCap && m_kept <= m_countCap)
			return;

		struct Kept { uint64_t seen; PluginImageId id; };
		std::vector<Kept> kept;
		const uint32_t count = m_count.load(std::memory_order_relaxed);
		for (PluginImageId id = 1; id <= count; ++id)
		{
			const Entry* e = Find(id);
			if (e->state.load(std::memory_order_relaxed) == kReady)
				kept.push_back({ e->lastSeen.load(std::memory_order_relaxed), id });
		}
		std::sort(kept.begin(), kept.end(), [](const Kept& a, const Kept& b)
		{
			return a.seen != b.seen ? a.seen < b.seen : a.id < b.id;
		});

		for (const Kept& k : kept)
		{
			if (m_bytes <= m_byteCap && m_kept <= m_countCap)
				break;
			Entry* e = Find(k.id);
			evicted.push_back({ k.id, e->handle.load(std::memory_order_relaxed), e->bytes });
			m_bytes -= e->bytes;
			--m_kept;
			e->bytes = 0;
			e->handle.store(nullptr, std::memory_order_relaxed);
			e->state.store(kIdle, std::memory_order_release);
		}
	}

	void Library::PublishNoImage(PluginImageId id)
	{
		std::lock_guard<std::mutex> lock(m_lock);
		Entry* e = Find(id);
		if (!e || !e->taken)
			return;
		e->taken = false;
		e->state.store(kNoImage, std::memory_order_release);
	}

	void Library::Fail(PluginImageId id)
	{
		std::lock_guard<std::mutex> lock(m_lock);
		Entry* e = Find(id);
		if (!e || !e->taken)
			return;
		e->taken = false;
		e->state.store(kFailed, std::memory_order_release);
	}

	void Library::Requeue(PluginImageId id)
	{
		std::lock_guard<std::mutex> lock(m_lock);
		Entry* e = Find(id);
		if (!e || !e->taken)
			return;
		e->taken = false;
		e->queued = true;
		m_queue.push_back(id);
	}

	bool Library::Alias(PluginImageId from, PluginImageId to)
	{
		std::lock_guard<std::mutex> lock(m_lock);
		Entry* source = Find(from);
		Entry* dest = Find(to);
		if (!source || !dest)
			return false;
		const PluginImageId forwarded = dest->target.load(std::memory_order_relaxed);
		const PluginImageId destId = forwarded ? forwarded : to;
		dest = Find(destId);
		if (!dest || destId == from || source->target.load(std::memory_order_relaxed) != 0)
			return false;
		if (source->taken || source->state.load(std::memory_order_relaxed) == kReady)
			return false;

		if (source->queued)
		{
			m_queue.erase(std::find(m_queue.begin(), m_queue.end(), from));
			source->queued = false;
		}
		dest->sets |= source->sets;
		const uint64_t seen = source->lastSeen.load(std::memory_order_relaxed);
		if (seen > dest->lastSeen.load(std::memory_order_relaxed))
			dest->lastSeen.store(seen, std::memory_order_relaxed);
		const bool wanted = source->state.load(std::memory_order_relaxed) == kWaiting;
		source->target.store(destId, std::memory_order_release);
		if (wanted)
			Ask(*dest, destId);
		return true;
	}

	void Library::AddToSets(PluginImageId id, uint32_t sets)
	{
		std::lock_guard<std::mutex> lock(m_lock);
		Entry* e = Find(id);
		if (e && (e = Resolve(e)) != nullptr)
			e->sets |= sets & kKnownSets;
	}

	void Library::SetCaps(uint64_t byteCap, uint32_t countCap)
	{
		std::lock_guard<std::mutex> lock(m_lock);
		m_byteCap = byteCap;
		m_countCap = countCap;
	}

	uint64_t Library::BytesKept() const
	{
		std::lock_guard<std::mutex> lock(m_lock);
		return m_bytes;
	}

	uint32_t Library::CountKept() const
	{
		std::lock_guard<std::mutex> lock(m_lock);
		return m_kept;
	}

	uint32_t Library::ImageCount() const
	{
		return m_count.load(std::memory_order_acquire);
	}

	// ---- The table handed to plugins --------------------------------------

	Library& Instance()
	{
		// Never destroyed: a render callback can still be reading at exit.
		static Library* const library = new Library();
		return *library;
	}

	namespace
	{
		PluginImageId ItemThunk(const IPluginSelf*, const char* name)  { return Instance().Item(name); }
		PluginImageId PathThunk(const IPluginSelf*, const char* path)  { return Instance().Path(path); }
		PluginTextureHandle GetThunk(PluginImageId id)                 { return Instance().Get(id); }
		int StateThunk(PluginImageId id)                               { return Instance().State(id); }
		void UseSetsThunk(const IPluginSelf* self, uint32_t sets)      { Instance().UseSets(self, sets); }

		// A texture is named by its object path, which needs the engine to read.
		PluginImageId FromTextureThunk(const IPluginSelf*, SDK::UTexture2D*) { return 0; }
	}

	IPluginImages* GetInterface()
	{
		static IPluginImages table = {
			ItemThunk,
			PathThunk,
			FromTextureThunk,
			GetThunk,
			StateThunk,
			UseSetsThunk,
		};
		return &table;
	}

	void ForgetPlugin(const IPluginSelf* self)
	{
		Instance().ForgetPlugin(self);
	}
}
