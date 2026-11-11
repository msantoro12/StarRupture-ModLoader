#include "pch.h"
#include "image_trial.h"

#ifdef MODLOADER_CLIENT_BUILD

#include "../UI/imgui_backend.h"
#include "../UI/image_trial_ops.h"
#include "../UI/toast.h"
#include "../hooks/game/object_lookup/object_lookup.h"
#include "../hooks/game/scan_patterns.h"
#include "../memory_scanner/scanner.h"
#include "../plugins/plugin_manager.h"
#include "../plugins/reload_policy.h"
#include "../logging/log.h"
#include "Engine_classes.hpp"
#include "Chimera_classes.hpp"
#include "AuItems_classes.hpp"
#include "AssetRegistry_classes.hpp"
#include "AssetRegistry_parameters.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <tuple>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    using FindByName_t  = SDK::UObject*  (__fastcall*)(SDK::UClass*, SDK::UObject*, const wchar_t*, bool);
    using LoadObject_t  = SDK::UObject*  (__fastcall*)(SDK::UClass*, SDK::UObject*, const wchar_t*, const wchar_t*,
                                                       uint32_t, void*, bool, const void*);
    using FindPackage_t = SDK::UPackage* (__fastcall*)(SDK::UObject*, const wchar_t*);
    using FullyLoad_t   = void           (__fastcall*)(SDK::UPackage*);
    using LoadPackage_t = SDK::UPackage* (__fastcall*)(SDK::UPackage*, const wchar_t*, uint32_t, void*, const void*);

    namespace Ops = ImageTrialOps;

    constexpr size_t kSampleEach      = 20;      // resident and not resident, per part: 40 in all
    constexpr size_t kSampleMax       = 40;
    constexpr size_t kWatchCount      = 20;
    constexpr double kWatchSeconds    = 300.0;
    constexpr size_t kDepsLogged      = 5;
    constexpr int    kDumpItems       = 8;
    constexpr int    kDumpBuildings   = 4;
    constexpr double kMenuDelayMs     = 20000.0;
    constexpr double kWorldDelayMs    = 10000.0;
    constexpr double kHeapWaitMs      = 120000.0;
    constexpr int    kParkTicksMax    = 300;
    constexpr int    kRefusedTicksMax = 120;
    constexpr int    kFenceTicksMax   = 600;
    constexpr int    kBaselineTicks   = 120;
    constexpr int    kPaceRetryMax    = 20;
    constexpr double kScanBudgetMs    = 3.0;
    constexpr int    kResidentPerTick = 300;
    constexpr int    kRetryPasses     = 3;
    constexpr double kRetryGapMs      = 5000.0;
    constexpr double kToastEveryMs    = 5000.0;

    enum class Ph
    {
        Idle, MenuWait, Gate, List, ListRes, MenuSelect, MenuSample, Scan, Retry, MenuDone, MenuAborted,
        WorldWait, WorldGate, WorldRes, WorldSelect, WorldSample, PaceBase, Pace, PaceDrain, PaceSettle,
        WorldDone, Inert
    };

    const char* PhaseName(Ph p)
    {
        static const char* const names[] = { "idle", "menu-wait", "gate", "list", "list-resident", "menu-select",
            "menu-sample", "scan", "retry", "menu-done", "menu-aborted", "world-wait", "world-gate", "world-resident",
            "world-select", "world-sample", "pace-baseline", "pace", "pace-drain", "pace-settle", "world-done", "inert" };
        return names[static_cast<int>(p)];
    }

    // ---- timing ----

    LONGLONG Now() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
    double Ms(LONGLONG a, LONGLONG b) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return 1000.0 * double(b - a) / double(f.QuadPart); }
    LONGLONG InMs(double ms) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return Now() + LONGLONG(ms * double(f.QuadPart) / 1000.0); }

    double Pct(std::vector<double> v, double q)
    {
        if (v.empty()) return 0;
        std::sort(v.begin(), v.end());
        return v[std::min(v.size() - 1, size_t(q * double(v.size())))];
    }

    constexpr double kMB = 1024.0 * 1024.0;

    // ---- engine entry points ----

    struct Engine
    {
        FindByName_t  find     = nullptr;
        LoadObject_t  load     = nullptr;
        FindPackage_t findPkg  = nullptr;
        FullyLoad_t   fullLoad = nullptr;
        LoadPackage_t loadPkg  = nullptr;
        bool          tried    = false;
        bool Ok() const { return find && load && findPkg && fullLoad && loadPkg; }
    };
    Engine g_eng;

    void ResolveEngine()
    {
        if (g_eng.tried) return;
        g_eng.tried    = true;
        g_eng.find     = reinterpret_cast<FindByName_t>(Hooks::ObjectLookup::GetStaticFindObject_ByNameAddress());
        g_eng.load     = reinterpret_cast<LoadObject_t>(Scanner::FindPatternInMainModule("StaticLoadObject", ScanPatterns::StaticLoadObject));
        g_eng.findPkg  = reinterpret_cast<FindPackage_t>(Hooks::ObjectLookup::GetFindPackageAddress());
        g_eng.fullLoad = reinterpret_cast<FullyLoad_t>(Hooks::ObjectLookup::GetUPackage_FullyLoadAddress());
        g_eng.loadPkg  = reinterpret_cast<LoadPackage_t>(Hooks::ObjectLookup::GetLoadPackageAddress());
        LogToFile::Info("[ImgTrial] ENGINE find=%d load=%d findPackage=%d fullyLoad=%d loadPackage=%d", g_eng.find != nullptr,
            g_eng.load != nullptr, g_eng.findPkg != nullptr, g_eng.fullLoad != nullptr, g_eng.loadPkg != nullptr);
    }

    // The faults a bad engine pointer gives. Anything else goes on to the crash handler.
    int Filter(unsigned code)
    {
        return (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR)
            ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
    }

    // Engine calls. SEH only, so nothing in here owns an object that needs unwinding.
    bool FindObjSEH(SDK::UClass* c, SDK::UObject* outer, const wchar_t* p, SDK::UObject** out)
    {
        __try { *out = g_eng.find(c, outer, p, false); return true; }
        __except (Filter(GetExceptionCode())) { *out = nullptr; return false; }
    }

    bool LoadObjSEH(SDK::UClass* c, const wchar_t* p, SDK::UObject** out)
    {
        __try { *out = g_eng.load(c, nullptr, p, nullptr, 0, nullptr, false, nullptr); return true; }
        __except (Filter(GetExceptionCode())) { *out = nullptr; return false; }
    }

    bool FindPkgSEH(const wchar_t* p, SDK::UPackage** out)
    {
        __try { *out = g_eng.findPkg(nullptr, p); return true; }
        __except (Filter(GetExceptionCode())) { *out = nullptr; return false; }
    }

    bool FullyLoadSEH(SDK::UPackage* pkg)
    {
        __try { g_eng.fullLoad(pkg); return true; }
        __except (Filter(GetExceptionCode())) { return false; }
    }

    bool LoadPkgSEH(const wchar_t* p, SDK::UPackage** out)
    {
        __try { *out = g_eng.loadPkg(nullptr, p, 0, nullptr, nullptr); return true; }
        __except (Filter(GetExceptionCode())) { *out = nullptr; return false; }
    }

    // How the brush of an item or building reads: 0 texture, 1 some other object, 2 empty, 3 not found, 4 wrong type, -1 fault.
    int ResolveBrushSEH(bool building, SDK::UPackage* pkg, const wchar_t* name, SDK::UObject** outRes)
    {
        *outRes = nullptr;
        __try
        {
            if (building)
            {
                SDK::UObject* o = g_eng.find(SDK::UCrBuildingData::StaticClass(), pkg, name, false);
                if (!o) return 3;
                if (!o->IsA(SDK::UCrBuildingData::StaticClass())) return 4;
                *outRes = static_cast<SDK::UCrBuildingData*>(o)->Icon.ResourceObject;
            }
            else
            {
                SDK::UObject* o = g_eng.find(SDK::UClass::StaticClass(), pkg, name, false);
                if (!o) return 3;
                SDK::UObject* cdo = static_cast<SDK::UClass*>(o)->ClassDefaultObject;
                if (!cdo) return 3;
                if (!cdo->IsA(SDK::UAuItemDataBase::StaticClass())) return 4;
                *outRes = static_cast<SDK::UAuItemDataBase*>(cdo)->ItemIcon.ResourceObject;
            }
            if (!*outRes) return 2;
            return (*outRes)->IsA(SDK::UTexture2D::StaticClass()) ? 0 : 1;
        }
        __except (Filter(GetExceptionCode())) { *outRes = nullptr; return -1; }
    }

    struct Raw { int impX, impY, firstMip, neverStream, lod, vts, comp; };

    bool ReadRawSEH(SDK::UTexture2D* t, Raw* out)
    {
        __try
        {
            out->impX = t->ImportedSize.X; out->impY = t->ImportedSize.Y;
            out->firstMip = t->FirstResourceMemMip; out->neverStream = t->NeverStream;
            out->lod = (int)t->LODGroup; out->vts = t->VirtualTextureStreaming; out->comp = (int)t->CompressionSettings;
            return true;
        }
        __except (Filter(GetExceptionCode())) { return false; }
    }

    void FullNameInto(SDK::UObject* o, std::string* out) { *out = o->GetFullName(); }

    bool PathSEH(SDK::UObject* o, std::string* out)
    {
        __try { FullNameInto(o, out); return true; }
        __except (Filter(GetExceptionCode())) { return false; }
    }

    PluginTextureHandle CopySEH(IPluginImGuiTextures* api, SDK::UTexture2D* t, const char* name)
    {
        __try { return api->LoadFromUTexture2D(t, name); }
        __except (Filter(GetExceptionCode())) { return nullptr; }
    }

    // ---- asset registry (the item scan BetterCheats uses) ----

    SDK::IAssetRegistry* GetAssetRegistry()
    {
        SDK::UAssetRegistryHelpers* cdo = SDK::UAssetRegistryHelpers::GetDefaultObj();
        if (!cdo) return nullptr;
        static SDK::UFunction* func = nullptr;
        if (!func)
            func = SDK::UAssetRegistryHelpers::StaticClass()->GetFunction("AssetRegistryHelpers", "GetAssetRegistry");
        if (!func) return nullptr;
        SDK::Params::AssetRegistryHelpers_GetAssetRegistry parms{};
        const auto flags = func->FunctionFlags;
        func->FunctionFlags |= 0x400;
        cdo->ProcessEvent(func, &parms);
        func->FunctionFlags = flags;
        SDK::UObject* registryObject = parms.ReturnValue.GetObjectRef();
        return registryObject ? reinterpret_cast<SDK::IAssetRegistry*>(registryObject) : nullptr;
    }

    bool GetAssetsByClass(const wchar_t* scriptPackage, const wchar_t* className, SDK::TArray<SDK::FAssetData>& out)
    {
        SDK::IAssetRegistry* registry = GetAssetRegistry();
        SDK::UObject* registryObject = registry ? registry->AsUObject() : nullptr;
        if (!registryObject) return false;
        static SDK::UFunction* func = nullptr;
        if (!func)
            func = SDK::IAssetRegistry::StaticClass()->GetFunction("AssetRegistry", "GetAssetsByClass");
        if (!func) return false;
        SDK::FString packagePath(scriptPackage);
        SDK::FString cls(className);
        SDK::Params::AssetRegistry_GetAssetsByClass parms{};
        parms.ClassPathName     = SDK::UKismetSystemLibrary::MakeTopLevelAssetPath(packagePath, cls);
        parms.bSearchSubClasses = true;
        const auto flags = func->FunctionFlags;
        func->FunctionFlags |= 0x400;
        registryObject->ProcessEvent(func, &parms);
        func->FunctionFlags = flags;
        out = std::move(parms.OutAssetData);
        return parms.ReturnValue;
    }

    // The hard references of one package, as the registry knows them. Loads nothing.
    bool GetHardDependencies(SDK::FName packageName, std::vector<std::string>* out)
    {
        SDK::IAssetRegistry* registry = GetAssetRegistry();
        SDK::UObject* registryObject = registry ? registry->AsUObject() : nullptr;
        if (!registryObject) return false;
        static SDK::UFunction* func = nullptr;
        if (!func)
            func = SDK::IAssetRegistry::StaticClass()->GetFunction("AssetRegistry", "K2_GetDependencies");
        if (!func) return false;
        SDK::Params::AssetRegistry_K2_GetDependencies parms{};
        parms.PackageName = packageName;
        parms.DependencyOptions.bIncludeHardPackageReferences = true;
        const auto flags = func->FunctionFlags;
        func->FunctionFlags |= 0x400;
        registryObject->ProcessEvent(func, &parms);
        func->FunctionFlags = flags;
        for (int i = 0; i < parms.OutDependencies.Num(); ++i)
            out->push_back(parms.OutDependencies[i].ToString());
        return parms.ReturnValue;
    }

    // ---- data ----

    struct Entry
    {
        bool         building = false;
        std::string  pkg, asset;
        SDK::FName   pkgName;
        bool         pkgResident = false;   // the package was loaded when the list was made
        bool         used        = false;   // taken as a timed sample
        bool         done        = false;   // brush resolved and facts taken
        bool         factLogged  = false;
        int          kind        = -1;      // ResolveBrushSEH result; 5 = package would not load
        std::string  texPath, texPkg;
        int          texResident = -1;      // image texture loaded before the entry was asked for: 1, 0, or -1 unknown
    };

    struct TexRec
    {
        bool             haveFacts = false;
        bool             inItems = false, inBuildings = false;
        Ops::Facts       f = {};
        Raw              raw = {};
        bool             rawOk = false;
    };

    struct Stats
    {
        std::vector<double> find, load, fullload, read, parkwait, fence, worst;
        int hits = 0, misses = 0, loadFail = 0, noTexture = 0, copyFail = 0, faults = 0, parked = 0, fenceTimeout = 0;
    };

    struct Cur
    {
        int          stage = 0;            // 0 none, 1 waiting out a parked residency request, 2 waiting for the copy fence
        size_t       idx = 0;
        PluginTextureHandle h = nullptr;
        std::string  name;
        bool         resident = false;
        double       find = -1, load = -1, fullload = -1, resolve = -1, read = -1, parkwait = -1;
        LONGLONG     parkStart = 0;
        int          parkTicks = 0, fenceTicks = 0;
        bool         parkedSeen = false;
        double       worst = 0;
        std::string  png = "-";
    };

    struct Watch
    {
        std::string  name, path;
        LONGLONG     start = 0;
        bool         gone = false;
    };

    struct Pending { PluginTextureHandle h; int ticks; };

    struct PaceRun
    {
        int      rate = 1;
        size_t   next = 0;
        int      retries = 0, retryNow = 0;
        int      ok = 0, loaded = 0, missing = 0, fail = 0, ticks = 0;
        std::vector<double>  dt;
        std::vector<Pending> pending;
        double   baseUsage = 0, peakUsage = 0;
    };

    // Everything below is touched on the game thread only, except the two due times.
    std::atomic<LONGLONG>  g_menuDue{ 0 }, g_worldDue{ 0 };
    Ph                     g_ph = Ph::Idle;
    bool                   g_started = false;     // the build gate has been passed once
    bool                   g_listed = false;
    LONGLONG               g_gateStart = 0, g_phaseStart = 0, g_partStart = 0;
    LONGLONG               g_lastTick = 0;
    double                 g_dt = 0;
    LONGLONG               g_nextToast = 0;
    const char*            g_toastText = nullptr;
    Ph                     g_afterList = Ph::MenuSelect;

    std::vector<Entry>                       g_entries;
    std::vector<size_t>                      g_order;       // items and buildings interleaved
    std::unordered_map<std::string, TexRec>  g_tex;
    size_t                                   g_cursor = 0;
    std::vector<size_t>                      g_retry;
    int                                      g_retryPass = 0;
    LONGLONG                                 g_retryDue = 0;
    std::vector<size_t>                      g_queue;       // the timed samples still to run
    size_t                                   g_queuePos = 0;
    std::vector<size_t>                      g_resBucket, g_missBucket;
    Cur                                      g_cur;
    Stats                                    g_stats;
    const char*                              g_part = "menu";
    int                                      g_sampleNo = 0, g_seq = 0, g_depsLogged = 0;
    int                                      g_dumpItems = 0, g_dumpBuildings = 0, g_dumpTries = 0;
    std::vector<Watch>                       g_watch;
    bool                                     g_watchActive = false;
    LONGLONG                                 g_watchNext = 0;
    double                                   g_vramBefore = -1;
    PaceRun                                  g_pace;
    std::vector<std::string>                 g_paceKeys;     // distinct item textures, by path
    bool                                     g_scanned = false, g_finishPending = false;
    std::vector<double>                      g_baseDt;

    std::wstring W(const std::string& s) { return std::wstring(s.begin(), s.end()); }   // object paths are ASCII

    void Summary(const char* what, const std::vector<double>& v)
    {
        LogToFile::Info("[ImgTrial] SUMMARY part=%s what=%s n=%zu p50=%.3f p95=%.3f max=%.3f", g_part, what, v.size(),
            Pct(v, .5), Pct(v, .95), Pct(v, 1));
    }

    bool ReadMemory(Ops::MemoryInfo* m)
    {
        return UI::ImGuiBackend::ImageTrial(Ops::Memory, m, nullptr) == 1;
    }

    void LogVram(const char* when)
    {
        Ops::MemoryInfo m = {};
        if (!ReadMemory(&m)) { LogToFile::Info("[ImgTrial] VRAM when=%s unavailable", when); return; }
        LogToFile::Info("[ImgTrial] VRAM when=%s usageMB=%.1f budgetMB=%.1f availableToReserveMB=%.1f reservedMB=%.1f", when,
            double(m.usage) / kMB, double(m.budget) / kMB, double(m.availableToReserve) / kMB, double(m.reserved) / kMB);
    }

    double VramUsageMB()
    {
        Ops::MemoryInfo m = {};
        return ReadMemory(&m) ? double(m.usage) / kMB : -1;
    }

    void LogPlugins()
    {
        const PluginInfo* infos[256] = {};
        const int n = PluginManager::GetLoadedPluginInfos(infos, 256);
        std::string list;
        for (int i = 0; i < n && i < 256; ++i)
            if (infos[i]) list += std::string(infos[i]->name ? infos[i]->name : "?") + " " + (infos[i]->version ? infos[i]->version : "?") + "; ";
        LogToFile::Info("[ImgTrial] PLUGINS loaded=%d: %s", n, list.c_str());
    }

    void SetPhase(Ph p)
    {
        g_ph = p;
        g_phaseStart = Now();
    }

    void ShowToast(const char* text)
    {
        g_toastText = text;
        g_nextToast = 0;
    }

    std::string PngDir()
    {
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        wchar_t* slash = wcsrchr(path, L'\\');
        if (slash) *(slash + 1) = 0;
        std::wstring dir = std::wstring(path) + L"ModLoader\\ImgTrial\\";
        CreateDirectoryW((std::wstring(path) + L"ModLoader").c_str(), nullptr);
        CreateDirectoryW(dir.c_str(), nullptr);
        return std::string(dir.begin(), dir.end());
    }

    SDK::UTexture2D* FindTexture(const std::string& path)
    {
        SDK::UObject* o = nullptr;
        const std::wstring w = W(path);
        if (!FindObjSEH(SDK::UTexture2D::StaticClass(), nullptr, w.c_str(), &o)) return nullptr;
        return static_cast<SDK::UTexture2D*>(o);
    }

    SDK::UTexture2D* LoadTexture(const std::string& path)
    {
        if (SDK::UTexture2D* t = FindTexture(path)) return t;
        SDK::UObject* o = nullptr;
        const std::wstring w = W(path);
        if (!LoadObjSEH(SDK::UTexture2D::StaticClass(), w.c_str(), &o)) return nullptr;
        return static_cast<SDK::UTexture2D*>(o);
    }

    // ---- the list ----

    void AddAssets(SDK::TArray<SDK::FAssetData>& assets, bool building)
    {
        for (int i = 0; i < assets.Num(); ++i)
        {
            Entry e;
            e.building = building;
            e.pkgName  = assets[i].PackageName;
            e.pkg      = assets[i].PackageName.ToString();
            e.asset    = assets[i].AssetName.ToString();
            if (e.pkg.empty() || e.asset.empty()) continue;
            if (e.asset.find("WhiteSquareTexture") != std::string::npos) continue;
            g_entries.push_back(std::move(e));
        }
    }

    bool BuildList()
    {
        const LONGLONG t0 = Now();
        SDK::TArray<SDK::FAssetData> items, buildings;
        const bool okItems     = GetAssetsByClass(L"/Script/AuItems", L"AuItemBlueprint", items);
        const bool okBuildings = GetAssetsByClass(L"/Script/Chimera", L"CrBuildingData", buildings);
        g_entries.clear();
        AddAssets(items, false);
        AddAssets(buildings, true);
        size_t nItems = 0, nBuildings = 0;
        for (const Entry& e : g_entries) (e.building ? nBuildings : nItems)++;
        std::vector<size_t> a, b;
        for (size_t i = 0; i < g_entries.size(); ++i) (g_entries[i].building ? b : a).push_back(i);
        g_order.clear();
        for (size_t i = 0; i < std::max(a.size(), b.size()); ++i)
        {
            if (i < a.size()) g_order.push_back(a[i]);
            if (i < b.size()) g_order.push_back(b[i]);
        }
        LogToFile::Info("[ImgTrial] LIST items=%zu buildings=%zu registryItems=%d registryBuildings=%d registryMs=%.3f", nItems,
            nBuildings, okItems, okBuildings, Ms(t0, Now()));
        return !g_entries.empty();
    }

    // ---- one image: find the package, load it if needed, read the brush, take the facts ----

    struct Timing { double find = -1, load = -1, fullload = -1, resolve = -1, query = -1; bool resident = false, loadFailed = false, fault = false; };

    // Pointer returned is good for this tick only.
    SDK::UTexture2D* Acquire(Entry& e, Timing& t, bool snapshotDeps, bool logDeps)
    {
        ResolveEngine();
        if (!g_eng.Ok()) { e.kind = 5; e.done = true; return nullptr; }
        const std::wstring pw = W(e.pkg);

        SDK::UPackage* package = nullptr;
        LONGLONG a = Now();
        if (!FindPkgSEH(pw.c_str(), &package)) t.fault = true;
        t.find = Ms(a, Now());
        t.resident = package != nullptr;

        // What the registry says this entry depends on, and which of those are loaded now, asked before anything is loaded.
        std::unordered_map<std::string, bool> deps;
        bool loggedDeps = false;
        if (snapshotDeps && !t.resident)
        {
            std::vector<std::string> names;
            a = Now();
            const bool ok = GetHardDependencies(e.pkgName, &names);
            t.query = Ms(a, Now());
            int loaded = 0;
            for (const std::string& n : names)
            {
                SDK::UPackage* dp = nullptr;
                const std::wstring dw = W(n);
                const bool isLoaded = FindPkgSEH(dw.c_str(), &dp) && dp;
                deps[n] = isLoaded;
                loaded += isLoaded ? 1 : 0;
            }
            if (logDeps && !e.building && g_depsLogged < int(kDepsLogged))
            {
                ++g_depsLogged;
                loggedDeps = true;
                LogToFile::Info("[ImgTrial] DEPS item=%s pkg=%s query=%d queryMs=%.3f deps=%zu depsLoaded=%d blueprintLoaded=0",
                    e.asset.c_str(), e.pkg.c_str(), ok, t.query, names.size(), loaded);
            }
        }

        if (package)
        {
            a = Now();
            if (!FullyLoadSEH(package)) t.fault = true;
            t.fullload = Ms(a, Now());
        }
        else
        {
            a = Now();
            if (!LoadPkgSEH(pw.c_str(), &package)) t.fault = true;
            t.load = Ms(a, Now());
            if (!package) t.loadFailed = true;
        }
        if (!package)
        {
            e.kind = 5; e.done = true;
            return nullptr;
        }

        const std::wstring nameW = W(e.building ? e.asset : e.asset + "_C");
        SDK::UObject* res = nullptr;
        a = Now();
        e.kind = ResolveBrushSEH(e.building, package, nameW.c_str(), &res);
        t.resolve = Ms(a, Now());
        if (e.kind < 0) t.fault = true;
        e.done = true;
        if (e.kind != 0 || !res) return nullptr;

        std::string full;
        if (!PathSEH(res, &full)) { t.fault = true; e.kind = -1; return nullptr; }
        const size_t sp = full.find(' ');
        e.texPath = sp == std::string::npos ? full : full.substr(sp + 1);
        const size_t dot = e.texPath.find('.');
        e.texPkg = dot == std::string::npos ? e.texPath : e.texPath.substr(0, dot);
        if (t.resident) e.texResident = 1;
        else
        {
            auto it = deps.find(e.texPkg);
            e.texResident = it == deps.end() ? -1 : (it->second ? 1 : 0);
        }
        if (loggedDeps)
            LogToFile::Info("[ImgTrial] DEPS item=%s texPkg=%s inDeps=%d texLoadedBefore=%d", e.asset.c_str(), e.texPkg.c_str(),
                deps.count(e.texPkg) ? 1 : 0, e.texResident);
        TexRec& rec = g_tex[e.texPath];
        (e.building ? rec.inBuildings : rec.inItems) = true;
        return static_cast<SDK::UTexture2D*>(res);
    }

    const char* KindName(int k)
    {
        switch (k)
        {
        case 0: return "texture";
        case 1: return "other";
        case 2: return "empty";
        case 3: return "not-found";
        case 4: return "wrong-type";
        case 5: return "no-package";
        default: return "fault";
        }
    }

    // F1. True once the facts for this entry's texture are in the log.
    bool GatherFacts(Entry& e, SDK::UTexture2D* tex)
    {
        if (e.factLogged) return true;
        TexRec& rec = g_tex[e.texPath];
        if (!rec.haveFacts)
        {
            rec.rawOk = ReadRawSEH(tex, &rec.raw);
            const intptr_t r = UI::ImGuiBackend::ImageTrial(Ops::TextureFacts, tex, &rec.f);
            if (r != 1) return false;
            rec.haveFacts = true;
        }
        const Ops::Facts& f = rec.f;
        const Raw& r = rec.raw;
        LogToFile::Info("[ImgTrial] FACT set=%s name=%s tex=%s pkgResident=%d texResident=%d fmt=%u res=%ux%u resMips=%u assetMips=%d "
            "firstResMip=%d imported=%dx%d neverStream=%d lodGroup=%d vts=%d compression=%d heap=%s protect=%s state=0x%x flags=0x%x "
            "dim=%u array=%u samples=%u alloc=%llu",
            e.building ? "building" : "item", e.asset.c_str(), e.texPath.c_str(), e.pkgResident ? 1 : 0, e.texResident, f.fmt, f.w, f.h,
            f.mips, rec.rawOk ? int(f.mips) + r.firstMip : -1, rec.rawOk ? r.firstMip : -1, rec.rawOk ? r.impX : -1, rec.rawOk ? r.impY : -1,
            rec.rawOk ? r.neverStream : -1, rec.rawOk ? r.lod : -1, rec.rawOk ? r.vts : -1, rec.rawOk ? r.comp : -1,
            f.heap == 0 ? "committed" : f.heap == 1 ? "placed" : "unreadable", f.reduced ? "reduced" : "full", f.state, f.flags,
            f.dimension, f.depthOrArray, f.samples, f.allocBytes);
        e.factLogged = true;
        return true;
    }

    // ---- timed samples (F4, F7, F8) ----

    void PickSamples()
    {
        std::vector<size_t> resident, miss;
        resident.assign(g_resBucket.begin(), g_resBucket.begin() + std::min(g_resBucket.size(), kSampleMax));
        miss.assign(g_missBucket.begin(), g_missBucket.begin() + std::min(g_missBucket.size(), kSampleMax));
        size_t takeRes = std::min(resident.size(), kSampleEach), takeMiss = std::min(miss.size(), kSampleEach);
        while (takeRes + takeMiss < kSampleMax && (takeRes < resident.size() || takeMiss < miss.size()))
            (takeRes < resident.size() ? takeRes : takeMiss)++;
        g_queue.clear();
        for (size_t i = 0; i < std::max(takeRes, takeMiss); ++i)
        {
            if (i < takeMiss) g_queue.push_back(miss[i]);
            if (i < takeRes)  g_queue.push_back(resident[i]);
        }
        g_queuePos = 0;
        g_stats = Stats();
        g_sampleNo = 0;
        LogToFile::Info("[ImgTrial] SELECT part=%s resident=%zu notResident=%zu candidatesResident=%zu candidatesNotResident=%zu", g_part,
            takeRes, takeMiss, g_resBucket.size(), g_missBucket.size());
    }

    void FinishPart()
    {
        LogToFile::Info("[ImgTrial] SUMMARY part=%s hits=%d misses=%d loadFail=%d noTexture=%d copyFail=%d faults=%d parked=%d fenceTimeout=%d", g_part,
            g_stats.hits, g_stats.misses, g_stats.loadFail, g_stats.noTexture, g_stats.copyFail, g_stats.faults, g_stats.parked, g_stats.fenceTimeout);
        Summary("find", g_stats.find);
        Summary("load", g_stats.load);
        Summary("fullyload", g_stats.fullload);
        Summary("read", g_stats.read);
        Summary("parkwait", g_stats.parkwait);
        Summary("fenceTicks", g_stats.fence);
        Summary("worstTick", g_stats.worst);
    }

    std::string SafeName(const std::string& s)
    {
        std::string o;
        for (char c : s) o += (isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') ? c : '_';
        return o;
    }

    void StartWatch(Entry& e)
    {
        if (g_watch.size() >= kWatchCount || e.texPath.empty()) return;
        Watch w;
        w.name = e.asset; w.path = e.texPath; w.start = Now();
        g_watch.push_back(std::move(w));
        g_watchActive = true;
        g_watchNext = InMs(1000);
    }

    void EndSample(Entry& e, const char* copy)
    {
        Cur& c = g_cur;
        LogToFile::Info("[ImgTrial] TIME part=%s n=%d set=%s name=%s resident=%d find=%.3f load=%.3f fullyload=%.3f resolve=%.3f read=%.3f "
            "parkwait=%.3f parked=%d fenceTicks=%d worstTick=%.3f kind=%s copy=%s png=%s tex=%s", g_part, g_sampleNo, e.building ? "building" : "item",
            e.asset.c_str(), c.resident ? 1 : 0, c.find, c.load, c.fullload, c.resolve, c.read, c.parkwait, c.parkedSeen ? 1 : 0, c.fenceTicks, c.worst,
            KindName(e.kind), copy, c.png.c_str(), e.texPath.empty() ? "-" : e.texPath.c_str());
        if (c.find >= 0) g_stats.find.push_back(c.find);
        if (c.load >= 0) g_stats.load.push_back(c.load);
        if (c.fullload >= 0) g_stats.fullload.push_back(c.fullload);
        if (c.read >= 0) g_stats.read.push_back(c.read);
        if (c.parkwait >= 0) g_stats.parkwait.push_back(c.parkwait);
        if (strcmp(copy, "ok") == 0) g_stats.fence.push_back(double(c.fenceTicks));
        g_stats.worst.push_back(c.worst);
        c = Cur();
    }

    // The copy is done on the GPU: dump a PNG if one is still wanted, free it, close the sample.
    void CopyFinished(Entry& e)
    {
        Cur& c = g_cur;
        const bool wantPng = e.building ? g_dumpBuildings < kDumpBuildings : g_dumpItems < kDumpItems;
        if (wantPng && g_dumpTries < 40)
        {
            ++g_dumpTries;
            const std::string file = PngDir() + (e.building ? "building_" : "item_") + std::to_string(e.building ? g_dumpBuildings : g_dumpItems) +
                "_" + SafeName(e.asset) + ".png";
            const std::wstring fw = W(file);
            const intptr_t r = UI::ImGuiBackend::ImageTrial(Ops::DumpPng, c.h, const_cast<wchar_t*>(fw.c_str()));
            const TexRec& rec = g_tex[e.texPath];
            std::string written = file;
            if (r == 2) written.replace(written.size() - 4, 4, ".dds");
            LogToFile::Info("[ImgTrial] DUMP set=%s name=%s result=%s fmt=%u res=%ux%u file=%s", e.building ? "building" : "item",
                e.asset.c_str(), r == 1 ? "png" : r == 2 ? "dds-raw" : r == 0 ? "not-ready" : "failed", rec.f.fmt, rec.f.w, rec.f.h, written.c_str());
            if (r == 1 || r == 2) { (e.building ? g_dumpBuildings : g_dumpItems)++; c.png = written; }
        }
        if (IPluginImGuiTextures* api = UI::ImGuiBackend::GetTextureAPI()) api->FreeTexture(c.h);
        c.h = nullptr;
        EndSample(e, "ok");
    }

    bool TryRead(Entry& e, SDK::UTexture2D* tex)
    {
        Cur& c = g_cur;
        IPluginImGuiTextures* api = UI::ImGuiBackend::GetTextureAPI();
        if (!api || !api->LoadFromUTexture2D || !api->FreeTexture) return false;
        c.name = "ImgTrial#" + std::to_string(++g_seq);
        const LONGLONG a = Now();
        c.h = CopySEH(api, tex, c.name.c_str());
        c.read = Ms(a, Now());
        (void)e;
        return c.h != nullptr;
    }

    void BeginSample()
    {
        Cur& c = g_cur;
        c = Cur();
        c.idx = g_queue[g_queuePos++];
        ++g_sampleNo;
        Entry& e = g_entries[c.idx];
        e.used = true;
        Timing t;
        SDK::UTexture2D* tex = Acquire(e, t, true, true);
        c.resident = t.resident;
        c.find = t.find; c.load = t.load; c.fullload = t.fullload; c.resolve = t.resolve;
        c.worst = g_dt;
        t.resident ? ++g_stats.hits : ++g_stats.misses;
        if (t.fault) ++g_stats.faults;
        if (t.loadFailed) { ++g_stats.loadFail; EndSample(e, "no-package"); return; }
        if (!tex) { ++g_stats.noTexture; EndSample(e, "no-texture"); return; }
        GatherFacts(e, tex);
        if (!t.resident && e.kind == 0 && g_part[0] == 'm') StartWatch(e);

        const intptr_t parkedBefore = UI::ImGuiBackend::ImageTrial(Ops::Parked, nullptr, nullptr);
        if (TryRead(e, tex))
        {
            c.stage = 2;
            if (UI::ImGuiBackend::ImageTrial(Ops::CopyState, c.h, nullptr) == 1) CopyFinished(e);
            return;
        }
        // Refused: a residency request is parked, the copy ring is full, or the texture is not ready yet. Try again each tick.
        const intptr_t parkedAfter = UI::ImGuiBackend::ImageTrial(Ops::Parked, nullptr, nullptr);
        c.parkedSeen = parkedAfter > 0 || parkedBefore > 0;
        if (c.parkedSeen) ++g_stats.parked;
        c.stage = 1;
        c.parkStart = Now();
    }

    void SampleStep()
    {
        Cur& c = g_cur;
        if (c.stage == 0)
        {
            if (g_queuePos >= g_queue.size()) return;
            BeginSample();
            return;
        }
        Entry& e = g_entries[c.idx];
        c.worst = std::max(c.worst, g_dt);
        if (c.stage == 1)
        {
            SDK::UTexture2D* tex = FindTexture(e.texPath);
            if (!tex) { ++g_stats.copyFail; EndSample(e, "texture-gone"); return; }
            if (TryRead(e, tex))
            {
                c.parkwait = Ms(c.parkStart, Now());
                c.stage = 2;
                if (UI::ImGuiBackend::ImageTrial(Ops::CopyState, c.h, nullptr) == 1) CopyFinished(e);
                return;
            }
            if (!c.parkedSeen && UI::ImGuiBackend::ImageTrial(Ops::Parked, nullptr, nullptr) > 0) { c.parkedSeen = true; ++g_stats.parked; }
            if (++c.parkTicks > (c.parkedSeen ? kParkTicksMax : kRefusedTicksMax))
            {
                ++g_stats.copyFail;
                c.parkwait = Ms(c.parkStart, Now());
                EndSample(e, c.parkedSeen ? "park-timeout" : "read-refused");
            }
            return;
        }
        const intptr_t s = UI::ImGuiBackend::ImageTrial(Ops::CopyState, c.h, nullptr);
        if (s == 1) { ++c.fenceTicks; CopyFinished(e); return; }
        if (s < 0) { ++g_stats.copyFail; EndSample(e, "bad-handle"); return; }
        if (++c.fenceTicks > kFenceTicksMax) { ++g_stats.fenceTimeout; c.h = nullptr; EndSample(e, "fence-timeout"); }
    }

    // ---- F5 ----

    void WatchTick()
    {
        if (!g_watchActive || Now() < g_watchNext) return;
        g_watchNext = InMs(1000);
        int held = 0;
        for (Watch& w : g_watch)
        {
            if (w.gone) continue;
            const double s = Ms(w.start, Now()) / 1000.0;
            if (FindTexture(w.path))
            {
                if (s >= kWatchSeconds)
                {
                    w.gone = true;
                    LogToFile::Info("[ImgTrial] RELEASE name=%s afterS=%.1f gone=0 held-to-the-end tex=%s", w.name.c_str(), s, w.path.c_str());
                }
                else ++held;
            }
            else
            {
                w.gone = true;
                LogToFile::Info("[ImgTrial] RELEASE name=%s afterS=%.1f gone=1 tex=%s", w.name.c_str(), s, w.path.c_str());
            }
        }
        LogToFile::Info("[ImgTrial] RELEASE-TICK held=%d vramMB=%.1f", held, VramUsageMB());
        if (held == 0)
        {
            g_watchActive = false;
            LogToFile::Info("[ImgTrial] RELEASE-DONE watched=%zu", g_watch.size());
        }
    }

    // ---- counts (F2, F3) and the summary of facts (F1) ----

    void LogCounts()
    {
        size_t items = 0, itemTex = 0, buildings = 0, buildingTex = 0, other[2] = {}, empty[2] = {}, unresolved = 0;
        size_t pkgRes[2] = {}, texRes[2] = {}, texKnown[2] = {};
        std::unordered_set<std::string> itemSet, buildSet;
        for (const Entry& e : g_entries)
        {
            const int s = e.building ? 1 : 0;
            (e.building ? buildings : items)++;
            if (e.pkgResident) ++pkgRes[s];
            if (e.kind == 0) { (e.building ? buildingTex : itemTex)++; (e.building ? buildSet : itemSet).insert(e.texPath); }
            else if (e.kind == 1) ++other[s];
            else if (e.kind == 2) ++empty[s];
            else ++unresolved;
            if (e.kind == 0 && e.texResident >= 0) { ++texKnown[s]; texRes[s] += e.texResident; }
        }
        size_t both = 0;
        for (const std::string& p : itemSet) if (buildSet.count(p)) ++both;
        LogToFile::Info("[ImgTrial] COUNT itemBlueprints=%zu itemsWithTexture=%zu itemTextures=%zu buildingAssets=%zu buildingsWithTexture=%zu "
            "buildingTextures=%zu texturesInBoth=%zu itemsMaterialOrOther=%zu itemsEmpty=%zu buildingsMaterialOrOther=%zu buildingsEmpty=%zu "
            "unresolved=%zu", items, itemTex, itemSet.size(), buildings, buildingTex, buildSet.size(), both, other[0], empty[0], other[1],
            empty[1], unresolved);
        LogToFile::Info("[ImgTrial] RESIDENT when=menu itemPackages=%zu/%zu buildingPackages=%zu/%zu itemTexturesLoadedBeforeAsked=%zu/%zu "
            "buildingTexturesLoadedBeforeAsked=%zu/%zu", pkgRes[0], items, pkgRes[1], buildings, texRes[0], texKnown[0], texRes[1], texKnown[1]);

        struct Key { unsigned fmt, w, h, mips; bool operator<(const Key& o) const
            { return std::tie(fmt, w, h, mips) < std::tie(o.fmt, o.w, o.h, o.mips); } };
        std::map<Key, std::pair<int, int>> groups;
        int missing = 0;
        for (const auto& kv : g_tex)
        {
            if (!kv.second.haveFacts) { ++missing; continue; }
            const Key k{ kv.second.f.fmt, kv.second.f.w, kv.second.f.h, kv.second.f.mips };
            (kv.second.inBuildings && !kv.second.inItems ? groups[k].second : groups[k].first)++;
        }
        for (const auto& g : groups)
            LogToFile::Info("[ImgTrial] FACT-SUMMARY fmt=%u res=%ux%u resMips=%u textures=%d (buildingOnly=%d)", g.first.fmt, g.first.w, g.first.h,
                g.first.mips, g.second.first + g.second.second, g.second.second);
        LogToFile::Info("[ImgTrial] FACT-SUMMARY groups=%zu texturesWithoutFacts=%d", groups.size(), missing);
    }

    // ---- the scan, one budgeted slice ----

    void ScanStep()
    {
        const LONGLONG start = Now();
        while (g_cursor < g_entries.size())
        {
            Entry& e = g_entries[g_cursor++];
            if (e.done) continue;
            Timing t;
            SDK::UTexture2D* tex = Acquire(e, t, true, false);
            if (tex && !GatherFacts(e, tex)) g_retry.push_back(g_cursor - 1);
            if (Ms(start, Now()) >= kScanBudgetMs) break;
        }
    }

    // Entries that gave no facts: the texture may not have its GPU resource yet. A few spaced passes.
    void RetryStep()
    {
        if (Now() < g_retryDue) return;
        std::vector<size_t> left;
        for (size_t i : g_retry)
        {
            Entry& e = g_entries[i];
            SDK::UTexture2D* tex = FindTexture(e.texPath);
            if (!tex || !GatherFacts(e, tex)) left.push_back(i);
        }
        g_retry.swap(left);
        ++g_retryPass;
        LogToFile::Info("[ImgTrial] RETRY pass=%d stillWithoutFacts=%zu", g_retryPass, g_retry.size());
    }

    // ---- world: F6 ----

    void PaceBegin(int rate)
    {
        g_pace = PaceRun();
        g_pace.rate = rate;
        g_pace.baseUsage = VramUsageMB();
        g_pace.peakUsage = g_pace.baseUsage;
    }

    void PaceFinish()
    {
        Ops::MemoryInfo m = {};
        ReadMemory(&m);
        const double budget = double(m.budget) / kMB;
        const double delta = g_pace.peakUsage - g_pace.baseUsage;
        LogToFile::Info("[ImgTrial] PACE rate=%d images=%zu read=%d loadedForTheRead=%d missing=%d retries=%d fail=%d ticks=%d "
            "tickP95Before=%.3f tickP95During=%.3f tickMaxDuring=%.3f vramBaseMB=%.1f vramPeakMB=%.1f deltaMB=%.1f budgetMB=%.1f deltaPctOfBudget=%.2f",
            g_pace.rate, g_paceKeys.size(), g_pace.ok, g_pace.loaded, g_pace.missing, g_pace.retries, g_pace.fail, g_pace.ticks,
            Pct(g_baseDt, .95), Pct(g_pace.dt, .95), Pct(g_pace.dt, 1), g_pace.baseUsage, g_pace.peakUsage, delta, budget,
            budget > 0 ? 100.0 * delta / budget : 0.0);
    }

    void PollPending()
    {
        IPluginImGuiTextures* api = UI::ImGuiBackend::GetTextureAPI();
        std::vector<Pending> keep;
        for (Pending& p : g_pace.pending)
        {
            const intptr_t s = UI::ImGuiBackend::ImageTrial(Ops::CopyState, p.h, nullptr);
            if (s == 1 || s < 0) { if (s == 1 && api) api->FreeTexture(p.h); }
            else if (++p.ticks > kFenceTicksMax) { ++g_pace.fail; }   // left alone: the GPU may still be using it
            else keep.push_back(p);
        }
        g_pace.pending.swap(keep);
    }

    void PaceStep()
    {
        ++g_pace.ticks;
        g_pace.dt.push_back(g_dt);
        PollPending();
        const double usage = VramUsageMB();
        if (usage > g_pace.peakUsage) g_pace.peakUsage = usage;
        IPluginImGuiTextures* api = UI::ImGuiBackend::GetTextureAPI();
        if (!api || !api->LoadFromUTexture2D || !api->FreeTexture) { g_pace.next = g_paceKeys.size(); return; }
        for (int n = 0; n < g_pace.rate && g_pace.next < g_paceKeys.size(); ++n)
        {
            const std::string& path = g_paceKeys[g_pace.next];
            const bool wasThere = FindTexture(path) != nullptr;
            SDK::UTexture2D* tex = LoadTexture(path);
            if (!tex) { ++g_pace.missing; ++g_pace.next; g_pace.retryNow = 0; continue; }
            if (!wasThere && g_pace.retryNow == 0) ++g_pace.loaded;
            const std::string name = "ImgTrial#" + std::to_string(++g_seq);
            PluginTextureHandle h = CopySEH(api, tex, name.c_str());
            if (h)
            {
                g_pace.pending.push_back({ h, 0 });
                ++g_pace.ok; ++g_pace.next; g_pace.retryNow = 0;
            }
            else
            {
                ++g_pace.retries;
                if (++g_pace.retryNow > kPaceRetryMax) { ++g_pace.fail; ++g_pace.next; g_pace.retryNow = 0; }
                break;   // the host is full or parked: next tick
            }
        }
    }

    // ---- driver ----

    bool GateOpen(bool world)
    {
        const intptr_t r = UI::ImGuiBackend::ImageTrial(Ops::HeapRows, nullptr, nullptr);
        if (r == 1) return true;
        if (r == 0 || r == -2)
        {
            LogToFile::Info("[ImgTrial] SKIP %s; nothing is measured", r == 0 ? "this game build has no heap rows in the version map" :
                "the ImGui host has no image trial export");
            SetPhase(Ph::Inert);
            return false;
        }
        if (Ms(g_gateStart, Now()) > kHeapWaitMs)
        {
            LogToFile::Info("[ImgTrial] SKIP the GPU device did not come up in time; nothing is measured");
            SetPhase(Ph::Inert);
        }
        (void)world;
        return false;
    }

    void Header(const char* part)
    {
        Ops::MemoryInfo m = {};
        if (ReadMemory(&m))
            LogToFile::Info("[ImgTrial] ADAPTER name=\"%s\" dedicatedMB=%.1f localBudgetMB=%.1f", m.adapter, double(m.dedicatedBytes) / kMB,
                double(m.budget) / kMB);
        else
            LogToFile::Info("[ImgTrial] ADAPTER unavailable");
        LogVram(part);
        if (!g_started) LogPlugins();
        g_started = true;
        g_partStart = Now();
    }

    void StartSelect()
    {
        g_resBucket.clear(); g_missBucket.clear();
        g_cursor = 0;
        SetPhase(g_part[0] == 'm' ? Ph::MenuSelect : Ph::WorldSelect);
    }

    // A slice of FindPackage calls to sort the list into loaded and not loaded.
    bool SelectStep()
    {
        ResolveEngine();
        int n = 0;
        while (g_cursor < g_order.size() && n < kResidentPerTick)
        {
            Entry& e = g_entries[g_order[g_cursor++]];
            ++n;
            if (e.used) continue;
            SDK::UPackage* p = nullptr;
            const std::wstring w = W(e.pkg);
            FindPkgSEH(w.c_str(), &p);
            (p ? g_resBucket : g_missBucket).push_back(&e - g_entries.data());
        }
        return g_cursor >= g_order.size() || (g_resBucket.size() >= kSampleMax && g_missBucket.size() >= kSampleMax);
    }

    void ListResidency()
    {
        int n = 0;
        while (g_cursor < g_entries.size() && n < kResidentPerTick)
        {
            Entry& e = g_entries[g_cursor++];
            SDK::UPackage* p = nullptr;
            const std::wstring w = W(e.pkg);
            FindPkgSEH(w.c_str(), &p);
            e.pkgResident = p != nullptr;
            ++n;
        }
    }

    void BeginWorldResident()
    {
        g_cursor = 0;
        SetPhase(Ph::WorldRes);
    }

    // F3 in the world: what is loaded now, by the paths the scan found.
    size_t g_wRes[2] = {}, g_wResTotal[2] = {}, g_wTex[2] = {}, g_wTexTotal[2] = {};

    bool WorldResidentStep()
    {
        int n = 0;
        while (g_cursor < g_entries.size() && n < kResidentPerTick)
        {
            const Entry& e = g_entries[g_cursor++];
            const int s = e.building ? 1 : 0;
            SDK::UPackage* p = nullptr;
            const std::wstring w = W(e.pkg);
            FindPkgSEH(w.c_str(), &p);
            ++g_wResTotal[s]; g_wRes[s] += p ? 1 : 0;
            if (e.kind == 0 && !e.texPath.empty()) { ++g_wTexTotal[s]; g_wTex[s] += FindTexture(e.texPath) ? 1 : 0; }
            ++n;
        }
        return g_cursor >= g_entries.size();
    }

    void BuildPaceList()
    {
        g_paceKeys.clear();
        std::unordered_set<std::string> seen;
        for (const Entry& e : g_entries)
            if (!e.building && e.kind == 0 && !e.texPath.empty() && seen.insert(e.texPath).second)
                g_paceKeys.push_back(e.texPath);
    }

    void StartWorld()
    {
        g_part = "world";
        g_dt = 0;
        SetPhase(Ph::WorldGate);
        g_gateStart = Now();
    }

    // The scan and its retries are over: the menu part is done, or the world part goes on to the paced pass.
    void AfterScan()
    {
        g_scanned = true;
        LogVram(g_part[0] == 'm' ? "menu-after-scan" : "world-after-scan");
        LogCounts();
        if (g_part[0] == 'm')
        {
            LogToFile::Info("[ImgTrial] DONE part=menu elapsedS=%.1f", Ms(g_partStart, Now()) / 1000.0);
            ShowToast("Image trial: menu part done");
            SetPhase(Ph::MenuDone);
            return;
        }
        BuildPaceList();
        g_baseDt.clear();
        SetPhase(Ph::PaceBase);
    }

    void Drive(bool inWorld)
    {
        switch (g_ph)
        {
        case Ph::Idle:
            break;
        case Ph::MenuWait:
        {
            const LONGLONG due = g_menuDue.load();
            if (inWorld || !due || Now() < due) break;
            g_menuDue.store(0);
            g_gateStart = Now();
            g_part = "menu";
            SetPhase(Ph::Gate);
            break;
        }
        case Ph::Gate:
            if (!GateOpen(false)) break;
            LogToFile::Info("[ImgTrial] START part=menu");
            Header("menu-start");
            g_afterList = Ph::MenuSelect;
            SetPhase(Ph::List);
            break;
        case Ph::List:
            if (!BuildList()) { LogToFile::Info("[ImgTrial] SKIP the registry gave no item or building assets"); SetPhase(Ph::Inert); break; }
            g_listed = true;
            g_cursor = 0;
            SetPhase(Ph::ListRes);
            break;
        case Ph::ListRes:
            ListResidency();
            if (g_cursor >= g_entries.size()) StartSelect();
            break;
        case Ph::MenuSelect:
        case Ph::WorldSelect:
            if (SelectStep()) { PickSamples(); SetPhase(g_ph == Ph::MenuSelect ? Ph::MenuSample : Ph::WorldSample); }
            break;
        case Ph::MenuSample:
            SampleStep();
            if (g_cur.stage == 0 && g_queuePos >= g_queue.size())
            {
                FinishPart();
                g_cursor = 0;
                SetPhase(Ph::Scan);
            }
            break;
        case Ph::Scan:
            ScanStep();
            if (g_cursor >= g_entries.size())
            {
                g_retryPass = 0;
                g_retryDue = InMs(kRetryGapMs);
                if (g_retry.empty()) AfterScan();
                else SetPhase(Ph::Retry);
            }
            break;
        case Ph::Retry:
            if (Now() < g_retryDue) break;
            RetryStep();
            g_retryDue = InMs(kRetryGapMs);
            if (g_retry.empty() || g_retryPass >= kRetryPasses)
            {
                for (size_t i : g_retry)
                    LogToFile::Info("[ImgTrial] FACT-MISSING name=%s tex=%s", g_entries[i].asset.c_str(), g_entries[i].texPath.c_str());
                AfterScan();
            }
            break;
        case Ph::MenuDone:
        case Ph::MenuAborted:
        case Ph::WorldDone:
        case Ph::Inert:
            break;
        case Ph::WorldWait:
            break;
        case Ph::WorldGate:
            if (!inWorld) break;
            if (!GateOpen(true)) break;
            LogToFile::Info("[ImgTrial] START part=world");
            Header("world-start");
            if (!g_listed)
            {
                g_afterList = Ph::WorldSelect;
                SetPhase(Ph::List);
                break;
            }
            BeginWorldResident();
            break;
        case Ph::WorldRes:
            if (WorldResidentStep())
            {
                LogToFile::Info("[ImgTrial] RESIDENT when=world itemPackages=%zu/%zu buildingPackages=%zu/%zu itemTextures=%zu/%zu buildingTextures=%zu/%zu",
                    g_wRes[0], g_wResTotal[0], g_wRes[1], g_wResTotal[1], g_wTex[0], g_wTexTotal[0], g_wTex[1], g_wTexTotal[1]);
                StartSelect();
            }
            break;
        case Ph::WorldSample:
            SampleStep();
            if (g_cur.stage == 0 && g_queuePos >= g_queue.size())
            {
                FinishPart();
                LogVram("world-after-sample");
                if (g_scanned)
                {
                    BuildPaceList();
                    g_baseDt.clear();
                    SetPhase(Ph::PaceBase);
                }
                else
                {
                    g_cursor = 0;
                    SetPhase(Ph::Scan);
                }
            }
            break;
        case Ph::PaceBase:
            g_baseDt.push_back(g_dt);
            if (int(g_baseDt.size()) >= kBaselineTicks)
            {
                if (g_paceKeys.empty()) { LogToFile::Info("[ImgTrial] PACE nothing to read: no item texture is known"); g_finishPending = true; SetPhase(Ph::WorldDone); break; }
                PaceBegin(1);
                SetPhase(Ph::Pace);
            }
            break;
        case Ph::Pace:
            PaceStep();
            if (g_pace.next >= g_paceKeys.size()) SetPhase(Ph::PaceDrain);
            break;
        case Ph::PaceDrain:
            g_pace.dt.push_back(g_dt);
            ++g_pace.ticks;
            PollPending();
            {
                const double usage = VramUsageMB();
                if (usage > g_pace.peakUsage) g_pace.peakUsage = usage;
            }
            if (g_pace.pending.empty())
            {
                PaceFinish();
                if (g_pace.rate == 1) SetPhase(Ph::PaceSettle);
                else { LogVram("world-after-pace"); g_finishPending = true; SetPhase(Ph::WorldDone); }
            }
            break;
        case Ph::PaceSettle:
            if (Ms(g_phaseStart, Now()) < 3000.0) break;
            PaceBegin(4);
            SetPhase(Ph::Pace);
            break;
        }
    }
}

