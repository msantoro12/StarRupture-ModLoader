#include "sdk_globals.h"

#if defined(MODLOADER_CLIENT_BUILD)

#include "CoreUObject_classes.hpp"
#include "../hooks/game/scan_patterns.h"
#include "../logging/logger.h"
#include "../memory_scanner/pattern_preflight.h"
#include "../memory_scanner/scan_validation.h"

#include <cstring>

namespace SdkGlobals
{
    namespace
    {
        struct Global
        {
            const char* name;       // as the SDK's Offsets:: spells it
            const char* pattern;    // the preflight registry row that finds it
            int32_t     sdkOffset;  // Offsets::<name>
            uintptr_t   address = 0;
        };

        // The registry row, found by its pattern so the request is the one
        // preflight verified -- the same transform, the same kind check.
        const ScanPatterns::PreflightEntry* FindRegistryEntry(const char* pattern)
        {
            for (const ScanPatterns::PreflightEntry& entry : ScanPatterns::PreflightRegistry)
            {
                if (strcmp(entry.pattern, pattern) == 0)
                    return &entry;
            }
            return nullptr;
        }
    }

    void ResolveAndApply()
    {
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));

        Global globals[] =
        {
            { "GObjects",     ScanPatterns::GObjects_PreAllocate,  SDK::Offsets::GObjects },
            { "GNames",       ScanPatterns::GNames_NamePoolData,   SDK::Offsets::GNames },
            { "AppendString", ScanPatterns::FName_AppendString,    SDK::Offsets::AppendString },
            { "GWorld",       ScanPatterns::GWorld_EngineLoopTick, SDK::Offsets::GWorld },
        };
        Global& gObjects     = globals[0];
        Global& appendString = globals[2];

        bool allResolved = true;
        for (Global& global : globals)
        {
            const ScanPatterns::PreflightEntry* entry = FindRegistryEntry(global.pattern);
            if (!entry)
            {
                ModLoaderLogger::LogError(L"[SdkGlobals] %S: no preflight registry row for its pattern", global.name);
                allResolved = false;
                continue;
            }

            const ScanValidation::Result result = ScanValidation::Resolve(PatternPreflight::MakeRequest(*entry));
            if (!result.Succeeded())
            {
                // Preflight has already logged the full reason for this row.
                ModLoaderLogger::LogWarn(L"[SdkGlobals] %S: did not resolve (see \"%S\" in the preflight above)",
                    global.name, entry->name);
                allResolved = false;
                continue;
            }

            global.address = result.address;
            const unsigned long long rva = global.address - base;
            ModLoaderLogger::LogInfo(L"[SdkGlobals] %-12S base+0x%llX  SDK Offsets::%S base+0x%X  %s",
                global.name, rva, global.name, global.sdkOffset,
                rva == static_cast<unsigned long long>(global.sdkOffset) ? L"(same)" : L"(DIFFERENT)");
        }

        if (!allResolved)
        {
            ModLoaderLogger::LogWarn(L"[SdkGlobals] Not every global resolved -- the SDK keeps its built-in Offsets:: values");
            return;
        }

        SDK::UObject::GObjects.InitManually(reinterpret_cast<void*>(gObjects.address));
        SDK::FName::InitManually(reinterpret_cast<void*>(appendString.address));

        // GNames and GWorld stop at the log line above. The SDK never reads
        // GNames (FName goes through AppendString), and UWorld::GetWorld()
        // reads Offsets::GWorld through an `if constexpr`, so there is nothing
        // to hand either of them to.
        ModLoaderLogger::LogInfo(L"[SdkGlobals] SDK now uses the resolved GObjects and FName::AppendString");
    }
}

#else

namespace SdkGlobals
{
    void ResolveAndApply() {}
}

#endif
