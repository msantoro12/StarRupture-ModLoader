#pragma once

// Startup preflight for the modloader's own scan patterns.
//
// Scans every entry in ScanPatterns::PreflightRegistry against the main
// module before any hook is installed. Every failure is logged to the
// modloader log. If any *required* pattern is missing, the modloader must
// not install any hooks at all -- callers check the return value and abort
// initialization entirely so the game never runs partially hooked.
//
// A successful preflight also warms the scan cache, so the per-hook scans
// that follow are near-free cache hits.

#include "memory_scanner/scan_validation.h"

#include <string>

namespace ScanPatterns { struct PreflightEntry; }

namespace PatternPreflight
{
    // Returns true if every required modloader pattern was found.
    // On failure, if outFailureDetails is non-null it receives a CRLF-separated
    // human-readable summary of the missing patterns, suitable for display in
    // the crash dialog.
    bool VerifyAllPatterns(std::wstring* outFailureDetails = nullptr);

    // The request preflight makes for one registry row. Anything that resolves
    // a registry pattern after preflight builds its request here, so the
    // address it gets is the one preflight verified, transforms included.
    ScanValidation::Request MakeRequest(const ScanPatterns::PreflightEntry& entry);
}
