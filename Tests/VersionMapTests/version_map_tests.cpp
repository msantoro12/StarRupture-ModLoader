// VersionMapTests: the loader's map keys (memory_scanner/version_map.cpp)
// against a fixture map (fixture/hooks/game/version_map.generated.h), and the
// loader's real map against its registry (hooks/game/scan_patterns.h).
#include "test_doubles.h"

#include "hooks/game/scan_patterns.h"
#include "memory_scanner/image_info.h"
#include "memory_scanner/version_map.h"

#include <array>
#include <cstdint>
#include <cstdio>

// The real map, beside the fixture one version_map.cpp reads (its includes are already in).

namespace Real
{
#include "../../StarRupture-ModLoader-Core/hooks/game/version_map.generated.h"
}

using ScanValidation::Kind;
using ScanValidation::Outcome;
using ScanValidation::Request;
using ScanValidation::Result;
using VersionMap::Candidate;

namespace
{
	int g_checks = 0;
	int g_failed = 0;

	void Check(bool ok, const char* what)
	{
		++g_checks;
		if (!ok)
		{
			++g_failed;
			printf("FAIL: %s\n", what);
		}
	}

	uintptr_t Base() { return Scanner::GetImageInfo(GetModuleHandleW(nullptr)).base; }

	const VersionMap::Exe kRunning{ 128786, 0x22222222, 0x02000000 };

	bool Logged(const wchar_t* text)
	{
		for (const std::wstring& line : TestDoubles::g_log)
			if (line.find(text) != std::wstring::npos)
				return true;
		return false;
	}

	Request Ask(const char* name, Kind kind, const char* pattern = "")
	{
		Request r;
		r.name    = name;
		r.kind    = kind;
		r.pattern = pattern;
		return r;
	}

	Result Key(const char* owner, const Request& request, const std::vector<Candidate>& own = {})
	{
		TestDoubles::g_log.clear();
		return VersionMap::ResolveMapKey(owner, request, own, kRunning);
	}

	Candidate AddressRow(uint32_t rva)
	{
		Candidate c;
		c.gameCL     = kRunning.cl;
		c.imageStamp = kRunning.imageStamp;
		c.imageSize  = kRunning.imageSize;
		c.rva        = rva;
		return c;
	}

	bool At(const Result& r, uintptr_t rva) { return r.Succeeded() && r.address == Base() + rva; }
}

