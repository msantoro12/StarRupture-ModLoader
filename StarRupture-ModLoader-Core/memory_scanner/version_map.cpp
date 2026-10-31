#include "memory_scanner/version_map.h"
#include "memory_scanner/image_info.h"
#include "memory_scanner/scan_cache.h"
#include "core/version_check.h"
#include "logging/logger.h"

#include <cstdio>
#include <cwctype>

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
			const std::vector<Candidate>& candidates, const Build& running)
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
		while (start > 0 && iswdigit(productVersion[start - 1]))
			--start;
		if (start == end || start < 3 || productVersion.compare(start - 3, 3, L"CL-") != 0 || end - start > 9)
			return 0;
		uint32_t cl = 0;
		for (size_t i = start; i < end; ++i)
			cl = cl * 10 + static_cast<uint32_t>(productVersion[i] - L'0');
		return cl;
	}

	Build RunningBuild()
	{
		static const Build build = []
		{
			Build b;
			b.cl = ParseCL(GetGameVersionString());
			HMODULE exe = GetModuleHandleW(nullptr);
			const Scanner::ImageInfo image = Scanner::GetImageInfo(exe);
			if (image.valid)
			{
				const auto* base = reinterpret_cast<const uint8_t*>(image.base);
				const auto* dos  = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
				const auto* nt   = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
				b.imageStamp = nt->FileHeader.TimeDateStamp;
				b.imageSize  = nt->OptionalHeader.SizeOfImage;
			}
			return b;
		}();
		return build;
	}

	ScanValidation::Result Select(const char* owner, const ScanValidation::Request& request,
		const std::vector<Candidate>& candidates, const Build& running)
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
}
