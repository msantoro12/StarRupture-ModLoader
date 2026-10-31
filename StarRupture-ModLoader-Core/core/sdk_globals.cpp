#include "sdk_globals.h"

#if defined(MODLOADER_CLIENT_BUILD)

#include "CoreUObject_classes.hpp"
#include "../hooks/game/scan_patterns.h"
#include "../logging/logger.h"
#include "../memory_scanner/pattern_preflight.h"
#include "../memory_scanner/scan_validation.h"

#include <algorithm>
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

        // Set by ResolveAndApply on the init thread before any hook is
        // installed, and by VerifyGObjects on the same thread; read-only
        // afterwards.
        bool                    g_gObjectsOverridden = false;
        std::vector<Resolution> g_resolutions;

        // The registry row for a pattern, so the request is the one preflight
        // verified -- the same transform, the same kind check.
        const ScanPatterns::PreflightEntry* FindRegistryEntry(const char* pattern)
        {
            for (const ScanPatterns::PreflightEntry& entry : ScanPatterns::PreflightRegistry)
            {
                if (entry.pattern == pattern)
                    return &entry;
            }
            return nullptr;
        }

        // Resolves one global through its registry row. False (and logged) on
        // any miss; preflight has already logged the reason for the row.
        bool Resolve(Global& global)
        {
            const ScanPatterns::PreflightEntry* entry = FindRegistryEntry(global.pattern);
            if (!entry)
            {
                ModLoaderLogger::LogError(L"[SdkGlobals] %S: no preflight registry row for its pattern", global.name);
                return false;
            }

            const ScanValidation::Result result = PatternPreflight::Resolve(*entry);
            if (!result.Succeeded())
            {
                ModLoaderLogger::LogWarn(L"[SdkGlobals] %S: did not resolve (see \"%S\" in the preflight above)",
                    global.name, entry->name);
                return false;
            }

            global.address = result.address;
            return true;
        }

        // A global the SDK is handed: a difference from Offsets:: is the thing
        // worth noticing.
        void LogApplied(const Global& global, uintptr_t base)
        {
            const unsigned long long rva = global.address - base;
            ModLoaderLogger::LogInfo(L"[SdkGlobals] %-12S base+0x%llX  SDK Offsets::%S base+0x%X  %s",
                global.name, rva, global.name, global.sdkOffset,
                rva == static_cast<unsigned long long>(global.sdkOffset) ? L"(same)" : L"(DIFFERENT)");
        }

        // A global that is only logged. The SDK's Offsets:: value for these is
        // not the engine global at all (GNames is TextFormatter, GWorld a field
        // of a static singleton that tracks the world), so no comparison.
        void LogOnly(const Global& global, uintptr_t base)
        {
            ModLoaderLogger::LogInfo(L"[SdkGlobals] %-12S base+0x%llX  (log only; the SDK does not take it)",
                global.name, static_cast<unsigned long long>(global.address - base));
        }

        struct ObjectArrayProbe
        {
            int32_t num;
            int32_t sampled;    // non-null objects among the first kProbeObjects
            int32_t badIndex;   // first index whose object fails a check, -1 if none
        };

        constexpr int32_t kProbeObjects = 256;

        // Structural checks on an object array, under SEH so a wrong address
        // reads as "no" instead of a crash. Engine-registered objects fill the
        // first slots of GUObjectArray before engine init returns and are never
        // freed, and each one records its own slot in UObject::Index, so a
        // wrong-but-plausible address fails here almost at once.
        bool ProbeObjectArray(const SDK::TUObjectArray* array, ObjectArrayProbe* out)
        {
            out->num = 0;
            out->sampled = 0;
            out->badIndex = -1;

            __try
            {
                if (!array || !array->Objects)
                    return false;

                const int32_t num = array->NumElements;
                out->num = num;
                // Enough chunks for the objects, no more than the maximum: an
                // array pre-allocated by gc.PreAllocateUObjectArray has all of
                // them from the start.
                if (num <= 0 || num > array->MaxElements || array->NumChunks > array->MaxChunks
                    || array->NumChunks < (num + SDK::TUObjectArray::ElementsPerChunk - 1) / SDK::TUObjectArray::ElementsPerChunk)
                    return false;

                const int32_t count = (std::min)(num, kProbeObjects);
                for (int32_t i = 0; i < count; ++i)
                {
                    const SDK::UObject* object = array->GetByIndex(i);
                    if (!object)
                        continue;

                    if (object->Index != i || !object->Class || !object->Class->Class)
                    {
                        out->badIndex = i;
                        return false;
                    }
                    ++out->sampled;
                }
                return out->sampled >= count / 2;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        bool CheckObjectArray(const wchar_t* which, uintptr_t address)
        {
            ObjectArrayProbe probe{};
            const bool ok = ProbeObjectArray(reinterpret_cast<const SDK::TUObjectArray*>(address), &probe);
            if (ok)
            {
                ModLoaderLogger::LogInfo(L"[SdkGlobals] GObjects (%s) looks valid: %d objects, %d of the first %d checked",
                    which, probe.num, probe.sampled, (std::min)(probe.num, kProbeObjects));
            }
            else
            {
                ModLoaderLogger::LogError(L"[SdkGlobals] GObjects (%s) at base+0x%llX does not look like the object array (count %d, first bad slot %d)",
                    which, static_cast<unsigned long long>(address - reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))),
                    probe.num, probe.badIndex);
            }
            return ok;
        }
    }

    void ResolveAndApply()
    {
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));

        Global gObjects     { "GObjects",     ScanPatterns::GObjects_PreAllocate,  SDK::Offsets::GObjects };
        Global gNames       { "GNames",       ScanPatterns::GNames_NamePoolData,   SDK::Offsets::GNames };
        Global appendString { "AppendString", ScanPatterns::FName_AppendString,    SDK::Offsets::AppendString };
        Global gWorld       { "GWorld",       ScanPatterns::GWorld_EngineLoopTick, SDK::Offsets::GWorld };

        // Each override stands on its own: a miss on one row says nothing
        // about another row's bytes. Any miss keeps the SDK on Offsets:: for
        // that global only, exactly as before.
        if (Resolve(gObjects))
        {
            LogApplied(gObjects, base);
            SDK::UObject::GObjects.InitManually(reinterpret_cast<void*>(gObjects.address));
            g_gObjectsOverridden = true;
        }
        if (Resolve(appendString))
        {
            LogApplied(appendString, base);
            SDK::FName::InitManually(reinterpret_cast<void*>(appendString.address));
        }

        // GNames and GWorld stop at the log line. The SDK never reads GNames
        // (FName goes through AppendString), and UWorld::GetWorld() reads
        // Offsets::GWorld through an `if constexpr`, so there is nothing to
        // hand either of them to.
        if (Resolve(gNames))
            LogOnly(gNames, base);
        if (Resolve(gWorld))
            LogOnly(gWorld, base);

        for (const Global* global : { &gObjects, &gNames, &appendString, &gWorld })
        {
            g_resolutions.push_back(Resolution{ global->name, global->address != 0,
                global->address ? global->address - base : 0, global->sdkOffset, false });
        }

        ModLoaderLogger::LogInfo(L"[SdkGlobals] SDK uses the %s GObjects and the %s FName::AppendString",
            g_gObjectsOverridden ? L"resolved" : L"built-in",
            appendString.address ? L"resolved" : L"built-in");
    }

    bool VerifyGObjects()
    {
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        const uintptr_t builtIn = base + SDK::Offsets::GObjects;

        if (!g_gObjectsOverridden)
            return CheckObjectArray(L"built-in", builtIn);

        const uintptr_t resolved = reinterpret_cast<uintptr_t>(SDK::UObject::GObjects.GetTypedPtr());
        if (CheckObjectArray(L"resolved", resolved))
            return true;

        // A unique match that found the wrong .data address. Go back to the
        // SDK's own value, the policy for every other kind of miss.
        SDK::UObject::GObjects.InitManually(reinterpret_cast<void*>(builtIn));
        g_gObjectsOverridden = false;
        for (Resolution& entry : g_resolutions)
        {
            if (std::strcmp(entry.name, "GObjects") == 0)
                entry.rejected = true;
        }
        ModLoaderLogger::LogWarn(L"[SdkGlobals] Falling back to the built-in Offsets::GObjects");
        return CheckObjectArray(L"built-in", builtIn);
    }

    const std::vector<Resolution>& Resolutions()
    {
        return g_resolutions;
    }
}

#else

namespace SdkGlobals
{
    void ResolveAndApply() {}
    bool VerifyGObjects() { return true; }
}

#endif
