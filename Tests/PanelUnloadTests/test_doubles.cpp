// Stand-ins for the loader pieces plugin_panel_registry.cpp reaches out to, so
// the test runs as a plain console program with no game and no plugin DLLs.
#include "plugins/plugin_manager.h"
#include "theme.h"

// FireConfigChanged resolves a plugin name through here; no test uses it.
namespace PluginManager
{
    const IPluginSelf* GetSelfForPlugin(const char*)
    {
        return nullptr;
    }
}

// plugin_panel_registry.cpp draws panel windows through these; nothing here
// renders.
namespace UI::Theme
{
    bool BeginChamferedWindow(const char*, const char*, bool*, const char*, ImGuiWindowFlags, bool)
    {
        return false;
    }

    void EndChamferedWindow() {}
}
