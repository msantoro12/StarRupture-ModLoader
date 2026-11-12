#pragma once

// The image library's table: which game images plugins have asked for, their
// state, what is kept in memory and in what order the rest are read.
//
// No engine and no GPU here. The pump that reads images, and the code that
// turns an item name into a texture path, call the members marked "pump" below
// and hand back what they made. That keeps every rule here testable with fakes.
//
// Threads. Item, Path, Get, State, UseSets and ForgetPlugin can be called from
// any thread. Get and State take no lock: they read per-image atomics from a
// table that never moves. Everything the pump calls takes the table's mutex,
// which no reader waits on.

#include "plugins/plugin_interface.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ImageLibrary
{
	// ---- Constants that the in-game trial settles -------------------------
	// The values here are the plan's starting picks.

	// The one size an image is stored at, in pixels.
	inline constexpr uint32_t kStoredSize = 256;

	// The most memory the stored copies may take, and the share of the video
	// memory budget that may be spent on them if that is smaller.
	inline constexpr uint64_t kByteCapMax          = 256ull * 1024 * 1024;
	inline constexpr uint64_t kByteCapBudgetPercent = 4;

	// The most images kept at once. The texture table the copies go into holds
	// 4096 slots for every plugin together.
	inline constexpr uint32_t kCountCap = 2048;

	// How long after it was last drawn (Get) or asked for an image counts as
	// seen lately, in milliseconds. A request that has not been seen lately is
	// dropped, unless the image belongs to a set a plugin uses.
	inline constexpr uint64_t kSeenLatelyMs = 5000;

	// ---- Table geometry ---------------------------------------------------
	// Ids index chunks of images that are allocated once and never move, so a
	// reader finds an image without a lock. A plugin that asks for more
	// different images than this gets id 0.
	inline constexpr uint32_t kIdsPerChunk = 512;
	inline constexpr uint32_t kMaxChunks   = 128;
	inline constexpr uint32_t kMaxIds      = kIdsPerChunk * kMaxChunks;

	// min(kByteCapMax, a share of the adapter's local video memory budget). A
	// budget of 0 (unknown) gives kByteCapMax.
	uint64_t ByteCapForBudget(uint64_t localBudgetBytes);

	// The key an item name is looked up under: trimmed, ASCII lower case.
	// Empty when there is nothing to look up.
	std::string NormalizeItemName(const char* name);

	// The key a texture object path is looked up under. Accepts the object path
	// ("/Game/UI/T_X.T_X"), the package path alone ("/Game/UI/T_X"), and the
	// reference form ("Texture2D'/Game/UI/T_X.T_X'"); the package path is
	// completed to an object path, and the case is folded. Empty when it is not
	// a path.
	std::string NormalizeObjectPath(const char* path);

	enum class Kind : uint8_t { Item, Path };

	// What the pump is handed to read.
	struct Request
	{
		PluginImageId id   = 0;
		Kind          kind = Kind::Item;
		std::string   name;   // as first asked for, trimmed, case kept
	};

	// A stored copy that left the table. The pump frees the texture.
	struct Released
	{
		PluginImageId       id     = 0;
		PluginTextureHandle handle = nullptr;
		uint64_t            bytes  = 0;
	};

	class Library
	{
	public:
		Library();
		~Library();
		Library(const Library&) = delete;
		Library& operator=(const Library&) = delete;

		// ---- Plugin side ---------------------------------------------------

		// Any thread. The same name or path always gives the same id. Asking
		// is a request: the image is read, unless nobody draws it. 0 for an
		// empty name or when the table is full.
		PluginImageId Item(const char* uniqueItemName);
		PluginImageId Path(const char* textureObjectPath);

		// Any thread, no lock. Marks the image as seen, and asks for it again
		// if it is not kept (evicted, or its request was dropped).
		PluginTextureHandle Get(PluginImageId id);

		// Any thread, no lock. A PluginImageState; does not mark the image seen.
		int State(PluginImageId id) const;

		// Any thread. The sets this plugin draws from; 0 clears.
		void UseSets(const IPluginSelf* self, uint32_t sets);

		// The plugin manager, before the plugin's module is freed.
		void ForgetPlugin(const IPluginSelf* self);

		// ---- Pump side -----------------------------------------------------

		// The time stamps of Get and of new requests are taken from this, in
		// milliseconds. Once per tick, before TakeNext.
		void SetNow(uint64_t nowMs);

		// The next image to read, or false. The most recently seen first;
		// requests not seen lately and outside every used set are dropped on
		// the way. The image stays WAITING until one of the four calls below.
		bool TakeNext(Request& out);

		// The read finished. Takes the copy and its size into the table, then
		// evicts the images seen least recently until the caps hold again;
		// those are appended to evicted for the pump to free. Returns whether
		// the image is READY now. False means the pump frees handle itself:
		// the copy alone is over the byte cap, or the image was no longer
		// waiting for it.
		bool Publish(PluginImageId id, PluginTextureHandle handle, uint64_t bytes,
			std::vector<Released>& evicted);

		// The image resolved and has no image to show.
		void PublishNoImage(PluginImageId id);

		// The read failed for good. The image stays NONE until the session ends.
		void Fail(PluginImageId id);

		// The read cannot be done now (not yet resident, the pump is busy).
		// The image goes back in the queue, still WAITING.
		void Requeue(PluginImageId id);

		// The image whose texture path the item lookup found is the same game
		// image as the one Path(thatPath) names: from then on both ids are
		// that one image, and it is read and kept once. Refused (false) when
		// from has been read already or is being read.
		bool Alias(PluginImageId from, PluginImageId to);

		// Adds the image to the sets (PluginImageSet bits). Requests for an
		// image in a set some plugin uses are not dropped for being unseen.
		void AddToSets(PluginImageId id, uint32_t sets);

		// ---- Limits and accounting ----------------------------------------

		void SetCaps(uint64_t byteCap, uint32_t countCap);
		uint64_t BytesKept() const;
		uint32_t CountKept() const;
		uint32_t ImageCount() const;   // ids handed out

	private:
		enum : int32_t
		{
			kIdle    = 0,   // not kept, not asked for: Get asks
			kWaiting = 1,
			kReady   = 2,
			kNoImage = 3,
			kFailed  = 4,   // reads as NONE, and Get does not ask again
		};

		struct Entry
		{
			// Read without the lock.
			std::atomic<int32_t>             state{ kIdle };
			std::atomic<PluginTextureHandle> handle{ nullptr };
			std::atomic<uint64_t>            lastSeen{ 0 };
			std::atomic<PluginImageId>       target{ 0 };   // the image this id is an alias of

			// Under the lock.
			Kind        kind = Kind::Item;
			std::string name;
			uint32_t    sets   = 0;
			uint64_t    bytes  = 0;
			bool        queued = false;
			bool        taken  = false;   // handed to the pump, not answered yet
		};

		struct Chunk
		{
			Entry entries[kIdsPerChunk];
		};

		Entry*       Find(PluginImageId id);
		const Entry* Find(PluginImageId id) const;
		Entry*       Resolve(Entry* e);
		const Entry* Resolve(const Entry* e) const;
		PluginImageId IdFor(Kind kind, const std::string& key, const char* name);
		void  Ask(Entry& e, PluginImageId id);   // lock held
		void  Drain();                           // lock held
		void  Trim(std::vector<Released>& evicted);   // lock held
		bool  InSetUse(const Entry& e) const;    // lock held

		std::atomic<Chunk*>               m_chunks[kMaxChunks];
		std::atomic<uint32_t>             m_count{ 0 };
		std::atomic<uint64_t>             m_now{ 0 };
		std::atomic<bool>                 m_askPending{ false };

		mutable std::mutex                m_lock;
		std::unordered_map<std::string, PluginImageId> m_ids;
		std::vector<PluginImageId>        m_queue;
		std::unordered_map<const IPluginSelf*, uint32_t> m_used;
		uint64_t                          m_byteCap  = kByteCapMax;
		uint32_t                          m_countCap = kCountCap;
		uint64_t                          m_bytes    = 0;
		uint32_t                          m_kept     = 0;
	};

	// The loader's table. Handed to plugins as hooks->Images.
	Library& Instance();
	IPluginImages* GetInterface();

	// Drop what a plugin's set interest recorded. Plugin manager, before FreeLibrary.
	void ForgetPlugin(const IPluginSelf* self);
}
