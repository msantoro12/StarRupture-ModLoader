#pragma once

// The image library's pump: once per engine tick it takes the next request
// from the table, finds or loads the texture, starts the read, and publishes
// the copy when the GPU has finished it.
//
// Everything that touches the engine or the GPU is behind Host, so the rules
// here (the budget per tick, what each read result does, eviction, device loss,
// shutdown) are testable with fakes. The loader's own Host is in
// image_pump_host.cpp.
//
// An item request waits in the pump until the item index knows its icon, and
// then becomes the image of the icon's texture path (Library::Alias).
//
// The sets (items, buildings): when a plugin uses a set and the loader's
// preload setting is on, the pump reads the set's list from the game, a slice
// per tick, and registers its images with the library. Preload then reads them
// in the background under the rules in the constants below.
//
// Game thread only. The pump keeps no pointer into the engine: a texture is
// found, read and forgotten inside one Host::Read call.

#include "UI/image_disk.h"
#include "UI/image_library.h"
#include "UI/image_lookup.h"

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace ImagePump
{
	// ---- Constants ---------------------------------------------------------

	// Per tick. The trial found a read costing over 1 ms on the game thread at
	// p95, so one image goes per tick. Package loads are synchronous, and one
	// per tick as well.
	inline constexpr uint32_t kReadsPerTick    = 1;
	inline constexpr uint32_t kLoadsPerTick    = 1;
	// Requests looked at per tick, whatever came of them. Keeps a queue of
	// images that are not readable yet from costing a tick.
	inline constexpr uint32_t kAttemptsPerTick = 8;

	inline constexpr uint64_t kSummaryIntervalMs = 60 * 1000;

	// ---- Preload -----------------------------------------------------------
	// Preload reads the members of the sets in use in the background. It runs
	// only while every rule below holds, and only on a tick that has no
	// on-demand read to make.

	// Preload is tried on one tick in this many: half the on-demand pace, since
	// the trial found even one read per tick over 1 ms on the game thread.
	inline constexpr uint32_t kPreloadTickInterval = 2;

	// The game releases the images the library made it load in sweeps, 44 to 50
	// seconds after the load in the trial, and some it kept for the whole run of
	// five minutes. The full-size images the library caused and the game has not
	// let go of, taken to be those loaded within the window, may not pass the
	// limit. An image counts at 4 bytes a pixel of its full size, which is more
	// than the game's compressed copy takes.
	inline constexpr uint64_t kPreloadHeldWindowMs   = 120 * 1000;
	inline constexpr uint64_t kPreloadHeldLimitBytes = 64ull * 1024 * 1024;

	// Preload waits while the adapter's local video memory in use is at or over
	// this share of its budget, checked at most once in this interval.
	inline constexpr uint32_t kPreloadVideoPercent  = 90;
	inline constexpr uint64_t kVideoCheckIntervalMs = 1000;

	// ---- What the pump needs from the engine and the GPU ---------------------

	enum class ReadResult
	{
		Started,       // the copy is on the GPU; the Copy is filled in
		NotYet,        // loaded, but not readable yet: ask again on a later tick
		Busy,          // the read path is full: ask again, and not again this tick
		Failed,        // this texture cannot be copied
		Unavailable,   // no read can run on this build or this session
		NotLoaded,     // no texture at that path is in memory
	};

	enum class LoadResult
	{
		Loaded,        // a texture is in memory now
		Missing,       // there is no texture at that path
		Fault,         // the load faulted
		Unsupported,   // this build cannot load by path
	};

	enum class CopyState { Pending, Done, Gone };

	enum class ShrinkResult
	{
		Started,   // the shrink is on the GPU; the source may be freed now
		Busy,      // no room to start it this tick: ask again
		Failed,    // the shrink cannot be made
	};

	// What a started shrink made: a texture slot of the texture API, at the stored
	// size, with the full chain of smaller levels.
	struct Shrunk
	{
		PluginTextureHandle texture  = nullptr;
		int                 width    = 0;
		int                 height   = 0;
		uint32_t            levels   = 0;
		uint32_t            readback = 0;   // 0: the stored copy is not read back
	};

	// What a started read made: a texture slot of the texture API, holding one
	// level of the game image.
	struct Copy
	{
		PluginTextureHandle texture = nullptr;
		int                 width   = 0;
		int                 height  = 0;
		int                 sourceWidth  = 0;   // level 0 of the game image
		int                 sourceHeight = 0;
		uint32_t            sourceFormat = 0;
	};

	class Host
	{
	public:
		virtual ~Host() = default;

		// The overlay's GPU side is up. False before the first frame, and again
		// once the device is lost or the overlay is shut down.
		virtual bool GpuReady() = 0;
		// The adapter's local video memory budget in bytes; 0 when unknown.
		virtual uint64_t LocalVideoBudget() = 0;
		// A steady clock, in microseconds.
		virtual uint64_t NowUs() = 0;
		// The loader setting for preloading images from the game. Read every tick,
		// so a change takes effect at once.
		virtual bool PreloadEnabled() = 0;
		// The adapter's local video memory in use and its budget, in bytes. False
		// when it cannot be read.
		virtual bool VideoMemory(uint64_t& usage, uint64_t& budget) = 0;

		// Finds the texture at an object path and starts reading one level of it
		// at the stored size. Never waits.
		virtual ReadResult Read(const std::string& objectPath, Copy& out) = 0;
		// Loads the texture at an object path into memory, and waits for it. The
		// one step an asynchronous load would replace: Read finds the result.
		virtual LoadResult Load(const std::string& objectPath) = 0;

		virtual CopyState StateOf(PluginTextureHandle texture) = 0;
		// Draws a finished copy into a new texture at the stored size, with every smaller
		// level. Never waits. The source is kept alive on the GPU until the shrink has
		// run, so the caller frees it right after Started.
		virtual ShrinkResult Shrink(PluginTextureHandle source, Shrunk& out) = 0;
		// Frees a texture. The texture API holds it until the GPU is past every
		// frame that may still draw it.
		virtual void Free(PluginTextureHandle texture) = 0;
		virtual void Log(const std::string& line) = 0;
	};

	struct Stats
	{
		uint64_t published = 0;
		uint64_t evicted   = 0;
		uint64_t failed    = 0;
		uint64_t noImage   = 0;
		uint64_t longestTickUs = 0;   // since the last summary line
	};

	class Pump
	{
	public:
		Pump(ImageLibrary::Library& library, Host& host, ImageLookup::ItemSource& items, ImageLookup::ItemSource& buildings);

		// Where the stored copies are saved. None by default; the pump does not own it.
		void SetSaver(ImageDisk::Saver* saver) { m_saver = saver; }

		// Once per engine tick, after the plugin callbacks.
		void Tick();

		// The engine is shutting down: every copy the library keeps is freed
		// while the texture API is still up. After this the pump does nothing.
		void Shutdown();

		const Stats& GetStats() const { return m_stats; }

	private:
		// A read that started and has not been published.
		struct InFlight
		{
			PluginImageId id = 0;
			Copy          copy;
			std::string   path;
		};

		// What a read becomes before it is published: the texture to keep and the bytes
		// its levels take.
		struct Stored
		{
			PluginTextureHandle texture = nullptr;
			uint64_t            bytes   = 0;
		};

		enum class StoreResult { Stored, Later, Failed };

		// An item request the index cannot answer yet.
		struct HeldItem
		{
			PluginImageId id = 0;
			std::string   key;
		};

		enum class Outcome { Started, Done, Later, Busy, Off };

		// Why preload did not read on the last tick it was tried.
		enum class Pause { Running, Off, NoSets, Listing, Screen, GameHolds, VideoMemory, Full };

		// A full-size game image the library made the game load.
		struct HeldLoad
		{
			uint64_t atMs  = 0;
			uint64_t bytes = 0;
		};

		void    Start();
		void    GpuGone();
		void    Collect();
		bool    Serve(uint32_t& loads);
		bool    StepItems();
		void    StepSets(uint32_t used, bool stepped);
		bool    StepSet(ImageLookup::ItemIndex& index, uint32_t set, bool& listed, bool stepped, const char* what);
		void    Preload(bool idle, bool stepped, uint32_t& loads);
		bool    GameHoldsTooMuch();
		bool    VideoFull();
		void    ResolveHeld();
		bool    Resolved(const HeldItem& item);
		void    FailQueued();
		void    FailHeld();
		Outcome Attempt(const ImageLibrary::Request& request, uint32_t& loads);
		StoreResult Store(const InFlight& read, Stored& out);
		void    FreeDeferred();
		void    LogSummary();

		ImageLibrary::Library& m_library;
		Host&                  m_host;
		ImageLookup::ItemIndex m_items;
		ImageLookup::ItemIndex m_buildings;
		ImageDisk::Saver*      m_saver = nullptr;

		bool m_started = false;   // the GPU side was up at least once
		bool m_off     = false;   // no read can run any more this session
		bool m_gone    = false;   // the GPU side went away: nothing may be freed
		bool m_stopped = false;

		std::vector<InFlight>                 m_inFlight;
		std::vector<ImageLibrary::Released>   m_scratch;
		std::vector<PluginImageId>            m_later;
		std::vector<HeldItem>                 m_held;
		// Preload.
		bool                                  m_itemsListed     = false;
		bool                                  m_buildingsListed = false;
		uint32_t                              m_preloadTick     = 0;
		Pause                                 m_pause           = Pause::Off;
		std::vector<HeldLoad>                 m_heldLoads;
		std::unordered_set<PluginImageId>     m_causedLoad;     // loaded by us, copy not started yet
		uint64_t                              m_nextVideoCheckMs = 0;
		bool                                  m_videoFull        = false;
		// Evicted copies, freed at the start of the next tick: the frame that
		// may still draw them has ended by then.
		std::vector<PluginTextureHandle>      m_toFree;

		struct PreloadLogged
		{
			uint32_t queued = 0;
			uint32_t done   = 0;
			Pause    pause  = Pause::Off;
		};

		Stats         m_stats;
		Stats         m_lastLogged;
		PreloadLogged m_lastPreload;
		uint64_t m_nextSummaryMs = 0;
	};

	// ---- The loader's pump ---------------------------------------------------

	// Engine tick, after the plugin callbacks. Game thread.
	void Tick();
	// Engine shutdown, after the plugin callbacks and before the overlay goes. Game thread.
	void Shutdown();
}