namespace ImageTrial
{
    void OnMenu()
    {
        if (g_ph != Ph::Idle) return;
        g_menuDue.store(InMs(kMenuDelayMs));
        g_part = "menu";
        SetPhase(Ph::MenuWait);
    }

    void OnGameWorld()
    {
        g_menuDue.store(0);
    }

    void OnWorldLoaded()
    {
        if (ReloadPolicy::GameplayWorldRunning()) g_worldDue.store(InMs(kWorldDelayMs));
    }

    void Tick()
    {
        const LONGLONG now = Now();
        g_dt = g_lastTick ? Ms(g_lastTick, now) : 0;
        g_lastTick = now;

        WatchTick();
        if (g_toastText && now >= g_nextToast)
        {
            UI::Toast::Show("ImageTrial", g_toastText);
            g_nextToast = InMs(kToastEveryMs);
        }

        const bool inWorld = ReloadPolicy::GameplayWorldRunning();

        // A world began while the menu part was still waiting or running: the menu part is dropped.
        const bool menuRunning = g_part[0] == 'm' && g_ph >= Ph::MenuWait && g_ph <= Ph::Retry;
        if (inWorld && menuRunning)
        {
            LogToFile::Info("[ImgTrial] MENU-ABORTED phase=%s", PhaseName(g_ph));
            if (g_cur.stage == 2 || g_cur.stage == 1) { g_cur = Cur(); }
            SetPhase(Ph::MenuAborted);
        }

        // The world part: due once a world is running and the menu part is over (or never started).
        if (inWorld && (g_ph == Ph::MenuDone || g_ph == Ph::MenuAborted || g_ph == Ph::Idle))
        {
            const LONGLONG due = g_worldDue.load();
            if (due && now >= due)
            {
                g_worldDue.store(0);
                g_toastText = nullptr;
                StartWorld();
            }
        }
        if (!inWorld && g_part[0] == 'w' && g_ph != Ph::WorldDone && g_ph != Ph::Inert)
        {
            LogToFile::Info("[ImgTrial] WORLD-ABORTED phase=%s", PhaseName(g_ph));
            g_cur = Cur();
            SetPhase(Ph::WorldDone);
        }

        Drive(inWorld);

        // "finished" waits for the release watch, so the log is complete when the owner reads it.
        if (g_finishPending && !g_watchActive)
        {
            g_finishPending = false;
            LogToFile::Info("[ImgTrial] DONE part=world elapsedS=%.1f", Ms(g_partStart, Now()) / 1000.0);
            ShowToast("Image trial finished");
        }
    }
}

#endif
