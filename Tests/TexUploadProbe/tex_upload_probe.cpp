// TexUploadProbe: a test plugin that drives the loader's texture upload API once,
// with nobody at the keyboard, so the diagnostic loader (branch
// test/texture-copy-timing) logs its [TexTiming] lines. See Tests/README.md.
//
// Being in the Plugins folder is the "on" switch. About 5 seconds after the first
// world begins (the main menu), on the game thread, it runs once:
//   1. 32 LoadFromRGBA calls, 256x256, generated pixels
//   2. up to 32 LoadFromUTexture2D copies of resident engine textures named *icon*
//   3. FreeTexture on every handle
// and writes one "[TexProbe] done:" line.
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <vector>

#include "plugins/plugin_interface.h"

#define PROBE_LOG(fmt, ...) g_self->logger->Info(g_self, "[TexProbe] " fmt, ##__VA_ARGS__)

namespace
{
    constexpr int       kBurst        = 32;
    constexpr int       kRgbaSize     = 256;
    constexpr int       kMaxAttempts  = 512;            // UTexture2D candidates tried at most
    constexpr int       kWalkCapacity = 65536;          // upper bound on the Texture2D walk
    constexpr ULONGLONG kDelayMs      = 5000;           // after the first world begins
    constexpr ULONGLONG kFallbackMs   = 60000;          // when no world event is seen

    IPluginSelf*                 g_self = nullptr;
    std::atomic<ULONGLONG>       g_runAt{ 0 };
    std::atomic<bool>            g_done{ false };

    double Ms(LARGE_INTEGER from, LARGE_INTEGER to)
    {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return 1000.0 * double(to.QuadPart - from.QuadPart) / double(f.QuadPart);
    }

    bool NameHasIcon(const char* name)
    {
        for (const char* p = name; *p; ++p)
            if (_strnicmp(p, "icon", 4) == 0)
                return true;
        return false;
    }

    // Everything below runs inside one game-thread tick. The UObject pointers from the
    // walk are used here and dropped when this function returns.
    void RunBurst()
    {
        IPluginHooks* hooks = g_self->hooks;
        IPluginImGuiTextures* tex = hooks ? hooks->ImGuiTextures : nullptr;
        if (!tex)
        {
            PROBE_LOG("done: no texture API (rgba=0 utex=0 freed=0)");
            return;
        }

        std::vector<PluginTextureHandle> handles;
        int rgba = 0, utex = 0, failed = 0, candidates = 0;
        char name[64];
        LARGE_INTEGER t0, t1, t2, t3;

        // 1. RGBA burst.
        std::vector<unsigned char> pixels(size_t(kRgbaSize) * kRgbaSize * 4);
        for (size_t i = 0; i < pixels.size(); ++i)
            pixels[i] = static_cast<unsigned char>((i * 7) ^ (i >> 8));
        QueryPerformanceCounter(&t0);
        for (int i = 0; i < kBurst && tex->GetFreeSlotCount() > 0; ++i)
        {
            std::snprintf(name, sizeof(name), "TexProbeRgba_%d", i);
            try
            {
                if (PluginTextureHandle h = tex->LoadFromRGBA(pixels.data(), kRgbaSize, kRgbaSize, name))
                {
                    handles.push_back(h);
                    ++rgba;
                }
                else
                    ++failed;
            }
            catch (...) { ++failed; }
        }
        QueryPerformanceCounter(&t1);

        // 2. Engine icon copies.
        QueryPerformanceCounter(&t2);
        IPluginObjectWalker* walker = hooks->ObjectWalker;
        if (!walker || !walker->IsReady())
            PROBE_LOG("walk: object walker %s, no engine copies", walker ? "not ready" : "missing");
        else
        {
            try
            {
                // The first call only counts the instances; the second fills a buffer that holds them all.
                PluginObjectInfo one{};
                const int total = walker->FindObjectsByClassNameInto("Texture2D", PluginObjectLookup_InstanceOnly, &one, 1);
                const int capacity = total < kWalkCapacity ? total : kWalkCapacity;
                std::vector<PluginObjectInfo> found(capacity > 0 ? capacity : 1);
                const int count = capacity > 0
                    ? walker->FindObjectsByClassNameInto("Texture2D", PluginObjectLookup_InstanceOnly, found.data(), capacity)
                    : 0;
                int attempts = 0;
                for (int i = 0; i < count && i < capacity && utex < kBurst && attempts < kMaxAttempts
                    && tex->GetFreeSlotCount() > 0; ++i)
                {
                    if (!found[i].object || !NameHasIcon(found[i].objectName))
                        continue;
                    ++candidates;
                    ++attempts;
                    std::snprintf(name, sizeof(name), "TexProbeUtex_%d", utex);
                    try
                    {
                        if (PluginTextureHandle h = tex->LoadFromUTexture2D(static_cast<SDK::UTexture2D*>(found[i].object), name))
                        {
                            handles.push_back(h);
                            ++utex;
                        }
                        else
                            ++failed;
                    }
                    catch (...) { ++failed; }
                }
                PROBE_LOG("walk: %d Texture2D instance(s)%s, %d tried, %d copied", total,
                    total > capacity ? " (walk truncated)" : "", candidates, utex);
            }
            catch (...) { PROBE_LOG("walk: threw, engine copies stopped at %d", utex); }
        }
        QueryPerformanceCounter(&t3);

        // 3. Release everything.
        for (PluginTextureHandle h : handles)
            tex->FreeTexture(h);

        PROBE_LOG("done: rgba=%d utex=%d freed=%d failed=%d rgba_ms=%.1f utex_ms=%.1f",
            rgba, utex, int(handles.size()), failed, Ms(t0, t1), Ms(t2, t3));
    }

    void OnTick(float)
    {
        if (g_done.load() || GetTickCount64() < g_runAt.load())
            return;
        if (!g_self->hooks->Engine->IsGameThread())
            return;
        g_done = true;
        RunBurst();
    }

    void OnAnyWorldBeginPlay(SDK::UWorld*, const char* worldName)
    {
        const ULONGLONG at = GetTickCount64() + kDelayMs;
        if (!g_done.load() && at < g_runAt.load())
        {
            g_runAt = at;
            PROBE_LOG("world '%s' began, burst in %llu ms", worldName ? worldName : "?", kDelayMs);
        }
    }

    PluginInfo s_info = {
        "TexUploadProbe",
        "test",
        "test",
        "Runs one texture upload burst for the loader's [TexTiming] diagnostics. Test only.",
        PLUGIN_INTERFACE_VERSION,
        PLUGIN_TARGET_CLIENT,
    };
}

extern "C"
{
    __declspec(dllexport) PluginInfo* GetPluginInfo() { return &s_info; }

    __declspec(dllexport) bool PluginInit(IPluginSelf* self)
    {
        g_self = self;
        g_done = false;
        g_runAt = GetTickCount64() + kFallbackMs;
        self->hooks->Engine->RegisterOnTick(OnTick);
        self->hooks->World->RegisterOnAnyWorldBeginPlay(OnAnyWorldBeginPlay);
        PROBE_LOG("loaded; burst runs %llu ms after the first world begins", kDelayMs);
        return true;
    }

    __declspec(dllexport) void PluginShutdown()
    {
        g_self->hooks->World->UnregisterOnAnyWorldBeginPlay(OnAnyWorldBeginPlay);
        g_self->hooks->Engine->UnregisterOnTick(OnTick);
        g_self = nullptr;
    }
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(module);
    return TRUE;
}
