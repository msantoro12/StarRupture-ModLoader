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

	// The trailing CL-<n> of a ProductVersion, or 0.
	uint32_t ParseCL(const std::wstring& productVersion);

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
}
