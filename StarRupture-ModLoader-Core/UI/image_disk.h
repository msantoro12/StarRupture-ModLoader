#pragma once

// Saves the image library's stored copies to disk. The pump tells the saver when a shrink
// has started; the shrink's command list also copied the finished pixels into a readback
// buffer, and the saver polls for them each tick. A finished copy becomes a job for the
// worker (image_cache_writer.h), which writes the file. The list of images in each set is
// written the same way when it changes.
//
// This is the write side only. Reading the files back at launch is the next stage; it starts
// from Host::CacheFolder (image_disk_host.cpp) and the parse functions of image_cache_file.h.
//
// Game thread only. No file I/O here: everything that touches the disk is a Job.

#include "UI/image_cache_file.h"
#include "UI/image_cache_writer.h"
#include "UI/image_library.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ImageDisk
{
	// Milliseconds between looks at the sets. A change is written at most this often.
	inline constexpr uint64_t kSetListIntervalMs = 10 * 1000;

	// What the pump knows when a shrink has started.
	struct Started
	{
		std::string path;                 // the object path the image was asked for
		uint32_t    sourceFormat = 0;     // DXGI_FORMAT of the game image
		uint32_t    sourceWidth  = 0;
		uint32_t    sourceHeight = 0;
		uint32_t    storedWidth  = 0;
		uint32_t    storedHeight = 0;
		uint32_t    levels       = 0;
		uint32_t    ticket       = 0;     // the readback of the stored copy; 0 when none was made
	};

	// What the pump calls. The pump holds one, or none.
	class Saver
	{
	public:
		virtual ~Saver() = default;
		virtual void Begin(const Started& started) = 0;
		// Once per tick after the pump has collected its reads.
		virtual void Poll(uint64_t nowMs) = 0;
		// The GPU side is gone: the readbacks will never finish.
		virtual void Abandon() = 0;
		// The engine is shutting down: stops the worker. Nothing is saved after this.
		virtual void Stop() = 0;
	};

	enum class Readback { Pending, Done, Gone };

	// What the saver needs from the engine and the GPU.
	class Host
	{
	public:
		virtual ~Host() = default;
		// Takes the pixels of a readback into dest (bytes long), or only frees the readback
		// when dest is null. Done and Gone free it; Pending leaves it.
		virtual Readback Take(uint32_t ticket, uint8_t* dest, size_t bytes) = 0;
		// The folder the files of the current game build and pak set go in. The pak set is read now;
		// the returned function is called by the worker, which is where the exe is read.
		virtual std::function<std::wstring()> CacheFolder() = 0;
	};

	class DiskSaver final : public Saver
	{
	public:
		DiskSaver(ImageLibrary::Library& library, Host& host, ImageCache::Sink& sink);

		void Begin(const Started& started) override;
		void Poll(uint64_t nowMs) override;
		void Abandon() override;
		void Stop() override;

	private:
		void Finish(const Started& started, std::vector<uint8_t>&& pixels);
		void WriteSetList();

		ImageLibrary::Library& m_library;
		Host&                  m_host;
		ImageCache::Sink&      m_sink;

		std::vector<Started>   m_pending;
		ImageCache::SetList    m_lastSets;
		uint64_t               m_nextSetCheckMs = 0;
		bool                   m_gone    = false;
		bool                   m_stopped = false;
	};

	// The loader's saver, on the loader's worker. Client builds only (image_disk_host.cpp).
	Saver* EngineSaver();
}
