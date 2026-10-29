// LayoutCheckTests: the startup layout check's comparison, with no game.
//
// fixtures\ holds two layout tables generated from the Game-SDK at CL 126119
// and CL 127004 (ACrCharacterPlayerBase and its reflected bases). One stands
// in for the SDK the loader was built against, the other for the running
// game, so the comparison sees exactly what a loader built for one build
// would see on the other. See Tests/README.md.

#include "layout_check.h"

#include "fixtures/layout_table_126119.h"
#include "fixtures/layout_table_127004.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>

namespace
{
    int g_failures = 0;

    void Check(bool ok, const char* what)
    {
        std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok)
            ++g_failures;
    }

    bool Contains(const LayoutCheck::Result& result, const std::string& line)
    {
        return std::find(result.lines.begin(), result.lines.end(), line) != result.lines.end();
    }

    // A table read back as what the game would reflect for each class.
    std::map<std::string, LayoutCheck::LiveStruct> AsGame(const std::vector<LayoutCheck::TableClass>& table)
    {
        std::map<std::string, LayoutCheck::LiveStruct> game;
        for (const LayoutCheck::TableClass& c : table)
        {
            LayoutCheck::LiveStruct& live = game[c.path];
            live.size = static_cast<int32_t>(c.sizeOf);
            live.minAlignment = static_cast<int32_t>(c.alignOf);
            for (const LayoutCheck::TableRow& row : c.rows)
            {
                live.properties.push_back(LayoutCheck::LiveProperty{ row.name, static_cast<int32_t>(row.offset),
                    static_cast<int32_t>(row.size), row.boolMask != 0, row.boolMask });
            }
        }
        return game;
    }

    LayoutCheck::Result CompareWith(const std::vector<LayoutCheck::TableClass>& sdk,
        const std::map<std::string, LayoutCheck::LiveStruct>& game)
    {
        return LayoutCheck::Compare(sdk, [&game](const LayoutCheck::TableClass& c) -> const LayoutCheck::LiveStruct*
        {
            const auto it = game.find(c.path);
            return it == game.end() ? nullptr : &it->second;
        });
    }

    const auto kTable126119 = []
    {
        return LayoutCheck::ReadTable(SrLayoutTable126119::kNames, SrLayoutTable126119::kClasses, SrLayoutTable126119::kRows);
    };
    const auto kTable127004 = []
    {
        return LayoutCheck::ReadTable(SrLayoutTable127004::kNames, SrLayoutTable127004::kClasses, SrLayoutTable127004::kRows);
    };

    void SameBuildMatches()
    {
        std::printf("127004 SDK on a 127004 game\n");
        const auto table = kTable127004();
        const LayoutCheck::Result result = CompareWith(table, AsGame(table));

        Check(result.classes == static_cast<int>(SrLayoutTable127004::kClassCount), "every class is counted");
        Check(result.checked == result.classes, "every class is checked");
        Check(result.mismatched == 0 && result.missing == 0 && result.deferred == 0, "0 mismatched, 0 missing, 0 deferred");
        Check(result.lines.empty(), "nothing to log");
    }

    void ContentUpdateMovesThePlayer()
    {
        std::printf("126119 SDK on a 127004 game\n");
        const LayoutCheck::Result result = CompareWith(kTable126119(), AsGame(kTable127004()));

        Check(result.changedClasses == std::vector<std::string>{ "ACrCharacterPlayerBase" },
            "ACrCharacterPlayerBase is the one changed class");
        Check(result.mismatched == 1 && result.missing == 0, "1 mismatched, 0 missing");
        Check(Contains(result, "ACrCharacterPlayerBase: size SDK 0x15E0, game 0x15F0"), "its size moved 0x15E0 -> 0x15F0");
        Check(Contains(result, "ACrCharacterPlayerBase::ADSMoveSpeed: offset SDK 0x13A8, game 0x13B8"),
            "ADSMoveSpeed moved 0x13A8 -> 0x13B8");

        for (const std::string& line : result.lines)
        {
            if (line.rfind("ACrCharacterPlayerBase", 0) != 0)
            {
                Check(false, ("only the player class differs: " + line).c_str());
                break;
            }
        }
    }

    void EachDifferenceIsReported()
    {
        std::printf("Synthetic differences\n");
        using LayoutCheck::Kind;
        const std::vector<LayoutCheck::TableClass> sdk = {
            { "AGone", "Mod.Gone", 0x10, 8, Kind::Class, {} },
            { "UBlueprint_C", "Blueprint_C.Blueprint_C", 0x10, 8, Kind::Blueprint, {} },
            { "FUnknown", "Mod.Unknown", 0x10, 8, Kind::Unknown, {} },
            { "FThing", "Mod.Thing", 0x20, 8, Kind::ScriptStruct, {
                { "Missing", 0x0, 4, 0 },
                { "Size", 0x8, 4, 0 },
                { "Bit", 0x10, 1, 0x02 },
                { "WasBool", 0x11, 1, 0xFF },
                { "NowBool", 0x12, 1, 0 },
                { "Unsized", 0x14, 0, 0 },
            } },
        };

        LayoutCheck::LiveStruct thing{ 0x20, 16, {
            { "Size", 0x8, 8, false, 0 },
            { "Bit", 0x10, 1, true, 0x04 },
            { "WasBool", 0x11, 1, false, 0 },
            { "NowBool", 0x12, 1, true, 0xFF },
            { "Unsized", 0x14, 12, false, 0 },
        } };
        const std::map<std::string, LayoutCheck::LiveStruct> game = { { "Mod.Thing", thing } };
        const LayoutCheck::Result result = CompareWith(sdk, game);

        Check(result.classes == 4 && result.checked == 1 && result.mismatched == 1 && result.missing == 1
            && result.deferred == 2, "4 classes: 1 checked and mismatched, 1 missing, 2 deferred");
        Check(Contains(result, "AGone (Mod.Gone): not found in the game"), "a missing class");
        Check(Contains(result, "FThing: alignment SDK 8, game 16"), "an alignment change");
        Check(Contains(result, "FThing::Missing: not found in the game"), "a missing member");
        Check(Contains(result, "FThing::Size: size SDK 0x4, game 0x8"), "a member size change");
        Check(Contains(result, "FThing::Bit: bool mask SDK 0x02, game 0x04"), "a bit-field that moved bit");
        Check(Contains(result, "FThing::WasBool: a bool in the SDK, not in the game"), "a bool that is no longer one");
        Check(Contains(result, "FThing::NowBool: a bool in the game, not in the SDK"), "a member that became a bool");
        Check(result.lines.size() == 7, "nothing else, and a size of 0 is not compared");
    }
}

int main()
{
    SameBuildMatches();
    ContentUpdateMovesThePlayer();
    EachDifferenceIsReported();

    if (g_failures)
        std::printf("\n%d check(s) FAILED\n", g_failures);
    else
        std::printf("\nAll checks passed\n");
    return g_failures ? 1 : 0;
}
