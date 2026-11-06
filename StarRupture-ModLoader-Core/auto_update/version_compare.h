#pragma once

#include <cstring>
#include <string>

// Version tags as the release workflows write them, compared by number. Used
// by the Core's update checks and by the standalone updater, so the banner,
// the plugin pass and the updater agree on what is newer.
namespace AutoUpdateVersion
{
    struct ParsedVersion
    {
        int  parts[4] = { 0, 0, 0, 0 };
        bool valid = false;
        bool hasSuffix = false; // e.g. "-preview" / "-beta.1"
    };

    // Accepts "v1.15.4", "1.15.2.0", "v1.16.0-beta.1", etc.
    inline ParsedVersion ParseVersion(const char* s)
    {
        ParsedVersion v;
        if (!s || !*s)
            return v;

        const char* p = s;
        if (*p == 'v' || *p == 'V')
            ++p;

        int idx = 0;
        while (idx < 4)
        {
            if (*p < '0' || *p > '9')
                break;
            long val = 0;
            while (*p >= '0' && *p <= '9')
            {
                val = val * 10 + (*p - '0');
                if (val > 1000000) val = 1000000;
                ++p;
            }
            v.parts[idx++] = static_cast<int>(val);
            if (*p == '.')
                ++p;
            else
                break;
        }

        v.valid = (idx >= 2); // require at least major.minor
        v.hasSuffix = (*p == '-' || *p == '+');
        return v;
    }

    // Returns >0 if a is newer than b, 0 if equal, <0 if older (numeric only).
    // A suffix is ignored: 1.23.0-rc1 and 1.23.0 are equal, so a pre-release
    // is never offered its own release and never offered an older one.
    inline int CompareVersions(const ParsedVersion& a, const ParsedVersion& b)
    {
        for (int i = 0; i < 4; ++i)
        {
            if (a.parts[i] != b.parts[i])
                return a.parts[i] < b.parts[i] ? -1 : 1;
        }
        return 0;
    }

    // Whether a version read from the installed DLL replaces the one stored in
    // update_state.ini: it parses, and the stored one is missing, does not
    // parse or is older by number.
    inline bool InstalledReplacesStored(const char* installed, const char* stored)
    {
        const ParsedVersion in = ParseVersion(installed);
        if (!in.valid)
            return false;
        const ParsedVersion was = ParseVersion(stored);
        return !was.valid || CompareVersions(in, was) > 0;
    }

    // Whether a release is newer than the installed version: by number, and
    // by plain difference when either does not parse.
    inline bool ReleaseIsNewer(const char* remote, const char* installed)
    {
        const ParsedVersion r = ParseVersion(remote);
        const ParsedVersion i = ParseVersion(installed);
        if (r.valid && i.valid)
            return CompareVersions(r, i) > 0;
        return std::strcmp(remote ? remote : "", installed ? installed : "") != 0;
    }

    // What the plugin update pass does with a release.
    //   stored   the version update_state.ini holds for the plugin ("" if none)
    //   onDisk   the version the installed DLL reports ("" if it could not be read)
    //   remote   the version the plugin's manifest offers
    // installed is the newer of stored and onDisk, so a build put in by hand,
    // newer than what the loader last wrote down, is never taken for the older
    // one. download is true only for a release newer than that; with no
    // installed version known there is nothing to compare against, so never.
    struct PluginUpdateDecision
    {
        std::string installed;
        bool        download = false;
    };

    inline PluginUpdateDecision DecidePluginUpdate(const char* stored, const char* onDisk, const char* remote)
    {
        PluginUpdateDecision d;
        d.installed = InstalledReplacesStored(onDisk, stored) ? onDisk : (stored ? stored : "");
        d.download  = !d.installed.empty() && ReleaseIsNewer(remote, d.installed.c_str());
        return d;
    }

    // What the loader does with the release notes of the manifest it fetched.
    //   running       the version this loader is ("" on a dev build)
    //   manifestTag   the manifest's build_tag ("" if the fetch failed)
    //   recorded      the version update_state.ini says notes were last shown for
    //   releaseNotes  the manifest's "release_notes" ("" if absent)
    //   notes         the manifest's "notes", used when there are no release_notes
    // The notes are shown once, for the release that is running: the manifest
    // names that version by number (a suffix such as -rc1 is ignored) and the
    // recorded version is missing, does not parse or is older. A newer manifest
    // is a release this loader is not yet, so nothing is shown or recorded.
    struct LoaderNotesDecision
    {
        bool        show = false;
        std::string text;
    };

    inline LoaderNotesDecision DecideLoaderNotes(const char* running, const char* manifestTag, const char* recorded,
                                                 const char* releaseNotes, const char* notes)
    {
        LoaderNotesDecision d;
        const ParsedVersion now = ParseVersion(running);
        const ParsedVersion rel = ParseVersion(manifestTag);
        if (!now.valid || !rel.valid || CompareVersions(rel, now) != 0)
            return d;

        const ParsedVersion shown = ParseVersion(recorded);
        if (shown.valid && CompareVersions(shown, now) >= 0)
            return d;

        const auto hasText = [](const char* t) { return t && t[0] && t[std::strspn(t, " \t\r\n")]; };
        const char* text = hasText(releaseNotes) ? releaseNotes : (hasText(notes) ? notes : nullptr);
        if (!text)
            return d;

        d.show = true;
        d.text = text;
        return d;
    }
}
