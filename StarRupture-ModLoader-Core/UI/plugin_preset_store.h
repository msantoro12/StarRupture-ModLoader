#pragma once

#ifdef MODLOADER_CLIENT_BUILD

// ---------------------------------------------------------------------------
// UI::PluginPresetStore
//
// Named, saved snapshots of a plugin's own Config page -- the loader-side
// counterpart to the per-plugin preset stores BetterDrone and BetterCheats
// each carry (src/preset_store.h/.cpp in those repos, not part of this one):
// same named-collection-in-an-INI shape, same SuggestName-style unique-name
// rule (UI::NamedEntryUtils::SuggestUniqueName, shared with this file), but
// applied through the loader's own config commit path (NotifyConfigChangedLive/
// CommitConfigChange, RenderConfigTab) instead of a plugin owning its data.
//
// One file per plugin (<configDir>\<plugin>-Presets.ini, next to its own
// <plugin>.ini), one INI section per saved preset. Pure INI I/O -- no
// per-frame disk access (callers cache/gate re-scans themselves, the same
// way RenderThemeTab's theme list already does) and no knowledge of
// ConfigKV/ConfigSchema/Hooks::Input; applying a loaded field back to the
// live plugin state, including a keybind's own "<key>Blocking" companion,
// is the caller's job (modloader_window.cpp), same as SaveUserTheme/
// LoadColors leave applying a loaded color to ImGuiStyle to theme.cpp's own
// ApplyTheme rather than doing it here.
// ---------------------------------------------------------------------------
namespace UI::PluginPresetStore
{
    struct Field
    {
        char section[64];
        char key[64];
        char value[256];
    };

    // <configDir>\<pluginName>-Presets.ini. False only if the config
    // directory itself isn't available yet (mirrors GetPluginIniPath's own
    // contract in modloader_window.cpp).
    bool GetIniPath(const char* pluginName, wchar_t* outPath, size_t outLen);

    // Fills outNames (cap entries, 64 bytes each) with every preset name
    // saved for pluginName, in the order the ini itself stores them (not
    // sorted -- same as GetAvailableThemes' own file-scan order). Returns
    // the count written.
    int ListNames(const char* pluginName, char outNames[][64], int cap);

    // Writes fields as a preset named presetName, creating it or
    // overwriting an existing one of that name. Each field is stored as one
    // ini key, named "<section>.<key>" so entries from different config
    // sections (and a keybind's own "<key>Blocking" companion, section-
    // qualified the same way) don't collide within one preset's section.
    bool Save(const char* pluginName, const char* presetName, const Field* fields, int count);

    // Fills outFields (cap entries) from the stored preset. Returns the
    // count written, or -1 if no such preset exists. Only reads the ini --
    // the caller applies each field back through its own commit path.
    int Load(const char* pluginName, const char* presetName, Field* outFields, int cap);

    // Fails and leaves the store unchanged if from doesn't exist, or to is
    // already taken (case-insensitive).
    bool Rename(const char* pluginName, const char* from, const char* to);

    bool Delete(const char* pluginName, const char* presetName);
}

#endif // MODLOADER_CLIENT_BUILD
