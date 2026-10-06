// PluginPanelRegistry and PluginWidgetRegistry ForgetModule test.
//
// A console program: no game, no plugin DLLs. The registry is the loader's own
// source file; test_doubles.cpp replaces only what it calls out to.
//
// Two "plugins" are needed, and a module is whatever an address lives in. This
// executable is one: its own functions are plugin A's panel and callback.
// kernel32.dll is the other: a function address from it stands in for plugin B's
// renderFn and callback. Neither is ever called -- no panel is opened and
// nothing is closed -- so only the module an address belongs to matters.
#include "UI/plugin_panel_registry.h"
#include "UI/plugin_widget_registry.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

static int g_checks = 0;
static int g_failed = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_failed;                                                          \
            printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
        }                                                                        \
    } while (0)

static void RunTest(const char* name, void (*fn)())
{
    const int before = g_failed;
    printf("[ RUN  ] %s\n", name);
    fn();
    printf("[ %s ] %s\n", g_failed == before ? " OK " : "FAIL", name);
}

namespace Registry = UI::PluginPanelRegistry;

// ---------------------------------------------------------------------------
// Plugin A: lives in this executable.
// ---------------------------------------------------------------------------

static int g_closedA = 0;

static void RenderA(IModLoaderImGui*) {}
static void ClosedA(PanelHandle) { ++g_closedA; }

static const PluginPanelDesc kPanelA = { "A", "Panel A", RenderA };

// ---------------------------------------------------------------------------
// Plugin B: lives in kernel32.dll. These are never called (see above).
// ---------------------------------------------------------------------------

static HMODULE ModuleB()
{
    return GetModuleHandleW(L"kernel32.dll");
}

static PluginImGuiRenderCallback RenderB()
{
    return reinterpret_cast<PluginImGuiRenderCallback>(GetProcAddress(ModuleB(), "GetLastError"));
}

static PluginPanelClosedCallback ClosedB()
{
    return reinterpret_cast<PluginPanelClosedCallback>(GetProcAddress(ModuleB(), "GetCurrentThreadId"));
}

