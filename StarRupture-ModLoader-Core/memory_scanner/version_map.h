#pragma once

#include "memory_scanner/scan_validation.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ScanPatterns { struct PreflightEntry; }

// ---------------------------------------------------------------------------
// VersionMap -- picks the locator for the game build that is running.
//
// A component that knows where a function sits on more than one game build
// hands every locator it has for it (one per changelist, see
// PluginScanCandidate in plugins/plugin_interface.h). The choice is made here,
// on top of ScanValidation, so every locator gets the same uniqueness and kind
// checks the request's own pattern gets:
//
//   1. Exact. The row whose changelist is the running one. On the very exe the
//      row was verified on (same changelist, PE TimeDateStamp and SizeOfImage),
//      it must land on its recorded RVA: same binary, different answer (another
//      address, or the pattern not found) is a failure with no fallback. On
//      another exe with the same changelist, a row whose pattern is not found
//      is logged and skipped. A row with no pattern is an address for its own
//      exe only, so it needs that exe's stamp and size and an RVA.
//   2. Agreement. Every other locator, the request's own pattern included. At
//      least one must resolve, and every one that resolves must give the same
//      final address. Different addresses are a failure (AMBIGUOUS).
//   3. Otherwise a failure, recorded as a miss.
//
// One "[Map]" line per key says which rule picked the address.
// ---------------------------------------------------------------------------

namespace VersionMap
{
	struct Candidate
	{
		uint32_t    gameCL     = 0;
		uint32_t    imageStamp = 0;   // 0 = unknown
		uint32_t    imageSize  = 0;
		std::string pattern;          // empty = address-only: honoured on the exact exe only
		int32_t     resultOffset    = 0;
		int32_t     followRel32At   = -1;
		int32_t     followRipDispAt = -1;
		uint32_t    rva = 0;          // 0 = none
	};

	// A game exe: the CL from the end of its ProductVersion, the stamp and
	// size from its PE headers.
	struct Exe
	{
		uint32_t cl         = 0;      // 0 = the ProductVersion has no CL-<n>
		uint32_t imageStamp = 0;
		uint32_t imageSize  = 0;
	};

	// The exe mapped at `module`.
	Exe ExeOf(HMODULE module);

	// The running game exe. Read once.
	Exe RunningExe();

	// The trailing CL-<n> of a ProductVersion, or 0 (also for a leading zero).
	uint32_t ParseCL(const std::wstring& productVersion);

	// The game builds this loader runs on, the gate VerifyGameVersion applies.
	// The client runs on every build its map has rows for (kBuilds; CI adds a
	// build only once every one of the loader's patterns resolves on it), the
	// server only on the build it was written for (kBuiltCL). Any other build
	// is refused.
	std::vector<uint32_t> SupportedBuilds();

	// True when the ProductVersion ends in the CL of one of SupportedBuilds().
	bool IsSupported(const std::wstring& productVersion);

	// Whether a plugin may run on `running`. On kBuiltCL every plugin may. On
	// any other build its compiled-in layouts and its SDK's Offsets:: point
	// where kBuiltCL had things, so it runs only when it takes all of them
	// from this build: interface 72 or later, the map in OnPluginLoadHooks
	// (a request with candidates, or ResolveField), and GetSdkGlobals in
	// PluginInit. The loader asks at each of those three points, passing true
	// for what it cannot know yet. Empty = admitted; else the reason, for the
	// log and the plugin list.
	std::string OffBuildRefusal(const Exe& running, int interfaceVersion, bool usedMap, bool askedSdkGlobals);

	// Resolves `request` (the component's own pattern, kind and transforms)
	// with `candidates`, as described above. `owner` names the component in
	// the [Map] line. The result reads like ScanValidation's: a failure's
	// detail says which rule failed and what each locator found.
	ScanValidation::Result Select(const char* owner, const ScanValidation::Request& request,
		const std::vector<Candidate>& candidates, const Exe& running);

	// The loader's own registry row (ScanPatterns::PreflightRegistry), resolved
	// with the rows its map (hooks/game/version_map.generated.h) holds for that
	// name. With no rows, the row's own pattern alone, as before the map. The
	// client only: the server build stays on its exact CL.
	//
	// A row marked siteArithmetic takes map rows only where they carry its own
	// pattern, because its hook site adds offsets that only that pattern's
	// shape makes true.
	ScanValidation::Result ResolveLoaderEntry(const ScanPatterns::PreflightEntry& entry,
		const ScanValidation::Request& request, const Exe& running);

	// What preflight picked for a registry pattern, by its text, so the hook
	// sites that scan the same ScanPatterns:: constant get the same address
	// (0 = none). Set during preflight, on the init thread, before any hook
	// site asks; only rows with no follow and no site arithmetic are kept,
	// because a site's own scan gives the match itself.
	void RememberPick(const ScanPatterns::PreflightEntry& entry, uintptr_t address);
	uintptr_t PickFor(const std::string& pattern);

	// ---- Fields ------------------------------------------------------------
	//
	// Where a member of a game class sits on the running build (see
	// PluginFieldRequest in plugins/plugin_interface.h for what a component
	// asks). A reflected member's offset is read from the running game's own
	// reflection data, and a row for the running changelist must agree with
	// it; a native member (one reflection does not describe) is the running
	// changelist's row, or the compiled offset on the build it was compiled
	// for, and nothing else.

	// One build's value for a field.
	struct FieldCandidate
	{
		uint32_t    gameCL   = 0;
		int32_t     offset   = -1;
		uint32_t    size     = 0;
		uint8_t     boolMask = 0;
		std::string member;           // the name on that build if it was renamed, else empty
	};

	struct FieldRequest
	{
		std::string key;              // "Class::Member", for the [Map] line and the failure
		std::string className;        // short name, or /Script/<Module>.<Name>
		std::string member;           // empty = the size of className itself
		uint32_t    flags       = 0;  // PluginFieldFlags
		uint32_t    size        = 0;
		uint32_t    builtCL     = 0;
		int32_t     builtOffset = -1;
		std::vector<FieldCandidate> candidates;
	};

	struct FieldValue
	{
		int32_t  offset   = -1;
		uint32_t size     = 0;
		uint8_t  boolMask = 0;
	};

	// What the running game reflects for one member (member empty: for the
	// class itself, whose size is its PropertiesSize). error is set when it
	// has no answer: no such class, more than one, or no such member.
	struct LiveField
	{
		std::string error;
		int32_t     offset   = 0;
		uint32_t    size     = 0;
		bool        isBool   = false;
		uint8_t     boolMask = 0;     // FBoolProperty::FieldMask when isBool
	};
	using LiveFieldSource = LiveField (*)(const std::string& className, const std::string& member);

	// The reflection reader SelectField asks. Set once, before the first
	// field is asked for: in the game, LayoutCheck's (core/layout_check.h).
	// With none set, every reflected field fails.
	void SetLiveFieldSource(LiveFieldSource source);

	// Applies the rules above to one request, logs its [Map] line, and on
	// success writes out. On failure, error says which rule failed.
	bool SelectField(const char* owner, const FieldRequest& request, const Exe& running,
		FieldValue& out, std::string& error);

	// The request for one of the loader's own members ("Class::Member", a key
	// of kFields), with the rows its map holds for it. size and builtOffset
	// are the member's in the SDK the loader is built against.
	FieldRequest LoaderFieldRequest(const char* key, uint32_t size, int32_t builtOffset);
}
