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
// Game thread only. The pump keeps no pointer into the engine: a texture is
// found, read and forgotten inside one Host::Read call.

#include "UI/image_library.h"

#include <cstdint>
#include <string>
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

	// What a started read made: a texture slot of the texture API, holding one
	// level of the game image.
	struct Copy
	{
		PluginTextureHandle texture = nullptr;
		int                 width   = 0;
		int                 height  = 0;
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

		// Finds the texture at an object path and starts reading one level of it
		// at the stored size. Never waits.
		virtual ReadResult Read(const std::string& objectPath, Copy& out) = 0;
		// Loads the texture at an object path into memory, and waits for it. The
		// one step an asynchronous load would replace: Read finds the result.
		virtual LoadResult Load(const std::string& objectPath) = 0;

		virtual CopyState StateOf(PluginTextureHandle texture) = 0;
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
		Pump(ImageLibrary::Library& library, Host& host);

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
		};

		// What a read becomes before it is published.
		struct Stored
		{
			PluginTextureHandle texture = nullptr;
			uint64_t            bytes   = 0;
		};

		enum class Outcome { Started, Done, Later, Busy, Off };

		void    Start();
		void    GpuGone();
		void    Collect();
		void    Serve();
		void    FailQueued();
		Outcome Attempt(const ImageLibrary::Request& request, uint32_t& loads);
		bool    Store(const InFlight& read, Stored& out);
		void    FreeDeferred();
		void    LogSummary();

		ImageLibrary::Library& m_library;
		Host&                  m_host;

		bool m_started = false;   // the GPU side was up at least once
		bool m_off     = false;   // no read can run any more this session
		bool m_gone    = false;   // the GPU side went away: nothing may be freed
		bool m_stopped = false;

		std::vector<InFlight>                 m_inFlight;
		std::vector<ImageLibrary::Released>   m_scratch;
		std::vector<PluginImageId>            m_later;
		// Evicted copies, freed at the start of the next tick: the frame that
		// may still draw them has ended by then.
		std::vector<PluginTextureHandle>      m_toFree;

		Stats    m_stats;
		Stats    m_lastLogged;
		uint64_t m_nextSummaryMs = 0;
	};

	// ---- The loader's pump ---------------------------------------------------

	// Engine tick, after the plugin callbacks. Game thread.
	void Tick();
	// Engine shutdown, after the plugin callbacks and before the overlay goes. Game thread.
	void Shutdown();
}