int main()
{
	TestDoubles::g_rvas = {
		{ "AA 01", 0x1000 }, { "AB 01", 0x2100 }, { "AC 05", 0x3005 }, { "AD 01 ?? 02", 0x4000 },
		{ "AD 01 ?? 03", 0x4200 }, { "AE 09", 0x5000 }, { "AF 01", 0x6100 }, { "AF 09", 0x6000 }, { "B0 01", 0x7000 },
		{ "B1 00", 0x8000 },
	};

	// Fingerprint test vectors: a plugin request of kind 2 with resultOffset -4
	// and followRel32At 3, without and with FOLLOW_REL32.
	Check(VersionMap::Fingerprint(2, "48 8b 05 ? ??  ?? ??", -4, -1, -1, 0) == 0xB3405899, "fp vector, no rel32 follow");
	Check(VersionMap::Fingerprint(2, "48 8b 05 ? ??  ?? ??", -4, 3, -1, 0) == 0x2D1AC6AE, "fp vector, rel32 follow");
	Check(VersionMap::Fingerprint(2, "48 8B 05 ?? ?? ?? ??", -4, -1, -1, 0) == 0xB3405899, "fp reads the pattern as ParsePattern does");
	Check(VersionMap::Fingerprint(2, "", 0, -1, -1, 0) == 0, "fp of no pattern is 0");
	Check(VersionMap::Fingerprint(2, "48 zz", 0, -1, -1, 0) == 0, "fp of a pattern that does not parse is 0");
	Check(VersionMap::Fingerprint(2, "B0 01", 0, -1, -1, 1) == 0x0BD817FA, "fp carries the arithmetic revision");
	Check(VersionMap::Fingerprint(2, "B0 01", 0, -1, -1, 0) != 0x0BD817FA, "fp changes with the revision");

	// A global key only the map holds; its row for the running build wins.
	Result r = Key("VmapProbe", Ask("Test::MapOnly", Kind::FunctionStart));
	Check(At(r, 0x1000) && Logged(L"loader map, CL-128786 row"), "global key: the map's row for this build");

	// The map's row for this build comes before the plugin's own row for it.
	Candidate ownRow = AddressRow(0);
	ownRow.imageStamp = 0x33333333;   // another exe of this build
	ownRow.pattern    = "AB 01";
	r = Key("VmapProbe", Ask("Test::MapOnly", Kind::FunctionStart, "AB 01"), { ownRow });
	Check(At(r, 0x1000) && Logged(L"loader map, CL-128786 row") && !Logged(L"own CL-128786 row"),
		"the map's row first: the own row for this build is not tried");

	// The plugin's own address-only row for this exe: agrees, or AMBIGUOUS.
	r = Key("VmapProbe", Ask("Test::MapOnly", Kind::FunctionStart), { AddressRow(0x1000) });
	Check(At(r, 0x1000) && Logged(L"the own CL-128786 row agrees"), "own row for this exe agrees with the map");
	r = Key("VmapProbe", Ask("Test::MapOnly", Kind::FunctionStart), { AddressRow(0x1010) });
	Check(r.outcome == Outcome::MultipleMatches && r.detail.find("AMBIGUOUS") == 0, "own row for this exe disagrees: AMBIGUOUS");

	// A global absent row binds the owners it lists, and nobody else.
	r = Key("Listed", Ask("Test::Absent", Kind::FunctionStart, "AB 01"));
	Check(r.outcome == Outcome::NoMatch && Logged(L"loader map: absent on CL-128786 (refused)"), "absent binds a listed owner, no fallback");
	r = Key("Other", Ask("Test::Absent", Kind::FunctionStart, "AB 01"));
	Check(At(r, 0x2100) && Logged(L"for other requests; own pattern"), "absent for another owner: own pattern");

	// Another kind under the key: the map is set aside.
	r = Key("VmapProbe", Ask("Test::Data", Kind::FunctionStart, "AC 05"));
	Check(At(r, 0x3005) && Logged(L"loader map row is for another kind (data); own pattern"), "kind mismatch skips the map");
	r = Key("VmapProbe", Ask("Test::Data", Kind::FunctionStart));
	Check(r.outcome == Outcome::NoMatch && r.detail.find("another kind") != std::string::npos,
		"kind mismatch on a key-only request: refused");

	// Sites: the owner's key, matched by fingerprint.
	r = Key("VmapProbe", Ask("Site", Kind::InFunction, "AD 01 ?? 02"));
	Check(At(r, 0x4000) && Logged(L"loader map, CL-128786 row"), "site: the row with the request's fp");
	r = Key("VmapProbe", Ask("Site", Kind::InFunction, "AD 01 ?? 03"));
	Check(At(r, 0x4200) && Logged(L"loader map row is for another request (fp "), "site: another fp is set aside");
	r = Key("VmapProbe", Ask("Site", Kind::InFunction));
	Check(r.outcome == Outcome::BadRequest, "site key with no pattern is refused");
	r = Key("OtherPlugin", Ask("Site", Kind::InFunction, "AD 01 ?? 03"));
	Check(At(r, 0x4200) && Logged(L"not in the loader's map (OtherPlugin/Site, fp "), "another owner's site is not this plugin's key");
	r = Key("VmapProbe", Ask("AbsentSite", Kind::InFunction, "AF 01"));
	Check(r.outcome == Outcome::NoMatch && Logged(L"(refused)"), "site absent with this fp: refused, though the own pattern matches");
	r = Key("VmapProbe", Ask("AbsentSite", Kind::InFunction, "AF 09"));
	Check(At(r, 0x6000), "site absent under another fp: this fp's row");
	r = Key("VmapProbe", Ask("OtherFp", Kind::InFunction, "AE 09"));
	Check(At(r, 0x5000) && Logged(L"no row for CL-128786; own pattern, loader map, CL-127004 row agree"), "no row for this build: agreement");
	// No row for this build, a row for another build with another fp whose
	// pattern resolves here, and an own pattern that does not match: a failure,
	// never the other request's address.
	r = Key("VmapProbe", Ask("OtherFp", Kind::InFunction, "AE 01"));
	Check(!r.Succeeded() && r.address != Base() + 0x5000 && Logged(L"loader map row is for another request (fp "),
		"site: another fp's row is not used, the own pattern decides");

	// Keys the map does not hold, and names it refuses.
	r = Key("VmapProbe", Ask("Test::Nope", Kind::FunctionStart, "B1 00"));
	Check(At(r, 0x8000) && Logged(L"not in the loader's map; own pattern"), "unknown global key: own pattern");
	r = Key("VmapProbe", Ask("Test::Nope", Kind::FunctionStart));
	Check(r.outcome == Outcome::NoMatch && r.detail.find("no pattern or rows of its own") != std::string::npos, "unknown key, nothing of its own");
	r = Key("VmapProbe", Ask("ModLoader/Test::Site", Kind::FunctionStart, "B1 00"));
	Check(At(r, 0x8000) && Logged(L"not in the loader's map; own pattern"), "a global key with a '/' reads no key");
	r = Key("ModLoader", Ask("Test::MapOnly", Kind::FunctionStart));
	Check(r.outcome == Outcome::BadRequest, "a plugin named ModLoader is refused");

	// The loader's own site: its key, fingerprint and arithmetic filter.
	ScanPatterns::PreflightEntry site{};
	site.name           = "Test::Site";
	site.pattern        = "B0 01";
	site.kind           = ScanPatterns::PatternKind::InFunction;
	site.siteArithmetic = 1;
	Check(VersionMap::LoaderEntryKey(site) == "ModLoader/Test::Site", "loader site key");
	Check(VersionMap::LoaderEntryFingerprint(site) == 0x0BD817FA, "loader site fp");
	TestDoubles::g_log.clear();
	r = VersionMap::ResolveLoaderEntry(site, Ask("Test::Site", Kind::InFunction, "B0 01"), kRunning);
	Check(At(r, 0x7000) && Logged(L"is not this site's own pattern") && Logged(L"own pattern, loader map, CL-127004 row agree"),
		"loader site: a new pattern is not used, the built row stands in");

	// The real map against the registry: every key is a registry row's, with its kind and fingerprint.
	int unmatched = 0, wrong = 0;
	for (const Real::VersionMap::Index& index : Real::VersionMap::kFunctions)
	{
		const ScanPatterns::PreflightEntry* entry = nullptr;
		for (const ScanPatterns::PreflightEntry& e : ScanPatterns::PreflightRegistry)
			if (VersionMap::LoaderEntryKey(e) == index.key)
				entry = &e;
		if (!entry)
		{
			++unmatched;
			printf("  not a registry key: %s\n", index.key);
			continue;
		}
		for (uint16_t i = index.first; i < index.first + index.count; ++i)
		{
			const Real::VersionMap::FunctionRow& row = Real::VersionMap::kFunctionRows[i];
			if (row.kind != static_cast<int>(entry->kind) || row.fp != VersionMap::LoaderEntryFingerprint(*entry))
			{
				++wrong;
				printf("  wrong kind or fp: %s\n", index.key);
			}
		}
	}
	Check(Real::VersionMap::kFormatVersion == 2, "the real map is format 2");
	Check(unmatched == 0, "every real map key is a registry row's");
	Check(wrong == 0, "every real map row has its registry row's kind and fp");

	printf("%d checks, %d failed\n", g_checks, g_failed);
	return g_failed ? 1 : 0;
}
