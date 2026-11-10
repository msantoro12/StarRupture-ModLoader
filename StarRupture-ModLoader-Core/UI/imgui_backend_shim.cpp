#include "pch.h"
#include "imgui_backend.h"

#ifdef MODLOADER_CLIENT_BUILD

#include "imgui_host_interface.h"
#include "../logging/log.h"

#include <atomic>

namespace
{
    HMODULE g_hostDll = nullptr;

    void                   (*g_initFn)(const ImGuiRenderCallbacks*)  = nullptr;
    void                   (*g_shutdownFn)()                          = nullptr;
    void                   (*g_setReadyFn)()                          = nullptr;
    IModLoaderImGui*       (*g_getAPIFn)()                            = nullptr;
    IPluginImGuiTextures*  (*g_getTexFn)()                            = nullptr;
    void                   (*g_fontRebuildFn)()                       = nullptr;
    bool                   (*g_isTextInputActiveFn)()                 = nullptr;
    // Atomic, unlike the rest: GetGameWindow is called from any thread.
    std::atomic<HWND (*)()> g_getGameWindowFn{ nullptr };
    // Atomic too: the render and RHI threads call these.
    std::atomic<int  (*)(void*)>                    g_recordUiOverlayFn{ nullptr };
    std::atomic<bool (*)(int, ImGuiUiOverlaySync*)> g_submitUiOverlayFn{ nullptr };
    // Atomic: the game thread and the RHI thread call these.
    std::atomic<void (*)(const char*, const char*)> g_diagEventFn{ nullptr };
    std::atomic<void (*)()>                         g_diagImmediateFn{ nullptr };
    std::atomic<void (*)(int, void*)>               g_diagTargetFn{ nullptr };
}

namespace UI::ImGuiBackend
{
    void Initialize(const ImGuiRenderCallbacks& cbs)
    {
        wchar_t dllPath[MAX_PATH]{};
        GetModuleFileNameW(nullptr, dllPath, MAX_PATH);
        wchar_t* slash = wcsrchr(dllPath, L'\\');
        if (slash)
            wcscpy_s(slash + 1, static_cast<rsize_t>(MAX_PATH - (slash + 1 - dllPath)),
                L"ModLoader\\StarRupture-ImGui.dll");

        g_hostDll = LoadLibraryW(dllPath);
        if (!g_hostDll)
        {
            LogToFile::Error("[ImGuiBackend] Failed to load ModLoader\\StarRupture-ImGui.dll (error %lu)",
                GetLastError());
            return;
        }

        g_initFn        = reinterpret_cast<decltype(g_initFn)>      (GetProcAddress(g_hostDll, "ImGuiHost_Initialize"));
        g_shutdownFn    = reinterpret_cast<decltype(g_shutdownFn)>   (GetProcAddress(g_hostDll, "ImGuiHost_Shutdown"));
        g_setReadyFn    = reinterpret_cast<decltype(g_setReadyFn)>   (GetProcAddress(g_hostDll, "ImGuiHost_SetRenderingReady"));
        g_getAPIFn      = reinterpret_cast<decltype(g_getAPIFn)>     (GetProcAddress(g_hostDll, "ImGuiHost_GetImGuiAPI"));
        g_getTexFn      = reinterpret_cast<decltype(g_getTexFn)>     (GetProcAddress(g_hostDll, "ImGuiHost_GetTextureAPI"));
        g_fontRebuildFn = reinterpret_cast<decltype(g_fontRebuildFn)>(GetProcAddress(g_hostDll, "ImGuiHost_RequestFontRebuild"));
        g_isTextInputActiveFn = reinterpret_cast<decltype(g_isTextInputActiveFn)>(GetProcAddress(g_hostDll, "ImGuiHost_IsTextInputActive"));
        g_getGameWindowFn.store(reinterpret_cast<HWND (*)()>(GetProcAddress(g_hostDll, "ImGuiHost_GetGameWindow")));
        g_recordUiOverlayFn.store(reinterpret_cast<int (*)(void*)>(GetProcAddress(g_hostDll, "ImGuiHost_RecordUiOverlay")));
        g_submitUiOverlayFn.store(reinterpret_cast<bool (*)(int, ImGuiUiOverlaySync*)>(GetProcAddress(g_hostDll, "ImGuiHost_SubmitUiOverlay")));
        g_diagEventFn.store(reinterpret_cast<void (*)(const char*, const char*)>(GetProcAddress(g_hostDll, "ImGuiHost_DiagEvent")));
        g_diagImmediateFn.store(reinterpret_cast<void (*)()>(GetProcAddress(g_hostDll, "ImGuiHost_DiagNoteImmediateSubmit")));
        g_diagTargetFn.store(reinterpret_cast<void (*)(int, void*)>(GetProcAddress(g_hostDll, "ImGuiHost_DiagCheckSubmitTarget")));

        if (!g_initFn)
        {
            LogToFile::Error("[ImGuiBackend] StarRupture-ImGui.dll missing expected exports");
            FreeLibrary(g_hostDll);
            g_hostDll = nullptr;
            return;
        }

        g_initFn(&cbs);
    }

    void Shutdown()
    {
        g_getGameWindowFn.store(nullptr);
        g_recordUiOverlayFn.store(nullptr);
        g_diagEventFn.store(nullptr);
        g_diagImmediateFn.store(nullptr);
        g_diagTargetFn.store(nullptr);
        if (g_shutdownFn) g_shutdownFn();
        g_submitUiOverlayFn.store(nullptr);
        if (g_hostDll)
        {
            FreeLibrary(g_hostDll);
            g_hostDll = nullptr;
        }
        g_initFn = nullptr;
        g_shutdownFn = nullptr;
        g_setReadyFn = nullptr;
        g_fontRebuildFn = nullptr;
        g_getAPIFn = nullptr;
        g_getTexFn = nullptr;
        g_isTextInputActiveFn = nullptr;
    }

    void SetRenderingReady()
    {
        if (g_setReadyFn) g_setReadyFn();
    }

    IModLoaderImGui* GetImGuiAPI()
    {
        return g_getAPIFn ? g_getAPIFn() : nullptr;
    }

    IPluginImGuiTextures* GetTextureAPI()
    {
        return g_getTexFn ? g_getTexFn() : nullptr;
    }

    void RequestFontRebuild()
    {
        if (g_fontRebuildFn) g_fontRebuildFn();
    }

    bool IsTextInputActive()
    {
        return g_isTextInputActiveFn ? g_isTextInputActiveFn() : false;
    }

    HWND GetGameWindow()
    {
        const auto fn = g_getGameWindowFn.load();
        return fn ? fn() : nullptr;
    }

    int RecordUiOverlay(void* rhiBackBuffer)
    {
        const auto fn = g_recordUiOverlayFn.load();
        return fn ? fn(rhiBackBuffer) : -1;
    }

    bool SubmitUiOverlay(int slot, ImGuiUiOverlaySync* sync)
    {
        const auto fn = g_submitUiOverlayFn.load();
        return fn ? fn(slot, sync) : false;
    }

    void DiagEvent(const char* event, const char* detail)
    {
        const auto fn = g_diagEventFn.load();
        if (fn) fn(event, detail);
    }

    void DiagNoteImmediateSubmit()
    {
        const auto fn = g_diagImmediateFn.load();
        if (fn) fn();
    }

    void DiagCheckSubmitTarget(int slot, void* rhiBackBuffer)
    {
        const auto fn = g_diagTargetFn.load();
        if (fn) fn(slot, rhiBackBuffer);
    }
}

#endif // MODLOADER_CLIENT_BUILD
