#pragma once

// Test build: what the image trial (core/image_trial.cpp) asks of the ImGui host.
// One export, ImGuiHost_ImageTrial(op, a, b), so the host DLL grows by one entry.
// Game thread only. Nothing here ships.

namespace ImageTrialOps
{
    enum Op : int
    {
        HeapRows     = 1,  // a = nullptr. Returns 1 when the version map heap rows are in use, 0 when not, -1 while the device is not up yet.
        TextureFacts = 2,  // a = UTexture2D*, b = Facts*. Returns 1 when filled, 0 when the texture has no RHI resource yet, -1 on a fault.
        Memory       = 3,  // a = MemoryInfo*. Returns 1 when filled.
        CopyState    = 4,  // a = PluginTextureHandle. Returns 1 when its copy has finished on the GPU, 0 while it is pending, -1 for a bad handle.
        DumpPng      = 5,  // a = PluginTextureHandle, b = const wchar_t* path. Returns 1 written, 0 not yet, -1 failed or format not handled.
        Parked       = 6,  // Returns the number of residency requests parked by the icon copies.
    };

    struct Facts
    {
        unsigned           fmt;          // DXGI_FORMAT of the resident resource
        unsigned           w, h;         // size of its top resident level
        unsigned           mips;         // resident levels
        unsigned           depthOrArray;
        unsigned           dimension;
        unsigned           flags;        // D3D12_RESOURCE_FLAGS
        unsigned           samples;
        unsigned           state;        // FD3D12Resource::DefaultResourceState
        int                heap;         // 0 committed, 1 placed in a heap that checked out, 2 placed or unreadable (probe invalid)
        int                reduced;      // 1 when the safe read would give this source reduced protection
        unsigned long long allocBytes;   // GetResourceAllocationInfo size
    };

    struct MemoryInfo
    {
        char               adapter[128]; // adapter name, UTF-8
        unsigned long long dedicatedBytes;
        unsigned long long budget;       // QueryVideoMemoryInfo, local segment
        unsigned long long usage;
        unsigned long long availableToReserve;
        unsigned long long reserved;
    };
}
