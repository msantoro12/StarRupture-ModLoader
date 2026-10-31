#include "memory_scanner/version_map.h"
#include "memory_scanner/image_info.h"
#include "memory_scanner/scan_cache.h"
#include "core/version_check.h"
#include "hooks/game/scan_patterns.h"
#include "hooks/game/version_map.generated.h"
#include "logging/logger.h"
#include "plugins/plugin_interface.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace VersionMap
{
	namespace
	{
		const char* NL = "\r\n";

		std::string Hex(unsigned long long v)
		{
			char buf[32]{};
			sprintf_s(buf, "0x%llX", v);
			return buf;
		}

		std::string ClText(uint32_t cl)
		{
			return cl ? "CL-" + std::to_string(cl) : std::string("an unknown CL");
		}

		ScanValidation::Request WithLocator(const ScanValidation::Request& base, const Candidate& c)
		{
			ScanValidation::Request r = base;
			r.pattern         = c.pattern;
			r.resultOffset    = c.resultOffset;
			r.followRel32At   = c.followRel32At;
			r.followRipDispAt = c.followRipDispAt;
			return r;
		}

		// FNV-1a over everything that decides the agreement verdict: the
		// request's own locator, every candidate, and the exe it ran on.
		struct Hash
		{
			uint64_t h = 1469598103934665603ull;
			void Bytes(const void* p, size_t n)
			{
				const auto* b = static_cast<const uint8_t*>(p);
				for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
			}
			void Text(const std::string& s) { Bytes(s.data(), s.size()); Bytes("", 1); }
			template <typename T> void Value(T v) { Bytes(&v, sizeof(v)); }
		};

		std::string SelectionCacheKey(const ScanValidation::Request& request,
			const std::vector<Candidate>& candidates, const Exe& running)
		{
			Hash h;
			h.Text(request.pattern);
			h.Value(request.resultOffset);
			h.Value(request.followRel32At);
			h.Value(request.followRipDispAt);
			h.Value(static_cast<int>(request.kind));
			h.Value(running.imageStamp);
			h.Value(running.imageSize);
			for (const Candidate& c : candidates)
			{
				h.Value(c.gameCL); h.Value(c.imageStamp); h.Value(c.imageSize);
				h.Text(c.pattern);
				h.Value(c.resultOffset); h.Value(c.followRel32At); h.Value(c.followRipDispAt);
				h.Value(c.rva);
			}
			// Not an AOB, so it can never share a scan_cache.ini entry with one.
			return "vmap " + request.name + " " + Hex(h.h);
		}

		struct Tried
		{
			std::string            label;   // "built pattern", "CL-128786 row"
			ScanValidation::Request request;
			ScanValidation::Result  result;
		};

		void LogPick(const char* owner, const std::string& key, const std::string& how)
		{
			ModLoaderLogger::LogInfo(L"[Map] %S: %S: %S", owner ? owner : "?", key.c_str(), how.c_str());
		}
	}

	uint32_t ParseCL(const std::wstring& productVersion)
	{
		size_t end = productVersion.size();
		size_t start = end;
		while (start > 0 && productVersion[start - 1] >= L'0' && productVersion[start - 1] <= L'9')
			--start;
		if (start == end || start < 3 || productVersion.compare(start - 3, 3, L"CL-") != 0 || end - start > 9
			|| productVersion[start] == L'0')
			return 0;
		uint32_t cl = 0;
		for (size_t i = start; i < end; ++i)
			cl = cl * 10 + static_cast<uint32_t>(productVersion[i] - L'0');
		return cl;
	}

	std::vector<uint32_t> SupportedBuilds()
	{
		std::vector<uint32_t> builds;
#if defined(MODLOADER_CLIENT_BUILD)
		for (const Build& b : kBuilds)
			builds.push_back(b.gameCL);
#else
		builds.push_back(kBuiltCL);
#endif
		return builds;
	}

	bool IsSupported(const std::wstring& productVersion)
	{
		const uint32_t cl = ParseCL(productVersion);
		const std::vector<uint32_t> builds = SupportedBuilds();
		return cl != 0 && std::find(builds.begin(), builds.end(), cl) != builds.end();
	}

	std::string OffBuildRefusal(const Exe& running, int interfaceVersion, bool usedMap, bool askedSdkGlobals)
	{
		constexpr int kFirstMapInterface = 72;   // ResolveField, candidates, GetSdkGlobals
		if (running.cl == kBuiltCL)
			return {};
		std::string why;
		if (interfaceVersion < kFirstMapInterface)
			why = "it was built for plugin interface v" + std::to_string(interfaceVersion) + ", before the version map";
		else if (!usedMap)
			why = "it asks the version map for none of its game offsets";
		else if (!askedSdkGlobals)
			why = "it does not take the Game-SDK globals from the loader (GetSdkGlobals)";
		else
			return {};
		const std::string built = "CL-" + std::to_string(kBuiltCL);
		return "Needs an update to run on CL-" + std::to_string(running.cl) + ": " + why
			+ ", so it would read this game's data where " + built + " had it. It loads on " + built + ".";
	}

	Exe ExeOf(HMODULE module)
	{
		Exe e;
		e.cl = ParseCL(GetGameVersionString());
		const Scanner::ImageInfo image = Scanner::GetImageInfo(module);
		if (image.valid)
		{
			const auto* base = reinterpret_cast<const uint8_t*>(image.base);
			const auto* dos  = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
			const auto* nt   = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
			e.imageStamp = nt->FileHeader.TimeDateStamp;
			e.imageSize  = nt->OptionalHeader.SizeOfImage;
		}
		return e;
	}

	Exe RunningExe()
	{
		static const Exe exe = ExeOf(GetModuleHandleW(nullptr));
		return exe;
	}

	ScanValidation::Result Select(const char* owner, const ScanValidation::Request& request,
		const std::vector<Candidate>& candidates, const Exe& running)
	{
		using ScanValidation::Outcome;
		using ScanValidation::Result;

		HMODULE module = request.module ? request.module : GetModuleHandleW(nullptr);
		const Scanner::ImageInfo image = Scanner::GetImageInfo(module);
		const std::string here = ClText(running.cl);

		// An address-only row is honoured on its own exe only, so it needs all of
		// the exe's stamp and size and the RVA.
		for (const Candidate& c : candidates)
		{
			if (c.pattern.empty() && !(c.imageStamp && c.imageSize && c.rva))
			{
				Result bad;
				bad.outcome = Outcome::BadRequest;
				bad.detail  = "the " + ClText(c.gameCL) + " row has no pattern, so it needs the exe's stamp and size and an RVA";
				return bad;
			}
		}

		// --- 1. Exact -------------------------------------------------------------
		const Candidate* exact = nullptr;
		if (running.cl)
		{
			for (const Candidate& c : candidates)
			{
				if (c.gameCL != running.cl)
					continue;
				if (exact)
				{
					Result bad;
					bad.outcome = Outcome::BadRequest;
					bad.detail  = "two locators for " + here + "; a map has one row per key and build";
					return bad;
				}
				exact = &c;
			}
		}

		const Candidate* skipped = nullptr;
		if (exact)
		{
			const bool sameExe = exact->imageStamp && exact->imageSize
				&& exact->imageStamp == running.imageStamp && exact->imageSize == running.imageSize;

			if (exact->pattern.empty())
			{
				if (sameExe)
				{
					Result r = ScanValidation::CheckAddress(request, image.base + exact->rva);
					if (r.Succeeded())
						LogPick(owner, request.name, here + " row (address on this exe)");
					else
						r.detail = here + " row, address " + Hex(exact->rva) + " on this exe: " + r.detail;
					return r;
				}
				ModLoaderLogger::LogWarn(L"[Map] %S: %S: the %S row gives an address for another exe of that build; trying every other locator",
					owner ? owner : "?", request.name.c_str(), here.c_str());
				skipped = exact;
			}
			else
			{
				Result r = ScanValidation::Resolve(WithLocator(request, *exact));
				if (r.Succeeded())
				{
					const uintptr_t rva = r.address - image.base;
					if (sameExe && exact->rva && rva != exact->rva)
					{
						Result bad;
						bad.outcome  = Outcome::WrongKind;
						bad.rawMatch = r.rawMatch;
						bad.address  = r.address;
						bad.matchCount = r.matchCount;
						bad.detail   = "on the exe the " + here + " row was verified on, it resolved to RVA " + Hex(rva)
						             + " instead of the recorded " + Hex(exact->rva)
						             + ": same exe, different answer, so no other locator is tried."
						             + NL + "            Pattern: " + exact->pattern;
						return bad;
					}
					LogPick(owner, request.name, here + " row");
					return r;
				}
				if (r.outcome != Outcome::NoMatch)
				{
					r.detail = here + " row: " + r.detail;
					return r;
				}
				if (sameExe && exact->rva)
				{
					r.detail = "on the exe the " + here + " row was verified on (RVA " + Hex(exact->rva)
					         + "), its pattern is not found: same exe, different answer, so no other locator is tried."
					         + NL + "            Pattern: " + exact->pattern;
					return r;
				}
				ModLoaderLogger::LogWarn(L"[Map] %S: %S: the %S row's pattern is not in this exe; trying every other locator",
					owner ? owner : "?", request.name.c_str(), here.c_str());
				skipped = exact;
			}
		}

		// --- 2. Agreement -----------------------------------------------------------
		std::vector<Tried> tried;
		tried.push_back({ "built pattern", request, {} });
		for (const Candidate& c : candidates)
		{
			// An address-only row applies to its own exe, which is the exact row's.
			if (&c == skipped || c.pattern.empty())
				continue;
			tried.push_back({ ClText(c.gameCL) + " row", WithLocator(request, c), {} });
		}

		const std::wstring gameVersion = module == GetModuleHandleW(nullptr) ? GetGameVersionString() : std::wstring();
		const std::string  cacheKey    = SelectionCacheKey(request, candidates, running);
		const std::string  lead        = skipped ? "the " + here + " row missed; " : "no row for " + here + "; ";

		// A verdict already reached on this exe: re-check only the locator that
		// won (its bytes, through its own cache entry) and skip the rest.
		ScanCache::Entry cached;
		if (!gameVersion.empty() && ScanCache::TryGet(gameVersion, cacheKey, cached)
			&& cached.matchCount >= 1 && cached.matchCount <= tried.size())
		{
			Tried& t = tried[cached.matchCount - 1];
			const Result r = ScanValidation::Resolve(t.request);
			if (r.Succeeded() && r.address - image.base == cached.offset)
			{
				LogPick(owner, request.name, lead + t.label + " (agreed on an earlier launch)");
				return r;
			}
		}

		size_t winner = 0;
		uintptr_t address = 0;
		bool disagree = false;
		std::string agreed;
		for (size_t i = 0; i < tried.size(); ++i)
		{
			tried[i].result = ScanValidation::Resolve(tried[i].request);
			if (!tried[i].result.Succeeded())
				continue;
			if (!address)
			{
				address = tried[i].result.address;
				winner  = i;
			}
			else if (tried[i].result.address != address)
				disagree = true;
			agreed += agreed.empty() ? tried[i].label : ", " + tried[i].label;
		}

		if (address && !disagree)
		{
			if (!gameVersion.empty())
			{
				ScanCache::Entry entry;
				entry.offset     = address - image.base;
				entry.matchCount = static_cast<uint32_t>(winner + 1);
				ScanCache::Store(gameVersion, cacheKey, entry);
			}
			LogPick(owner, request.name, lead + agreed + (agreed.find(',') == std::string::npos ? " resolves" : " agree"));
			return tried[winner].result;
		}

		Result fail;
		std::string list;
		for (const Tried& t : tried)
		{
			list += NL;
			list += "            " + t.label + ": ";
			if (t.result.Succeeded())
				list += "RVA " + Hex(t.result.address - image.base);
			else
			{
				const std::string& d = t.result.detail;
				list += d.substr(0, d.find('\r'));
			}
		}

		if (disagree)
		{
			fail.outcome = Outcome::MultipleMatches;
			fail.detail  = "AMBIGUOUS: " + lead + "the locators that resolve disagree:" + list;
		}
		else
		{
			fail.outcome = Outcome::NoMatch;
			fail.detail  = lead + "no locator resolves:" + list;
		}
		return fail;
	}

	namespace
	{
		// The map's rows for one registry name, as Select reads them.
		std::vector<Candidate> LoaderRows(const ScanPatterns::PreflightEntry& entry)
		{
			std::vector<Candidate> rows;
#if defined(MODLOADER_CLIENT_BUILD)
			const auto key = std::find_if(kFunctions.begin(), kFunctions.end(),
				[&](const Index& i) { return strcmp(i.key, entry.name) == 0; });
			if (key == kFunctions.end())
				return rows;
			for (uint16_t i = key->first; i < key->first + key->count; ++i)
			{
				const FunctionRow& f = kFunctionRows[i];
				Candidate c;
				c.gameCL     = f.gameCL;
				c.imageStamp = f.imageStamp;
				c.imageSize  = f.imageSize;
				c.rva        = f.rva;

				// On the build the code was written for, the code's own pattern
				// stands in for the row's, so the row's RVA checks the pattern
				// the code ships: one edited in the code fails here instead of
				// passing on the row it replaced.
				if (f.gameCL == kBuiltCL)
				{
					c.pattern         = entry.pattern;
					c.followRel32At   = entry.followRel32At;
					c.followRipDispAt = entry.followRipDispAt;
					rows.push_back(std::move(c));
					continue;
				}
				if (entry.siteArithmetic && (!f.pattern || strcmp(f.pattern, entry.pattern) != 0))
				{
					ModLoaderLogger::LogWarn(L"[Map] ModLoader: %S: the CL-%u row is not this site's own pattern, so it is not used",
						entry.name, f.gameCL);
					continue;
				}
				c.pattern         = f.pattern ? f.pattern : "";
				c.resultOffset    = f.resultOffset;
				c.followRel32At   = f.followRel32At;
				c.followRipDispAt = f.followRipDispAt;
				rows.push_back(std::move(c));
			}
#else
			(void)entry;
#endif
			return rows;
		}

		// Written during preflight only, read after it (see RememberPick).
		std::unordered_map<std::string, uintptr_t> g_picks;
	}

	ScanValidation::Result ResolveLoaderEntry(const ScanPatterns::PreflightEntry& entry,
		const ScanValidation::Request& request, const Exe& running)
	{
		const std::vector<Candidate> rows = LoaderRows(entry);
		if (rows.empty())
			return ScanValidation::Resolve(request);
		return Select("ModLoader", request, rows, running);
	}

	void RememberPick(const ScanPatterns::PreflightEntry& entry, uintptr_t address)
	{
		if (entry.followRel32At < 0 && entry.followRipDispAt < 0 && !entry.siteArithmetic)
			g_picks[entry.pattern] = address;
	}

	uintptr_t PickFor(const std::string& pattern)
	{
		const auto it = g_picks.find(pattern);
		return it == g_picks.end() ? 0 : it->second;
	}
}

