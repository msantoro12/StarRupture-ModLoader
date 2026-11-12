#pragma once

// The size and cost of the image library's stored copy: 8-bit RGBA, at most
// maxSize on the larger side, with the full chain of smaller levels. No D3D12
// here, so the rules are testable without a GPU; the pass that draws the copy
// is in image_shrink_gpu.h.

#include <cstdint>

namespace ImageShrink
{
	inline constexpr uint32_t kBytesPerPixel = 4;

	struct Size
	{
		uint32_t width  = 0;
		uint32_t height = 0;
	};

	// The size an image of the given source size is stored at: scaled down so the
	// larger side is maxSize, keeping the proportions, and never scaled up. An
	// image that is already maxSize or smaller keeps its own size.
	inline Size StoredSize(uint32_t sourceWidth, uint32_t sourceHeight, uint32_t maxSize)
	{
		const uint32_t larger = sourceWidth > sourceHeight ? sourceWidth : sourceHeight;
		if (sourceWidth == 0 || sourceHeight == 0 || larger <= maxSize)
			return { sourceWidth, sourceHeight };

		const auto scaled = [&](uint32_t side)
		{
			const uint64_t value = (static_cast<uint64_t>(side) * maxSize + larger / 2) / larger;
			return value > 0 ? static_cast<uint32_t>(value) : 1u;
		};
		return { scaled(sourceWidth), scaled(sourceHeight) };
	}

	// Levels in a full chain down to 1x1: 256x256 has 9, 256x128 has 9, 32x32 has 6.
	inline uint32_t LevelCount(uint32_t width, uint32_t height)
	{
		uint32_t larger = width > height ? width : height;
		uint32_t levels = 0;
		while (larger > 0)
		{
			++levels;
			larger >>= 1;
		}
		return levels;
	}

	inline Size LevelSize(Size size, uint32_t level)
	{
		const uint32_t width  = size.width >> level;
		const uint32_t height = size.height >> level;
		return { width > 0 ? width : 1u, height > 0 ? height : 1u };
	}

	// The pixel data of the first `levels` levels, in bytes. The driver may pad each
	// level to its own alignment; that padding is not counted.
	inline uint64_t Bytes(Size size, uint32_t levels)
	{
		uint64_t total = 0;
		for (uint32_t level = 0; level < levels; ++level)
		{
			const Size at = LevelSize(size, level);
			total += static_cast<uint64_t>(at.width) * at.height * kBytesPerPixel;
		}
		return total;
	}
}
