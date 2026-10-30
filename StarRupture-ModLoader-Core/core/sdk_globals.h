#pragma once

#include <cstdint>
#include <vector>

// The Game-SDK's build-specific globals, found by pattern.
//
// The SDK carries GObjects, GNames, FName::AppendString and GWorld as fixed
// offsets (Offsets:: in Basic.hpp) generated for one game build. This finds
// them through the code that uses them instead (ScanPatterns, "Game-SDK
// globals"), logs each one, and hands the SDK the ones it takes at run time.
//
// Called once from the init thread, after the pattern preflight and before
// any hook is installed. The game's main thread is still held then, so no
// SDK call has run yet and nothing can race the hand-over.
//
// Client builds only; on a server build these do nothing and the SDK keeps
// its Offsets:: values.
namespace SdkGlobals
{
    // Resolves all four. GObjects and FName::AppendString are each handed to
    // the SDK (InitManually) when their own pattern resolves, and logged
    // against Offsets::; a miss keeps that one on Offsets::, exactly as
    // before. GNames and GWorld are logged only -- the SDK has no run-time
    // entry point for either, and its Offsets:: values for them are not the
    // engine globals.
    void ResolveAndApply();

    // Checks that the GObjects the SDK now uses has the shape of the engine's
    // object array: sane counts, and each of the first objects sitting in its
    // own slot with a class. A resolved address that fails is replaced by
    // Offsets::GObjects and that is checked in turn. False when neither looks
    // right; nothing should walk GObjects then.
    //
    // Needs the engine's object array to be populated, so call it after
    // engine init, with the game thread still parked.
    bool VerifyGObjects();

    // One global as ResolveAndApply found it, for the compatibility report.
    struct Resolution
    {
        const char* name;       // as the SDK's Offsets:: spells it
        bool        resolved;
        uint64_t    rva;        // 0 when not resolved; kept when rejected
        int32_t     sdkOffset;  // Offsets::<name>
        bool        rejected;   // resolved, but VerifyGObjects threw the hit away
                                // and the SDK uses Offsets:: (GObjects only)
    };

    // Every global ResolveAndApply looked for, in order. Client builds only.
    const std::vector<Resolution>& Resolutions();
}
