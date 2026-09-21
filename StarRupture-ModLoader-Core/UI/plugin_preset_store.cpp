#include "pch.h"
#include "plugin_preset_store.h"

#ifdef MODLOADER_CLIENT_BUILD

#include "config/config_manager.h"
#include <cstring>
#include <cstdio>

namespace UI::PluginPresetStore
{
    bool GetIniPath(const char* pluginName, wchar_t* outPath, size_t outLen)
    {
        const wchar_t* configDir = ModLoaderLogger::GetConfigDirectory();
        if (!configDir || !pluginName) return false;
        swprintf_s(outPath, outLen, L"%s\\%S-Presets.ini", configDir, pluginName);
        return true;
    }

    int ListNames(const char* pluginName, char outNames[][64], int cap)
    {
        wchar_t iniPath[MAX_PATH];
        if (!GetIniPath(pluginName, iniPath, MAX_PATH)) return 0;

        wchar_t sectionBuf[4096] = {};
        GetPrivateProfileSectionNamesW(sectionBuf, ARRAYSIZE(sectionBuf), iniPath);

        int n = 0;
        for (const wchar_t* sec = sectionBuf; *sec && n < cap; sec += wcslen(sec) + 1)
        {
            char narrow[64] = {};
            snprintf(narrow, sizeof(narrow), "%ls", sec);
            strncpy_s(outNames[n++], 64, narrow, _TRUNCATE);
        }
        return n;
    }

    bool Save(const char* pluginName, const char* presetName, const Field* fields, int count)
    {
        wchar_t iniPath[MAX_PATH];
        if (!GetIniPath(pluginName, iniPath, MAX_PATH)) return false;
        if (!presetName || !presetName[0]) return false;

        wchar_t wSection[64];
        swprintf_s(wSection, L"%S", presetName);

        for (int i = 0; i < count; ++i)
        {
            wchar_t wKey[192], wValue[256];
            swprintf_s(wKey, L"%S.%S", fields[i].section, fields[i].key);
            swprintf_s(wValue, L"%S", fields[i].value);
            WritePrivateProfileStringW(wSection, wKey, wValue, iniPath);
        }
        return true;
    }

    int Load(const char* pluginName, const char* presetName, Field* outFields, int cap)
    {
        wchar_t iniPath[MAX_PATH];
        if (!GetIniPath(pluginName, iniPath, MAX_PATH)) return -1;
        if (!presetName || !presetName[0]) return -1;

        wchar_t wSection[64];
        swprintf_s(wSection, L"%S", presetName);

        // A section with no keys reads back empty either way (missing vs
        // present-but-empty are indistinguishable through this API) -- good
        // enough here, since an empty preset is a degenerate case the UI
        // shouldn't be able to produce in the first place (Save always
        // writes the whole current entry set).
        wchar_t kvBuf[8192] = {};
        GetPrivateProfileSectionW(wSection, kvBuf, ARRAYSIZE(kvBuf), iniPath);
        if (!kvBuf[0]) return -1;

        int n = 0;
        for (const wchar_t* kv = kvBuf; *kv && n < cap; kv += wcslen(kv) + 1)
        {
            const wchar_t* eq = wcschr(kv, L'=');
            if (!eq) continue;

            char compound[128] = {};
            snprintf(compound, sizeof(compound), "%.*ls", (int)(eq - kv), kv);

            // Split "<section>.<key>" on the first '.' -- config section
            // names are plain identifiers (General, Drone, Player, ...)
            // that never contain one themselves.
            char* dot = strchr(compound, '.');
            if (!dot) continue;
            *dot = '\0';

            strncpy_s(outFields[n].section, compound, _TRUNCATE);
            strncpy_s(outFields[n].key,     dot + 1,  _TRUNCATE);
            snprintf(outFields[n].value, sizeof(outFields[n].value), "%ls", eq + 1);
            ++n;
        }
        return n;
    }

    bool Rename(const char* pluginName, const char* from, const char* to)
    {
        wchar_t iniPath[MAX_PATH];
        if (!GetIniPath(pluginName, iniPath, MAX_PATH)) return false;
        if (!from || !from[0] || !to || !to[0]) return false;

        char existing[64][64];
        int existingCount = ListNames(pluginName, existing, 64);
        for (int i = 0; i < existingCount; ++i)
            if (_stricmp(existing[i], to) == 0) return false; // already taken

        wchar_t wFrom[64], wTo[64];
        swprintf_s(wFrom, L"%S", from);
        swprintf_s(wTo,   L"%S", to);

        wchar_t kvBuf[8192] = {};
        GetPrivateProfileSectionW(wFrom, kvBuf, ARRAYSIZE(kvBuf), iniPath);
        if (!kvBuf[0]) return false; // from doesn't exist (or is empty -- same as not existing here)

        for (const wchar_t* kv = kvBuf; *kv; kv += wcslen(kv) + 1)
        {
            const wchar_t* eq = wcschr(kv, L'=');
            if (!eq) continue;
            wchar_t key[192] = {};
            wcsncpy_s(key, kv, eq - kv);
            WritePrivateProfileStringW(wTo, key, eq + 1, iniPath);
        }

        WritePrivateProfileStringW(wFrom, nullptr, nullptr, iniPath); // delete the old section
        return true;
    }

    bool Delete(const char* pluginName, const char* presetName)
    {
        wchar_t iniPath[MAX_PATH];
        if (!GetIniPath(pluginName, iniPath, MAX_PATH)) return false;
        if (!presetName || !presetName[0]) return false;

        wchar_t wSection[64];
        swprintf_s(wSection, L"%S", presetName);
        return WritePrivateProfileStringW(wSection, nullptr, nullptr, iniPath) != 0;
    }
}

#endif // MODLOADER_CLIENT_BUILD