static int PanelCount()
{
    int registered = 0;
    Registry::GetPanelCounts(&registered, nullptr);
    return registered;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

// The unloading plugin's panels are dropped, the other plugin's are kept, and
// the same title can be registered again afterwards (a reloaded plugin does).
static void Test_PanelsPurgedByModule()
{
    const PluginPanelDesc panelB = { "B", "Panel B", RenderB() };

    PanelHandle a = Registry::RegisterPanel(&kPanelA);
    PanelHandle b = Registry::RegisterPanel(&panelB);
    CHECK(a && b);
    CHECK(PanelCount() == 2);

    Registry::ForgetModule(GetModuleHandleW(nullptr)); // plugin A unloads

    CHECK(PanelCount() == 1);
    CHECK(Registry::RegisterPanel(&panelB) == nullptr); // B is still registered: duplicate title
    PanelHandle a2 = Registry::RegisterPanel(&kPanelA);  // A is gone: registers afresh
    CHECK(a2 != nullptr);
    CHECK(PanelCount() == 2);

    Registry::ForgetModule(GetModuleHandleW(nullptr));
    Registry::ForgetModule(ModuleB());
    CHECK(PanelCount() == 0);
}

// A panel that was open when its plugin went away is dropped quietly: no
// callback into the module, and a handle the plugin still holds is harmless.
static void Test_OpenPanelAndStaleHandle()
{
    g_closedA = 0;
    Registry::RegisterOnPanelWindowClosed(ClosedA);

    PanelHandle a = Registry::RegisterPanel(&kPanelA);
    Registry::SetPanelOpen(a);
    CHECK(Registry::AnyPanelOpen());

    Registry::ForgetModule(GetModuleHandleW(nullptr));
    CHECK(!Registry::AnyPanelOpen());
    CHECK(PanelCount() == 0);

    Registry::SetPanelClose(a);   // stale handle: ignored
    Registry::SetPanelOpen(a);
    Registry::UnregisterPanel(a);
    CHECK(PanelCount() == 0);
    CHECK(g_closedA == 0);
}

// Panel-closed callbacks have an owner too: the unloading plugin's is dropped
// and never called again, the other plugin's stays registered. Closing plugin
// B's panel fires every callback still registered. B's own callback is the
// harmless GetCurrentThreadId; A's is the one that would run freed code.
static void Test_ClosedCallbacksPurgedByModule()
{
    g_closedA = 0;
    const PluginPanelDesc panelB = { "B", "Panel B", RenderB() };

    Registry::RegisterOnPanelWindowClosed(ClosedA);
    Registry::RegisterOnPanelWindowClosed(ClosedB());
    PanelHandle b = Registry::RegisterPanel(&panelB);

    // Control: with both registered, closing B's panel reaches A's callback.
    Registry::SetPanelOpen(b);
    Registry::SetPanelClose(b);
    CHECK(g_closedA == 1);

    Registry::ForgetModule(GetModuleHandleW(nullptr)); // plugin A unloads

    Registry::SetPanelOpen(b);
    Registry::SetPanelClose(b);
    CHECK(g_closedA == 1); // not called again

    Registry::UnregisterOnPanelWindowClosed(ClosedB());
    Registry::ForgetModule(ModuleB());
}

// Forgetting a module that owns nothing, or no module, changes nothing.
static void Test_ForgetOfUnrelatedModuleIsHarmless()
{
    PanelHandle a = Registry::RegisterPanel(&kPanelA);
    CHECK(a != nullptr);

    Registry::ForgetModule(nullptr);
    Registry::ForgetModule(ModuleB());
    CHECK(PanelCount() == 1);

    Registry::ForgetModule(GetModuleHandleW(nullptr));
    CHECK(PanelCount() == 0);
}

// Widgets hold the same kind of pointers and are purged the same way. There is
// no widget count to read, so a duplicate name shows what is still registered.
static void Test_WidgetsPurgedByModule()
{
    const PluginWidgetDesc widgetA = { "Widget A", RenderA, nullptr };
    const PluginWidgetDesc widgetB = { "Widget B", RenderB(), nullptr };

    CHECK(UI::PluginWidgetRegistry::RegisterWidget(&widgetA) != nullptr);
    CHECK(UI::PluginWidgetRegistry::RegisterWidget(&widgetB) != nullptr);

    UI::PluginWidgetRegistry::ForgetModule(GetModuleHandleW(nullptr)); // plugin A unloads

    CHECK(UI::PluginWidgetRegistry::RegisterWidget(&widgetB) == nullptr); // B kept
    CHECK(UI::PluginWidgetRegistry::RegisterWidget(&widgetA) != nullptr); // A gone

    UI::PluginWidgetRegistry::ForgetModule(GetModuleHandleW(nullptr));
    UI::PluginWidgetRegistry::ForgetModule(ModuleB());
    CHECK(UI::PluginWidgetRegistry::RegisterWidget(&widgetB) != nullptr);
    UI::PluginWidgetRegistry::ForgetModule(ModuleB());
}

// The tests above prove the purge works; this one proves the loader calls it.
// plugin_manager.cpp cannot be linked into a console program (it is the plugin
// loader), so its three paths that unmap a plugin -- unload all, unload one,
// reload -- are read as source. Each must call both registries' ForgetModule,
// on the module it frees, before FreeLibrary, since afterwards a panel's or
// widget's renderFn is freed memory. Delete any one call and this fails.
static std::string StripLineComments(const std::string& src)
{
    std::string out;
    for (size_t pos = 0; pos < src.size();)
    {
        size_t eol = src.find('\n', pos);
        if (eol == std::string::npos) eol = src.size();
        std::string line = src.substr(pos, eol - pos);
        const size_t c = line.find("//");
        if (c != std::string::npos) line.erase(c);
        out += line;
        out += '\n';
        pos = eol + 1;
    }
    return out;
}

// Text of the function whose definition starts with signature, up to its
// closing brace (a tab-indented "}" line; the functions sit in a namespace).
static std::string FunctionBody(const std::string& src, const char* signature)
{
    const size_t start = src.find(signature);
    if (start == std::string::npos) return "";
    const size_t end = src.find("\n\t}", start);
    return end == std::string::npos ? "" : src.substr(start, end - start);
}

static void Test_UnloadPathsPurgePanels()
{
    // <solution>\build\tests\panel_unload_tests.exe: the exe's name, then tests, then build.
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring path = exe;
    for (int up = 0; up < 3; ++up)
        path.erase(path.find_last_of(L"\\/"));
    path += L"\\StarRupture-ModLoader-Core\\plugins\\plugin_manager.cpp";

    std::ifstream in(path.c_str(), std::ios::binary);
    CHECK(in.good());
    const std::string src = StripLineComments(
        std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()));

    const char* const kPaths[] =
    {
        "void UnloadAllPlugins()",
        "bool UnloadPlugin(int index)",
        "bool ReloadPlugin(int index)",
    };
    for (const char* signature : kPaths)
    {
        printf("    %s\n", signature);
        const std::string body = FunctionBody(src, signature);
        CHECK(!body.empty());

        const size_t free = body.find("FreeLibrary(");
        CHECK(free != std::string::npos);
        if (free == std::string::npos) continue;

        // The module being freed, e.g. "p.hModule".
        const size_t argStart = free + strlen("FreeLibrary(");
        const std::string module = body.substr(argStart, body.find(')', argStart) - argStart);

        for (const char* registry : { "PluginPanelRegistry", "PluginWidgetRegistry" })
        {
            const size_t forget = body.find(std::string(registry) + "::ForgetModule(" + module + ")");
            CHECK(forget != std::string::npos);
            CHECK(forget < free);
        }
    }
}

int main()
{
    RunTest("PanelsPurgedByModule",              Test_PanelsPurgedByModule);
    RunTest("OpenPanelAndStaleHandle",           Test_OpenPanelAndStaleHandle);
    RunTest("ClosedCallbacksPurgedByModule",     Test_ClosedCallbacksPurgedByModule);
    RunTest("ForgetOfUnrelatedModuleIsHarmless", Test_ForgetOfUnrelatedModuleIsHarmless);
    RunTest("WidgetsPurgedByModule",             Test_WidgetsPurgedByModule);
    RunTest("UnloadPathsPurgePanels",            Test_UnloadPathsPurgePanels);

    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
