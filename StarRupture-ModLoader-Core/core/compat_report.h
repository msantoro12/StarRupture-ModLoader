#pragma once

#include "layout_check.h"

#include <cstdint>
#include <string>
#include <vector>

// The compatibility report: ModLoader\compat-report-<CL>.json, written once
// when the startup layout check switches plugins off. It says which game build
// this is, which scan patterns missed, where the engine globals resolved and
// which SDK class members moved, so a loader update can be made from it.
// Nothing else goes in: no paths, no user or machine names, no plugin list.
namespace CompatReport
{
    struct PatternMiss
    {
        std::string name;
        std::string pattern;
        bool        required;
        bool        ambiguous;  // found, but not provably the right match
    };

    struct GlobalRva
    {
        std::string name;
        bool        resolved;
        uint64_t    rva;        // 0 when not resolved
        int32_t     sdkOffset;  // the SDK's Offsets:: value
    };

    struct Inputs
    {
        std::string                productVersion;  // the game exe's ProductVersion
        std::string                exeName;         // file name only
        std::string                exeSha256;       // lowercase hex; empty if it could not be read
        uint32_t                   sdkCL;           // the CL of the SDK the loader was built against
        std::vector<PatternMiss>   patternMisses;
        std::vector<GlobalRva>     globals;
        const LayoutCheck::Result* layout;
    };

    // The report's JSON text. Kept apart from the game so it can be tested.
    std::string Build(const Inputs& inputs);

    // The CL in a ProductVersion ("1.0.0-CL-127004" -> "127004"), or
    // "unknown" when it has none.
    std::string ClOf(const std::string& productVersion);

    // Client builds: gathers the inputs, writes the report and returns its
    // file name (empty if it could not be written). Called once, from the
    // init thread, when the layout check switches plugins off.
    std::wstring Write(const LayoutCheck::Result& layout);
}
