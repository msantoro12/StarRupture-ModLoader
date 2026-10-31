#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace SDK
{
    class ACrCrafter;
    class AGameStateBase;
    class UCrCraftingComponent;
    class ULineBatchComponent;
    class UNetDriver;
    class UWorld;
}

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

    // The current world, for the loader's own code; use it in place of
    // SDK::UWorld::GetWorld(). The SDK reads Offsets::GWorld, an address that
    // holds only on the build the SDK was dumped for (VersionMap::kBuiltCL).
    // On any other build this reads the engine's GWorld that ResolveAndApply
    // found, and gives null when it found none. The server build, which runs
    // on kBuiltCL only, keeps the SDK's getter.
    SDK::UWorld* GetWorld();

    // True when the SDK walks objects and names through the GObjects and
    // FName::AppendString that ResolveAndApply found (and VerifyGObjects
    // kept), not through Offsets::. Offsets:: holds only on the SDK's own
    // build, so on any other build plugins must not run without this.
    bool SdkOnResolvedGlobals();

    // ---- Members the loader reads, resolved per build ---------------------
    //
    // The game-class members the loader reads at an offset that can move
    // between game builds. Each is read from the running game's reflection
    // data, and the loader's version map row for that build
    // (hooks/game/version_map.generated.h, kFields) must agree with it
    // (VersionMap::SelectField). The layout check leaves them out
    // (IsPerBuildField).

    // Resolves them, once, on the init thread after the layout check and
    // before any plugin loads. Returns "key: reason" for each one that did
    // not resolve. On VersionMap::kBuiltCL a member that does not resolve
    // keeps the SDK's compiled offset (logged), so only another build can
    // return any. The server build, which runs on kBuiltCL only, takes the
    // compiled offsets.
    std::vector<std::string> ResolveFields();

    // True for a member ResolveFields resolves (className is the SDK's name).
    bool IsPerBuildField(const char* className, const char* member);

    // The member of the object, or null when the object is null or the
    // member did not resolve on this build (and before ResolveFields).
    SDK::AGameStateBase*       WorldGameState(const SDK::UWorld* world);
    SDK::UNetDriver*           WorldNetDriver(const SDK::UWorld* world);
    SDK::ULineBatchComponent*  WorldLineBatcher(const SDK::UWorld* world, int index);  // index into UWorld::LineBatchers
    SDK::UCrCraftingComponent* CrafterCraftComponent(const SDK::ACrCrafter* crafter);
}