namespace VersionMap
{
	namespace
	{
		LiveFieldSource g_liveFields = nullptr;

		std::string FieldText(const FieldValue& v, bool bitfield)
		{
			std::string text = Hex(static_cast<unsigned long long>(v.offset)) + "/" + Hex(v.size);
			if (bitfield)
				text += " mask " + Hex(v.boolMask);
			return text;
		}

		bool SameField(const FieldValue& a, const FieldValue& b, bool bitfield)
		{
			return a.offset == b.offset && a.size == b.size && (!bitfield || a.boolMask == b.boolMask);
		}

		const FieldCandidate* RowFor(const FieldRequest& request, uint32_t cl)
		{
			for (const FieldCandidate& row : request.candidates)
				if (row.gameCL == cl)
					return &row;
			return nullptr;
		}

		FieldValue ValueOf(const FieldCandidate& row)
		{
			return FieldValue{ row.offset, row.size, row.boolMask };
		}
	}

	void SetLiveFieldSource(LiveFieldSource source)
	{
		g_liveFields = source;
	}

	namespace
	{
		bool PickField(const char* owner, const FieldRequest& request, const Exe& running,
			FieldValue& out, std::string& error)
		{
			const bool native   = (request.flags & PLUGIN_FIELD_NATIVE) != 0;
			const bool check    = (request.flags & PLUGIN_FIELD_CHECK) != 0;
			const bool bitfield = (request.flags & PLUGIN_FIELD_BITFIELD) != 0;
			const bool sizeRow  = request.member.empty();

			if (request.className.empty() || (sizeRow && (native || bitfield)))
			{
				error = "bad request: a field needs a class, and a size row is neither native nor a bitfield";
				return false;
			}
			for (size_t i = 0; i < request.candidates.size(); ++i)
				for (size_t j = i + 1; j < request.candidates.size(); ++j)
					if (request.candidates[i].gameCL == request.candidates[j].gameCL)
					{
						error = "bad request: two rows for " + ClText(request.candidates[i].gameCL);
						return false;
					}

			const FieldCandidate* row = running.cl ? RowFor(request, running.cl) : nullptr;
			const bool onBuilt = running.cl != 0 && running.cl == request.builtCL;

			// The compiled value: offsetof and sizeof, or for a bitfield (which has
			// no offsetof) its builtCL row.
			FieldValue built{ request.builtOffset, request.size, 0 };
			bool haveBuilt = !bitfield && !sizeRow;
			if (bitfield)
			{
				if (const FieldCandidate* builtRow = RowFor(request, request.builtCL))
				{
					built     = ValueOf(*builtRow);
					haveBuilt = true;
				}
			}

			FieldValue value;
			std::string how;
			if (native)
			{
				if (row)
				{
					value = ValueOf(*row);
					how   = ClText(row->gameCL) + " row";
				}
				else if (onBuilt && haveBuilt)
				{
					value = built;
					how   = "built offset";
				}
				else
				{
					error = "no row for " + ClText(running.cl) + ", and a native member has no other source";
					return false;
				}
				if (value.size != request.size)
				{
					error = how + " gives size " + Hex(value.size) + ", the code was compiled for " + Hex(request.size);
					return false;
				}
			}
			else
			{
				if (!g_liveFields)
				{
					error = "no reflection data to read a reflected field from";
					return false;
				}
				const std::string member = row && !row->member.empty() ? row->member : request.member;
				const LiveField live = g_liveFields(request.className, member);
				if (!live.error.empty())
				{
					error = "reflection: " + live.error;
					return false;
				}
				if (bitfield && !live.isBool)
				{
					error = "asked for as a bitfield, but the game's member is not a bool";
					return false;
				}
				if (!bitfield && !sizeRow && live.isBool && live.boolMask != 0xFF)
				{
					error = "the game's member is a bitfield bool; ask for it with PLUGIN_FIELD_BITFIELD";
					return false;
				}
				value = FieldValue{ sizeRow ? 0 : live.offset, live.size, bitfield ? live.boolMask : uint8_t(0) };
				if (!sizeRow && value.size != request.size)
				{
					error = "the game's member is " + Hex(value.size) + " bytes, the code was compiled for " + Hex(request.size);
					return false;
				}
				if (row)
				{
					if (!SameField(ValueOf(*row), value, bitfield) && !(sizeRow && row->size == value.size))
					{
						error = "the " + ClText(row->gameCL) + " row says " + FieldText(ValueOf(*row), bitfield)
							+ ", the game says " + FieldText(value, bitfield);
						return false;
					}
					how = ClText(row->gameCL) + " row, as reflected";
				}
				else
				{
					how = "reflected (no row for " + ClText(running.cl) + ")";
				}
			}

			if (value.offset < 0)
			{
				error = how + " gives a negative offset";
				return false;
			}

			if (check)
			{
				const bool same = sizeRow ? value.size == request.size : haveBuilt && SameField(built, value, bitfield);
				if (!same)
				{
					error = !sizeRow && !haveBuilt
						? std::string("a checked bitfield needs its ") + ClText(request.builtCL) + " row"
						: "the code relies on " + (sizeRow ? Hex(request.size) : FieldText(built, bitfield))
							+ ", this build has " + (sizeRow ? Hex(value.size) : FieldText(value, bitfield));
					return false;
				}
				how += ", as compiled";
			}

			LogPick(owner, request.key, how + " (" + (sizeRow ? Hex(value.size) : FieldText(value, bitfield)) + ")");
			out = value;
			return true;
		}
	}

