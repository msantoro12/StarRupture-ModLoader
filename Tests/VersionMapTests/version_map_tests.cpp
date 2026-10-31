// VersionMapTests: the loader's map keys (memory_scanner/version_map.cpp)
// against a fixture map (fixture/hooks/game/version_map.generated.h), and the
// loader's real map against its registry (hooks/game/scan_patterns.h).
#include "test_doubles.h"

#include "hooks/game/scan_patterns.h"
#include "memory_scanner/image_info.h"
#include "memory_scanner/version_map.h"
#include "plugins/plugin_interface.h"

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

	Result Key(const char* owner, const Request& request, const std::vector<Candidate>& own = {}, bool nameShared = false)
	{
		TestDoubles::g_log.clear();
		return VersionMap::ResolveMapKey(owner, request, own, kRunning, nameShared);
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

	// Reflection as the field tests see it: ATest is 0x50 bytes, AOther 0x10.
	VersionMap::LiveField FakeLive(const std::string& className, const std::string& member)
	{
		VersionMap::LiveField f;
		if (className == "AOther" && member.empty())
			f.size = 0x10;
		else if (className != "ATest")
			f.error = "no class " + className;
		else if (member.empty())
			f.size = 0x50;
		else if (member == "Reflected")
			f.offset = 0x30, f.size = 8;
		else if (member == "NewName")
			f.offset = 0x44, f.size = 4;
		else
			f.error = "no member " + member;
		return f;
	}

	bool Field(const char* key, const char* className, const char* member, uint32_t flags, uint32_t size,
		const std::vector<VersionMap::FieldCandidate>& own, VersionMap::FieldValue& out, std::string& error)
	{
		TestDoubles::g_log.clear();
		error.clear();
		VersionMap::FieldRequest request;
		request.key         = key;
		request.className   = className;
		request.member      = member;
		request.flags       = flags;
		request.size        = size;
		request.builtCL     = 127004;
		request.builtOffset = 0;
		request.candidates  = own;
		return VersionMap::SelectField("Test", request, kRunning, out, error);
	}

	VersionMap::FieldCandidate FieldRow(uint32_t cl, int32_t offset, uint32_t size)
	{
		VersionMap::FieldCandidate c;
		c.gameCL = cl;
		c.offset = offset;
		c.size   = size;
		return c;
	}
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
	Check(r.outcome == Outcome::MultipleMatches && r.detail.find("AMBIGUOUS") == 0 && Logged(L"AMBIGUOUS"),
		"own row for this exe disagrees: AMBIGUOUS");

	// A global absent row binds the owners it lists, and nobody else.
	r = Key("Listed", Ask("Test::Absent", Kind::FunctionStart, "AB 01"));
	Check(r.outcome == Outcome::NoMatch && Logged(L"loader map: absent on CL-128786 (refused)"), "absent binds a listed owner, no fallback");
	r = Key("Other", Ask("Test::Absent", Kind::FunctionStart, "AB 01"));
	Check(At(r, 0x2100) && Logged(L"for other requests; own pattern"), "absent for another owner: own pattern");

	// Another kind under the key: the map is set aside.
	r = Key("VmapProbe", Ask("Test::Data", Kind::FunctionStart, "AC 05"));
	Check(At(r, 0x3005) && Logged(L"loader map row is for another kind (data); own pattern"), "kind mismatch skips the map");
	r = Key("VmapProbe", Ask("Test::Data", Kind::FunctionStart));
	Check(r.outcome == Outcome::NoMatch && r.detail.find("another kind") != std::string::npos
		&& Logged(L"another kind (data); no pattern or rows of its own (refused)"),
		"kind mismatch on a key-only request: refused");

	// Sites: the owner's key, matched by fingerprint.
	r = Key("VmapProbe", Ask("Site", Kind::InFunction, "AD 01 ?? 02"));
	Check(At(r, 0x4000) && Logged(L"loader map, CL-128786 row"), "site: the row with the request's fp");
	r = Key("VmapProbe", Ask("Site", Kind::InFunction, "AD 01 ?? 03"));
	Check(At(r, 0x4200) && Logged(L"loader map row is for another request (fp "), "site: another fp is set aside");
	r = Key("VmapProbe", Ask("Site", Kind::InFunction));
	Check(r.outcome == Outcome::BadRequest && Logged(L"(refused)"), "site key with no pattern is refused");
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
	Check(r.outcome == Outcome::NoMatch && r.detail.find("no pattern or rows of its own") != std::string::npos
		&& Logged(L"not in the loader's map; no pattern or rows of its own (refused)"), "unknown key, nothing of its own");
	r = Key("VmapProbe", Ask("ModLoader/Test::Site", Kind::FunctionStart, "B1 00"));
	Check(At(r, 0x8000) && Logged(L"not in the loader's map; own pattern"), "a global key with a '/' reads no key");
	r = Key("ModLoader", Ask("Test::MapOnly", Kind::FunctionStart));
	Check(r.outcome == Outcome::BadRequest && Logged(L"(refused)"), "a plugin named ModLoader is refused");

	// Two loaded plugins with one name: neither reads site keys, with one line
	// saying why; global keys are unaffected.
	r = Key("VmapProbe", Ask("Site", Kind::InFunction, "AD 01 ?? 02"), {}, true);
	Check(r.outcome == Outcome::BadRequest && TestDoubles::g_log.size() == 1
		&& Logged(L"[Map] VmapProbe: Site: another loaded plugin is also named VmapProbe, so neither reads site keys (refused)"),
		"shared name: a site key is refused");
	r = Key("VmapProbe", Ask("Test::MapOnly", Kind::FunctionStart), {}, true);
	Check(At(r, 0x1000) && Logged(L"loader map, CL-128786 row"), "shared name: a global key is unaffected");

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
	// Fields: the map's rows first, the class and member match, the class bound.
	{
		VersionMap::SetLiveFieldSource(FakeLive);
		VersionMap::FieldValue v;
		std::string e;
		Check(Field("ATest::Native", "ATest", "Native", PLUGIN_FIELD_NATIVE, 4, {}, v, e) && v.offset == 0x28
			&& Logged(L"ATest::Native: loader map, CL-128786 row (0x28/0x4)"), "field: a native member only the map holds");
		Check(Field("ATest::Native", "ATest", "Native", PLUGIN_FIELD_NATIVE, 4, { FieldRow(128786, 0x28, 4) }, v, e)
			&& v.offset == 0x28, "field: an own row that agrees with the map's");
		Check(!Field("ATest::Native", "ATest", "Native", PLUGIN_FIELD_NATIVE, 4, { FieldRow(127004, 0x24, 4) }, v, e)
			&& e == "the loader map's CL-127004 row says 0x20/0x4, the plugin's own says 0x24/0x4"
			&& Logged(L"ATest::Native: refused: the loader map's CL-127004 row says"), "field: an own row that disagrees, for any build, is refused");
		Check(Field("ATest::Old", "ATest", "Old", PLUGIN_FIELD_NATIVE, 4, { FieldRow(128786, 0x18, 4) }, v, e) && v.offset == 0x18
			&& Logged(L"ATest::Old: CL-128786 row (0x18/0x4)"), "field: an own row for a build the map has no row for");
		Check(Field("ATest::Native", "AOther", "Native", PLUGIN_FIELD_NATIVE, 4, { FieldRow(128786, 0x8, 4) }, v, e) && v.offset == 0x8
			&& Logged(L"loader map rows are for ATest::Native, not AOther::Native; own rows"), "field: a key for another class is set aside");
		Check(!Field("ATest::Native", "ATest", "Other", PLUGIN_FIELD_NATIVE, 4, {}, v, e)
			&& Logged(L"not ATest::Other; own rows") && e.find("no row for CL-128786") == 0, "field: a key for another member is set aside");
		Check(Field("ATest::Renamed", "ATest", "NewName", 0, 4, {}, v, e) && v.offset == 0x44
			&& Logged(L"loader map, CL-128786 row, as reflected (0x44/0x4)"), "field: a key whose row records the rename");
		Check(!Field("ATest::Reflected", "ATest", "Reflected", 0, 8, {}, v, e)
			&& e == "the loader map, CL-128786 row says 0x38/0x8, the game says 0x30/0x8", "field: a map row reflection disagrees with");
		Check(!Field("ATest::Past", "ATest", "Past", PLUGIN_FIELD_NATIVE, 8, { FieldRow(128786, 0x4C, 8) }, v, e)
			&& e == "CL-128786 row gives 0x4C/0x8, past the end of ATest (0x50 bytes on this build)", "field: native past the class's end");
		Check(Field("ATest::Past", "ATest", "Past", PLUGIN_FIELD_NATIVE, 8, { FieldRow(128786, 0x48, 8) }, v, e) && v.offset == 0x48,
			"field: native ending at the class's end");
		Check(!Field("ANone::X", "ANone", "X", PLUGIN_FIELD_NATIVE, 4, { FieldRow(128786, 0, 4) }, v, e)
			&& e == "reflection, for the size of ANone: no class ANone", "field: native in a class reflection does not know");
		VersionMap::SetLiveFieldSource(nullptr);
		Check(!Field("ATest::Native", "ATest", "Native", PLUGIN_FIELD_NATIVE, 4, {}, v, e) && e.find("cannot be bounded") != std::string::npos,
			"field: no reflection reader, no native member");
	}

	Check(Real::VersionMap::kFormatVersion == 2, "the real map is format 2");
	Check(unmatched == 0, "every real map key is a registry row's");
	Check(wrong == 0, "every real map row has its registry row's kind and fp");

	printf("%d checks, %d failed\n", g_checks, g_failed);
	return g_failed ? 1 : 0;
}
