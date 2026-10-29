#pragma once

// The Game-SDK's build-specific globals, found by pattern.
//
// The SDK carries GObjects, GNames, FName::AppendString and GWorld as fixed
// offsets (Offsets:: in Basic.hpp) generated for one game build. This finds
// them through the code that uses them instead (ScanPatterns, "Game-SDK
// globals"), logs each next to the SDK's value, and hands the SDK the ones it
// takes at run time.
//
// Called once from the init thread, after the pattern preflight and before
// any hook is installed. The game's main thread is still held then, so no
// SDK call has run yet and nothing can race the hand-over.
//
// Client builds only; on a server build this does nothing and the SDK keeps
// its Offsets:: values.
namespace SdkGlobals
{
    // Resolves all four and logs each against Offsets::. Only when all four
    // resolve does it pass GObjects and FName::AppendString to the SDK
    // (InitManually); any miss is logged and the SDK keeps Offsets::, exactly
    // as before. GNames and GWorld are logged only -- the SDK has no run-time
    // entry point for either.
    void ResolveAndApply();
}