	bool SelectField(const char* owner, const FieldRequest& request, const Exe& running,
		FieldValue& out, std::string& error)
	{
		if (PickField(owner, request, running, out, error))
			return true;
		ModLoaderLogger::LogWarn(L"[Map] %S: %S: refused: %S", owner ? owner : "?", request.key.c_str(), error.c_str());
		return false;
	}

	FieldRequest LoaderFieldRequest(const char* key, uint32_t size, int32_t builtOffset)
	{
		FieldRequest request;
		request.key         = key;
		const size_t split  = request.key.find("::");
		request.className   = request.key.substr(0, split);
		request.member      = split == std::string::npos ? std::string() : request.key.substr(split + 2);
		request.size        = size;
		request.builtCL     = kBuiltCL;
		request.builtOffset = builtOffset;
		const auto index = std::find_if(kFields.begin(), kFields.end(),
			[&](const Index& i) { return strcmp(i.key, key) == 0; });
		if (index != kFields.end())
		{
			for (uint16_t i = index->first; i < index->first + index->count; ++i)
			{
				const FieldRow& row = kFieldRows[i];
				request.candidates.push_back(FieldCandidate{ row.gameCL, row.offset, row.size,
					row.boolMask, row.member ? row.member : "" });
			}
		}
		return request;
	}
}
