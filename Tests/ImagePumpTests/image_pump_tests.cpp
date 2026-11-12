// Image pump test: the rules of UI/image_pump.cpp on a fake engine and GPU. The
// start gate, the budget per tick, what each read and load result does, the
// publish of a finished copy, eviction, device loss, shutdown, the summary
// line, how an item request waits for the item index and becomes the image of
// its icon, and the background preload of the sets (the lists, the setting, and
// each rule that pauses it).
//
// A console program: no game, no GPU. The pump, the library and the item index
// are the loader's own sources; a texture here is a number standing in for a
// texture slot.
#include "UI/image_pump.h"
#include "UI/image_shrink.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

static int g_checks = 0;
static int g_failed = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_failed;                                                          \
            printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
        }                                                                        \
    } while (0)

static void RunTest(const char* name, void (*fn)())
{
    const int before = g_failed;
    printf("[ RUN  ] %s\n", name);
    fn();
    printf("[ %s ] %s\n", g_failed == before ? " OK " : "FAIL", name);
}

using namespace ImagePump;
using ImageLibrary::Library;

// ---------------------------------------------------------------------------

class FakeHost : public Host
{
public:
    bool     ready  = false;
    uint64_t budget = 0;
    uint64_t clockUs = 0;
    uint64_t readCostUs = 0;
    bool     preload = false;
    bool     videoKnown = false;
    uint64_t videoUsage = 0, videoBudget = 0;
    int      videoChecks = 0;
    int      sourceSize = 0;   // level 0 of the game image; 0 = same as the copy

    // Paths in memory, and what a read of each does (Started when not set).
    std::set<std::string>              loaded;
    std::map<std::string, ReadResult>  readResult;
    std::map<std::string, LoadResult>  loadResult;
    ReadResult                         readAfterLoad = ReadResult::Started;
    int                                width = 256, height = 256;
    CopyState                          copyState = CopyState::Done;
    std::map<PluginTextureHandle, CopyState> copyStateOf;
    ShrinkResult                       shrinkResult = ShrinkResult::Started;
    std::vector<PluginTextureHandle>   shrunkFrom;
    std::vector<PluginTextureHandle>   made;          // what each started shrink made, in order
    std::vector<PluginTextureHandle>   copiesFreed;   // temporary copies let go after a shrink; not in `freed`
    int                                shrinkCalls = 0;

    std::vector<std::string>           reads, loads, logs;
    std::vector<PluginTextureHandle>   freed;
    uint32_t                           nextTexture = 1;

    bool GpuReady() override { return ready; }
    uint64_t LocalVideoBudget() override { return budget; }
    uint64_t NowUs() override { return clockUs; }
    bool PreloadEnabled() override { return preload; }
    bool VideoMemory(uint64_t& usage, uint64_t& total) override
    {
        ++videoChecks;
        usage = videoUsage;
        total = videoBudget;
        return videoKnown;
    }

    ReadResult Read(const std::string& path, Copy& out) override
    {
        reads.push_back(path);
        clockUs += readCostUs;
        if (!loaded.count(path))
            return ReadResult::NotLoaded;
        const auto it = readResult.find(path);
        const ReadResult result = it == readResult.end() ? ReadResult::Started : it->second;
        if (result == ReadResult::Started)
        {
            out.texture = reinterpret_cast<PluginTextureHandle>(static_cast<uintptr_t>(nextTexture++) * 16);
            out.width   = width;
            out.height  = height;
            out.sourceWidth  = sourceSize;
            out.sourceHeight = sourceSize;
        }
        return result;
    }

    LoadResult Load(const std::string& path) override
    {
        loads.push_back(path);
        const auto it = loadResult.find(path);
        const LoadResult result = it == loadResult.end() ? LoadResult::Loaded : it->second;
        if (result == LoadResult::Loaded)
        {
            loaded.insert(path);
            readResult[path] = readAfterLoad;
        }
        return result;
    }

    CopyState StateOf(PluginTextureHandle texture) override
    {
        const auto it = copyStateOf.find(texture);
        return it == copyStateOf.end() ? copyState : it->second;
    }

    // The host's shrink: the stored size and the full chain of levels, in a new texture.
    ShrinkResult Shrink(PluginTextureHandle source, Shrunk& out) override
    {
        ++shrinkCalls;
        if (shrinkResult != ShrinkResult::Started)
            return shrinkResult;
        shrunkFrom.push_back(source);
        const ImageShrink::Size size = ImageShrink::StoredSize(static_cast<uint32_t>(width), static_cast<uint32_t>(height), ImageLibrary::kStoredSize);
        out.texture = reinterpret_cast<PluginTextureHandle>(static_cast<uintptr_t>(nextTexture++) * 16);
        out.width   = static_cast<int>(size.width);
        out.height  = static_cast<int>(size.height);
        out.levels  = ImageShrink::LevelCount(size.width, size.height);
        made.push_back(out.texture);
        return ShrinkResult::Started;
    }

    void Free(PluginTextureHandle texture) override
    {
        const bool isCopy = std::find(shrunkFrom.begin(), shrunkFrom.end(), texture) != shrunkFrom.end();
        (isCopy ? copiesFreed : freed).push_back(texture);
    }
    void Log(const std::string& line) override { logs.push_back(line); }

    size_t Freed(PluginTextureHandle texture) const
    {
        size_t n = 0;
        for (PluginTextureHandle h : freed)
            n += h == texture;
        return n;
    }
    size_t SummaryLines() const
    {
        size_t n = 0;
        for (const std::string& line : logs)
            n += line.rfind("[Images] requests=", 0) == 0;
        return n;
    }
};

// The items of a fake game. Reading one costs time on the host's clock.
class FakeItems : public ImageLookup::ItemSource
{
public:
    struct Item
    {
        std::string            name;
        std::string            path;
        ImageLookup::ItemRead  read = ImageLookup::ItemRead::Icon;
    };

    std::vector<Item>   items;
    bool                listable   = true;
    uint64_t*           clockUs    = nullptr;
    uint64_t            readCostUs = 0;
    int                 lists      = 0;
    std::vector<size_t> reads;

    bool List() override
    {
        ++lists;
        return listable;
    }
    size_t Count() const override { return items.size(); }
    ImageLookup::ItemRead Read(size_t index, ImageLookup::ItemRecord& out) override
    {
        reads.push_back(index);
        if (clockUs)
            *clockUs += readCostUs;
        out.name = items[index].name;
        out.texturePath = items[index].path;
        return items[index].read;
    }

    void Add(const std::string& name, const char* path)
    {
        items.push_back({ name, path, ImageLookup::ItemRead::Icon });
    }
};

// A library, a host and a pump that has seen the GPU come up.
struct Rig
{
    Library   library;
    FakeHost  host;
    FakeItems items;
    FakeItems buildings;
    Pump      pump;

    explicit Rig(bool gpuUp = true) : pump(library, host, items, buildings)
    {
        host.ready = gpuUp;
        host.clockUs = 1000 * 1000;
        items.clockUs = &host.clockUs;
        buildings.clockUs = &host.clockUs;
    }

    // One engine tick, a millisecond later than the last.
    void Tick(uint32_t n = 1)
    {
        while (n--)
        {
            host.clockUs += 1000;
            pump.Tick();
        }
    }

    // An image whose texture is in memory already.
    PluginImageId Loaded(const char* path)
    {
        host.loaded.insert(path);
        return library.Path(path);
    }
};

static const char* kA = "/Game/UI/T_A.T_A";
static const char* kB = "/Game/UI/T_B.T_B";
static const char* kC = "/Game/UI/T_C.T_C";

// ---------------------------------------------------------------------------

static void NothingBeforeTheGpuIsUp()
{
    Rig rig(false);
    const PluginImageId a = rig.Loaded(kA);
    rig.Tick(5);
    CHECK(rig.host.reads.empty());
    CHECK(rig.host.loads.empty());
    CHECK(rig.library.State(a) == PLUGIN_IMAGE_WAITING);

    rig.host.ready = true;
    rig.Tick(2);
    CHECK(rig.host.reads.size() == 1);
    CHECK(rig.library.State(a) == PLUGIN_IMAGE_READY);
}

static void CapsFollowTheVideoBudget()
{
    // 4 percent of 1 MB is 41943 bytes: a 256 px copy (262144 bytes) does not fit.
    Rig rig;
    rig.host.budget = 1024 * 1024;
    const PluginImageId a = rig.Loaded(kA);
    rig.Tick(2);
    CHECK(rig.library.State(a) == PLUGIN_IMAGE_NONE);
    CHECK(rig.library.CountKept() == 0);
    // The refused copy is freed by the pump.
    CHECK(rig.host.freed.size() == 1);

    // An unknown budget keeps the largest cap.
    Rig other;
    const PluginImageId b = other.Loaded(kB);
    other.Tick(2);
    CHECK(other.library.State(b) == PLUGIN_IMAGE_READY);
}

static void AStartedReadIsPublishedWhenTheCopyIsDone()
{
    Rig rig;
    const PluginImageId a = rig.Loaded(kA);
    rig.host.copyState = CopyState::Pending;
    rig.Tick(3);
    CHECK(rig.host.reads.size() == 1);   // read once, then waiting on the GPU
    CHECK(rig.library.State(a) == PLUGIN_IMAGE_WAITING);
    CHECK(rig.library.Get(a) == nullptr);

    rig.host.copyState = CopyState::Done;
    rig.Tick();
    CHECK(rig.library.State(a) == PLUGIN_IMAGE_READY);
    CHECK(rig.library.Get(a) != nullptr);
    CHECK(rig.library.BytesKept() == 349524);   // 256 x 256 with its 9 levels
    CHECK(rig.pump.GetStats().published == 1);
}

static void TheShrinkMakesTheStoredSizeAndCountsItsLevels()
{
    // A 1024 x 512 read is stored at 256 x 128, 9 levels.
    Rig big;
    big.host.width = 1024;
    big.host.height = 512;
    big.Loaded(kA);
    big.Tick(2);
    CHECK(big.library.BytesKept() == ImageShrink::Bytes({ 256, 128 }, 9));
    CHECK(big.library.BytesKept() == 174764);

    // A 128 x 128 read is under the stored size: it keeps its size, 8 levels.
    Rig small;
    small.host.width = 128;
    small.host.height = 128;
    small.Loaded(kA);
    small.Tick(2);
    CHECK(small.library.BytesKept() == ImageShrink::Bytes({ 128, 128 }, 8));
    CHECK(small.library.BytesKept() == 87380);
}

static void TheTemporaryCopyIsLetGoOnceTheShrinkStarts()
{
    Rig rig;
    const PluginImageId a = rig.Loaded(kA);
    rig.host.copyState = CopyState::Pending;
    rig.Tick(3);
    CHECK(rig.host.shrinkCalls == 0);       // the shrink waits for the copy
    CHECK(rig.host.copiesFreed.empty());

    rig.host.copyState = CopyState::Done;
    rig.Tick();
    CHECK(rig.host.shrinkCalls == 1);
    CHECK(rig.host.shrunkFrom.size() == 1);
    CHECK(rig.host.copiesFreed.size() == 1 && rig.host.copiesFreed[0] == rig.host.shrunkFrom[0]);
    CHECK(rig.host.freed.empty());          // what the library keeps is not freed
    CHECK(rig.library.Get(a) == rig.host.made[0]);
    rig.Tick(3);
    CHECK(rig.host.shrinkCalls == 1);       // shrunk once
}

static void AShrinkWithNoRoomIsTriedAgainNextTick()
{
    Rig rig;
    const PluginImageId a = rig.Loaded(kA);
    rig.host.shrinkResult = ShrinkResult::Busy;
    rig.Tick(4);
    CHECK(rig.host.shrinkCalls == 3);       // once a tick after the read
    CHECK(rig.library.State(a) == PLUGIN_IMAGE_WAITING);
    CHECK(rig.host.copiesFreed.empty());
    CHECK(rig.host.freed.empty());
    CHECK(rig.pump.GetStats().failed == 0);

    rig.host.shrinkResult = ShrinkResult::Started;
    rig.Tick();
    CHECK(rig.library.State(a) == PLUGIN_IMAGE_READY);
    CHECK(rig.host.reads.size() == 1);      // the copy was kept, not read again
    CHECK(rig.pump.GetStats().published == 1);
}

static void AShrinkThatFailsEndsAsNone()
{
    Rig rig;
    const PluginImageId a = rig.Loaded(kA);
    rig.host.shrinkResult = ShrinkResult::Failed;
    rig.Tick(2);
    CHECK(rig.library.State(a) == PLUGIN_IMAGE_NONE);
    CHECK(rig.pump.GetStats().failed == 1);
    CHECK(rig.host.freed.size() == 1);      // the copy is freed
    rig.Tick(3);
    CHECK(rig.host.shrinkCalls == 1);       // not tried again
    CHECK(rig.host.reads.size() == 1);
}

static void TheRequestKeepsTheNameItWasAskedWith()
{
    Rig rig;
    rig.Loaded(kA);
    rig.Tick();
    CHECK(rig.host.reads.size() == 1 && rig.host.reads[0] == kA);
}

static void ACopyThatIsGoneEndsAsNone()
{
    Rig rig;
    const PluginImageId a = rig.Loaded(kA);
    rig.host.copyState = CopyState::Gone;
    rig.Tick(2);
    CHECK(rig.library.State(a) == PLUGIN_IMAGE_NONE);
    CHECK(rig.pump.GetStats().failed == 1);
    rig.Tick(3);
    CHECK(rig.host.reads.size() == 1);   // not asked again
}

static void ACopyWithNoSizeIsRefused()
{
    Rig rig;
    rig.host.width = 0;
    const PluginImageId a = rig.Loaded(kA);
    rig.Tick(2);
    CHECK(rig.library.State(a) == PLUGIN_IMAGE_NONE);
    CHECK(rig.host.freed.size() == 1);
}

static void NotYetAndBusyAreRequeued()
{
    Rig rig;
    const PluginImageId a = rig.Loaded(kA);
    rig.host.readResult[kA] = ReadResult::NotYet;
    rig.Tick(3);
    CHECK(rig.host.reads.size() == 3);   // once per tick, still wanted
    CHECK(rig.library.State(a) == PLUGIN_IMAGE_WAITING);

    rig.host.readResult[kA] = ReadResult::Busy;
    rig.Tick(2);
    CHECK(rig.host.reads.size() == 5);
    CHECK(rig.library.State(a) == PLUGIN_IMAGE_WAITING);

    rig.host.readResult[kA] = ReadResult::Started;
    rig.Tick(2);
    CHECK(rig.library.State(a) == PLUGIN_IMAGE_READY);
}

static void BusyEndsTheTick()
{
    Rig rig;
    rig.Loaded(kA);
    rig.Loaded(kB);
    rig.host.readResult[kA] = ReadResult::Busy;
    rig.host.readResult[kB] = ReadResult::Busy;
    rig.Tick();
    CHECK(rig.host.reads.size() == 1);
}

static void FailedIsNotAskedAgain()
{
    Rig rig;
    const PluginImageId a = rig.Loaded(kA);
    rig.host.readResult[kA] = ReadResult::Failed;
    rig.Tick(4);
    CHECK(rig.host.reads.size() == 1);
    CHECK(rig.library.State(a) == PLUGIN_IMAGE_NONE);
    CHECK(rig.pump.GetStats().failed == 1);
    CHECK(rig.library.Get(a) == nullptr);
    rig.Tick(2);
    CHECK(rig.host.reads.size() == 1);   // Get did not ask again
}

static void UnavailableTurnsTheLibraryOff()
{
    Rig rig;
    const PluginImageId a = rig.Loaded(kA);
    const PluginImageId b = rig.Loaded(kB);
    const PluginImageId item = rig.library.Item("Item_X");
    rig.host.readResult[kA] = ReadResult::Unavailable;
    rig.host.readResult[kB] = ReadResult::Unavailable;
    rig.Tick();
    CHECK(rig.library.State(a) == PLUGIN_IMAGE_NONE);
    CHECK(rig.library.State(b) == PLUGIN_IMAGE_NONE);
    CHECK(rig.library.State(item) == PLUGIN_IMAGE_NONE);
    const size_t reads = rig.host.reads.size();

    const PluginImageId late = rig.library.Path("/Game/UI/T_Late.T_Late");
    rig.Tick(3);
    CHECK(rig.host.reads.size() == reads);   // nothing is read any more
    CHECK(rig.library.State(late) == PLUGIN_IMAGE_NONE);
}

static void APathNotInMemoryIsLoadedThenRead()
{
    Rig rig;
    const PluginImageId a = rig.library.Path(kA);
    rig.Tick();
    CHECK(rig.host.loads.size() == 1);
    CHECK(rig.host.reads.size() == 2);   // not there, then there
    rig.Tick();
    CHECK(rig.library.State(a) == PLUGIN_IMAGE_READY);
}

static void ALoadedTextureThatIsNotReadableYetIsRetried()
{
    Rig rig;
    rig.host.readAfterLoad = ReadResult::NotYet;
    const PluginImageId a = rig.library.Path(kA);
    rig.Tick();
    CHECK(rig.host.loads.size() == 1);
    CHECK(rig.library.State(a) == PLUGIN_IMAGE_WAITING);

    rig.host.readResult[kA] = ReadResult::Started;
    rig.Tick(2);
    CHECK(rig.host.loads.size() == 1);   // loaded once
    CHECK(rig.library.State(a) == PLUGIN_IMAGE_READY);
}

static void LoadResultsEndTheRequest()
{
    Rig rig;
    const PluginImageId missing = rig.library.Path("/Game/UI/T_Missing.T_Missing");
    const PluginImageId fault = rig.library.Path("/Game/UI/T_Fault.T_Fault");
    const PluginImageId unsupported = rig.library.Path("/Game/UI/T_Unsupported.T_Unsupported");
    rig.host.loadResult["/Game/UI/T_Missing.T_Missing"] = LoadResult::Missing;
    rig.host.loadResult["/Game/UI/T_Fault.T_Fault"] = LoadResult::Fault;
    rig.host.loadResult["/Game/UI/T_Unsupported.T_Unsupported"] = LoadResult::Unsupported;
    rig.Tick(5);
    CHECK(rig.library.State(missing) == PLUGIN_IMAGE_NO_IMAGE);
    CHECK(rig.library.State(fault) == PLUGIN_IMAGE_NONE);
    CHECK(rig.library.State(unsupported) == PLUGIN_IMAGE_NONE);
    CHECK(rig.pump.GetStats().noImage == 1);
    CHECK(rig.pump.GetStats().failed == 2);
    CHECK(rig.host.loads.size() == 3);   // each tried once
}

static void ALoadThatFindsNothingIsNoImage()
{
    // The load said it loaded, and the texture is still not there.
    Rig rig;
    rig.host.readAfterLoad = ReadResult::NotLoaded;
    const PluginImageId a = rig.library.Path(kA);
    rig.Tick(2);
    CHECK(rig.library.State(a) == PLUGIN_IMAGE_NO_IMAGE);
}

static void OneLoadPerTick()
{
    Rig rig;
    const PluginImageId a = rig.library.Path(kA);
    const PluginImageId b = rig.library.Path(kB);
    const PluginImageId c = rig.library.Path(kC);
    rig.host.readAfterLoad = ReadResult::NotYet;   // keeps the reads from ending the tick
    rig.Tick();
    CHECK(rig.host.loads.size() == 1);
    rig.Tick();
    CHECK(rig.host.loads.size() == 2);
    rig.Tick();
    CHECK(rig.host.loads.size() == 3);
    CHECK(rig.library.State(a) == PLUGIN_IMAGE_WAITING);
    CHECK(rig.library.State(b) == PLUGIN_IMAGE_WAITING);
    CHECK(rig.library.State(c) == PLUGIN_IMAGE_WAITING);
}

static void OneReadStartedPerTick()
{
    Rig rig;
    const PluginImageId a = rig.Loaded(kA);
    const PluginImageId b = rig.Loaded(kB);
    const PluginImageId c = rig.Loaded(kC);
    rig.Tick();
    CHECK(rig.host.reads.size() == 1);
    rig.Tick();
    CHECK(rig.host.reads.size() == 2);
    rig.Tick();
    CHECK(rig.host.reads.size() == 3);
    rig.Tick();
    CHECK(rig.library.State(a) == PLUGIN_IMAGE_READY);
    CHECK(rig.library.State(b) == PLUGIN_IMAGE_READY);
    CHECK(rig.library.State(c) == PLUGIN_IMAGE_READY);
}

static void RequestsLookedAtPerTickAreCapped()
{
    Rig rig;
    for (int i = 0; i < 20; ++i)
    {
        const std::string path = "/Game/UI/T_" + std::to_string(i) + ".T_" + std::to_string(i);
        rig.Loaded(path.c_str());
        rig.host.readResult[path] = ReadResult::NotYet;
    }
    rig.Tick();
    CHECK(rig.host.reads.size() == kAttemptsPerTick);
}

static void AnItemBecomesTheImageOfItsIconTexture()
{
    Rig rig;
    rig.items.Add("Item_X", kA);
    rig.host.loaded.insert(kA);
    const PluginImageId item = rig.library.Item("Item_X");

    rig.Tick(3);
    const PluginImageId path = rig.library.Path(kA);
    CHECK(rig.library.State(item) == PLUGIN_IMAGE_READY);
    CHECK(rig.library.State(path) == PLUGIN_IMAGE_READY);
    CHECK(rig.host.reads.size() == 1 && rig.host.reads[0] == kA);   // one read, one copy
    CHECK(rig.library.CountKept() == 1);
    CHECK(rig.library.Get(item) != nullptr && rig.library.Get(item) == rig.library.Get(path));
    CHECK(rig.library.ImageCount() == 2);   // the item and the path it made
}

static void AnItemIsAskedForByAnyCase()
{
    Rig rig;
    rig.items.Add("Item_X", kA);
    rig.host.loaded.insert(kA);
    const PluginImageId item = rig.library.Item("  item_x ");
    rig.Tick(3);
    CHECK(rig.library.State(item) == PLUGIN_IMAGE_READY);
}

static void AnItemWithNoIconIsNoImage()
{
    Rig rig;
    rig.items.items.push_back({ "Item_Empty", "", ImageLookup::ItemRead::NoIcon });
    const PluginImageId item = rig.library.Item("Item_Empty");
    rig.Tick(2);
    CHECK(rig.library.State(item) == PLUGIN_IMAGE_NO_IMAGE);
    CHECK(rig.host.reads.empty());
    CHECK(rig.pump.GetStats().noImage == 1);
}

static void AnItemWhoseIconIsAPlaceholderIsNoImage()
{
    Rig rig;
    rig.items.Add("Item_White", "/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture");
    rig.items.Add("Item_Grid", "/Engine/EngineMaterials/DefaultWhiteGrid.DefaultWhiteGrid");
    rig.items.Add("Item_NoItem", "/Game/UI/Icons/T_NoItem_White_Icon.T_NoItem_White_Icon");
    const PluginImageId white = rig.library.Item("Item_White");
    const PluginImageId grid = rig.library.Item("Item_Grid");
    const PluginImageId noItem = rig.library.Item("Item_NoItem");
    rig.Tick(3);
    CHECK(rig.library.State(white) == PLUGIN_IMAGE_NO_IMAGE);
    CHECK(rig.library.State(grid) == PLUGIN_IMAGE_NO_IMAGE);
    CHECK(rig.library.State(noItem) == PLUGIN_IMAGE_NO_IMAGE);
    CHECK(rig.host.reads.empty());
    CHECK(rig.host.loads.empty());
    CHECK(rig.library.ImageCount() == 3);   // no path request was made for either
}

static void APathToAPlaceholderIsNoImage()
{
    Rig rig;
    const PluginImageId white = rig.library.Path("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture");
    const PluginImageId placeholder = rig.library.Path("Texture2D'/Game/Scene/WhitePlaceholder.WhitePlaceholder'");
    rig.Tick(3);
    CHECK(rig.library.State(white) == PLUGIN_IMAGE_NO_IMAGE);
    CHECK(rig.library.State(placeholder) == PLUGIN_IMAGE_NO_IMAGE);
    CHECK(rig.host.reads.empty());
    CHECK(rig.host.loads.empty());

    // A texture that only has the word in its name is a texture.
    const PluginImageId other = rig.Loaded("/Game/UI/T_WhiteSquareTextureIcon.T_WhiteSquareTextureIcon");
    rig.Tick(2);
    CHECK(rig.library.State(other) == PLUGIN_IMAGE_READY);
}

static void AnItemThatIsNotInTheGameIsNoneOnceTheListIsRead()
{
    Rig rig;
    rig.items.Add("Item_X", kA);
    const PluginImageId unknown = rig.library.Item("Item_Nobody");
    rig.Tick(2);
    CHECK(rig.library.State(unknown) == PLUGIN_IMAGE_NONE);
    CHECK(rig.pump.GetStats().failed == 1);
    CHECK(rig.pump.GetStats().noImage == 0);
}

static void AnItemWaitsWhileTheListIsStillBeingRead()
{
    Rig rig;
    rig.items.readCostUs = ImageLookup::kItemBudgetUs;   // one item per tick
    for (int i = 0; i < 5; ++i)
        rig.items.Add("Item_" + std::to_string(i), kA);
    rig.host.loaded.insert(kA);
    const PluginImageId last = rig.library.Item("Item_4");
    rig.Tick(3);
    CHECK(rig.items.reads.size() == 3);
    CHECK(rig.library.State(last) == PLUGIN_IMAGE_WAITING);   // not in the index yet
    CHECK(rig.host.reads.empty());                            // and nothing read for it

    rig.Tick(5);
    CHECK(rig.library.State(last) == PLUGIN_IMAGE_READY);
}

static void TheItemIndexReadsNothingUntilAnItemIsAskedFor()
{
    Rig rig;
    rig.items.Add("Item_X", kA);
    rig.Loaded(kB);
    rig.Tick(5);
    CHECK(rig.items.lists == 0);
    CHECK(rig.items.reads.empty());

    rig.library.Item("Item_X");
    rig.Tick(2);
    CHECK(rig.items.lists == 1);
    CHECK(rig.items.reads.size() == 1);
}

static void TheItemIndexIsBuiltOnceAndStopsWhenNobodyWaits()
{
    Rig rig;
    rig.items.readCostUs = ImageLookup::kItemBudgetUs;   // one item per tick
    for (int i = 0; i < 6; ++i)
        rig.items.Add("Item_" + std::to_string(i), kA);
    rig.host.loaded.insert(kA);

    rig.library.Item("Item_1");
    rig.Tick(4);
    const size_t readSoFar = rig.items.reads.size();
    CHECK(readSoFar < 6);   // stopped when the item was found
    rig.Tick(5);
    CHECK(rig.items.reads.size() == readSoFar);   // nobody waits: no more reads

    // A later item goes on from there, and the first is not read again.
    const PluginImageId later = rig.library.Item("Item_5");
    rig.Tick(8);
    CHECK(rig.library.State(later) == PLUGIN_IMAGE_READY);
    CHECK(rig.items.lists == 1);
    CHECK(rig.items.reads.size() == 6);
}

static void TheItemIndexSpendsTheBudgetPerTick()
{
    // 1.5 ms per item against 3 ms: the second read is the last one of the tick.
    Rig rig;
    rig.items.readCostUs = 1500;
    for (int i = 0; i < 10; ++i)
        rig.items.Add("Item_" + std::to_string(i), kA);
    rig.library.Item("Item_Nobody");
    rig.Tick();
    CHECK(rig.items.reads.size() == 2);
    rig.Tick();
    CHECK(rig.items.reads.size() == 4);

    // One slow item runs over once; the next tick goes on from there.
    Rig slow;
    slow.items.readCostUs = 5 * ImageLookup::kItemBudgetUs;
    for (int i = 0; i < 4; ++i)
        slow.items.Add("Item_" + std::to_string(i), kA);
    slow.library.Item("Item_Nobody");
    slow.Tick();
    CHECK(slow.items.reads.size() == 1);
    slow.Tick();
    CHECK(slow.items.reads.size() == 2);
}

static void ManyItemsAskedForAreHeldWithoutCostingReads()
{
    Rig rig;
    rig.items.Add("Item_0", kA);
    rig.host.loaded.insert(kA);
    std::vector<PluginImageId> ids;
    for (int i = 0; i < 30; ++i)
        ids.push_back(rig.library.Item(("Item_" + std::to_string(i)).c_str()));
    const PluginImageId b = rig.Loaded(kB);
    rig.Tick(4);
    CHECK(rig.library.State(b) == PLUGIN_IMAGE_READY);
    CHECK(rig.library.State(ids[0]) == PLUGIN_IMAGE_READY);
    for (int i = 1; i < 30; ++i)
        CHECK(rig.library.State(ids[i]) == PLUGIN_IMAGE_NONE);
}

static void WhenTheListCannotBeMadeItemsAreNone()
{
    Rig rig;
    rig.items.listable = false;
    const PluginImageId item = rig.library.Item("Item_X");
    rig.Tick(2);
    CHECK(rig.library.State(item) == PLUGIN_IMAGE_NONE);
    CHECK(rig.pump.GetStats().failed == 1);

    // Asked again later: the list is not made again, and the item is none at once.
    const PluginImageId later = rig.library.Item("Item_Y");
    rig.Tick(2);
    CHECK(rig.library.State(later) == PLUGIN_IMAGE_NONE);
    CHECK(rig.items.lists == 1);

    // A path is not held up by it.
    const PluginImageId a = rig.Loaded(kA);
    rig.Tick(2);
    CHECK(rig.library.State(a) == PLUGIN_IMAGE_READY);
    size_t unavailableLines = 0;
    for (const std::string& line : rig.host.logs)
        unavailableLines += line.find("No item can be read") != std::string::npos;
    CHECK(unavailableLines == 1);
}

static void AnItemTheEngineCannotReadEndsTheIndex()
{
    Rig rig;
    rig.items.items.push_back({ "Item_A", "", ImageLookup::ItemRead::Unavailable });
    rig.items.Add("Item_X", kA);
    const PluginImageId item = rig.library.Item("Item_X");
    rig.Tick(2);
    CHECK(rig.library.State(item) == PLUGIN_IMAGE_NONE);
}

static void AnItemThatCannotBeReadIsSkipped()
{
    Rig rig;
    rig.items.items.push_back({ "Item_Broken", "", ImageLookup::ItemRead::Skipped });
    rig.items.Add("Item_X", kA);
    rig.host.loaded.insert(kA);
    const PluginImageId item = rig.library.Item("Item_X");
    const PluginImageId broken = rig.library.Item("Item_Broken");
    rig.Tick(3);
    CHECK(rig.library.State(item) == PLUGIN_IMAGE_READY);
    CHECK(rig.library.State(broken) == PLUGIN_IMAGE_NONE);
}

static void WaitingItemsEndWhenTheGpuGoes()
{
    Rig rig;
    rig.items.readCostUs = ImageLookup::kItemBudgetUs;
    for (int i = 0; i < 5; ++i)
        rig.items.Add("Item_" + std::to_string(i), kA);
    const PluginImageId item = rig.library.Item("Item_4");
    rig.Tick(2);
    CHECK(rig.library.State(item) == PLUGIN_IMAGE_WAITING);

    rig.host.ready = false;
    rig.Tick();
    CHECK(rig.library.State(item) == PLUGIN_IMAGE_NONE);
}

static void WaitingItemsEndAtShutdown()
{
    Rig rig;
    rig.items.readCostUs = ImageLookup::kItemBudgetUs;
    for (int i = 0; i < 5; ++i)
        rig.items.Add("Item_" + std::to_string(i), kA);
    const PluginImageId item = rig.library.Item("Item_4");
    rig.Tick(2);
    CHECK(rig.library.State(item) == PLUGIN_IMAGE_WAITING);

    rig.pump.Shutdown();
    CHECK(rig.library.State(item) == PLUGIN_IMAGE_NONE);
}

static void EvictedCopiesAreFreedOnTheNextTick()
{
    Rig rig;
    const PluginImageId a = rig.Loaded(kA);
    rig.Tick(2);
    rig.library.SetCaps(ImageLibrary::kByteCapMax, 1);

    // A is seen after B was asked for, so B is the one to go.
    rig.host.clockUs += 1000;
    const PluginImageId b = rig.Loaded(kB);
    rig.host.clockUs += 1000;
    rig.pump.Tick();
    rig.library.Get(a);
    rig.host.clockUs += 1000;
    rig.pump.Tick();
    CHECK(rig.library.State(a) == PLUGIN_IMAGE_READY);
    CHECK(rig.library.State(b) != PLUGIN_IMAGE_READY);
    CHECK(rig.pump.GetStats().evicted == 1);
    CHECK(rig.host.freed.empty());   // not on the tick that evicted it
    rig.Tick();
    CHECK(rig.host.freed.size() == 1);
}

static void AnEvictedImageIsReadAgainWhenDrawn()
{
    Rig rig;
    const PluginImageId a = rig.Loaded(kA);
    rig.Tick(2);
    rig.library.SetCaps(ImageLibrary::kByteCapMax, 1);
    rig.host.clockUs += 1000;
    const PluginImageId b = rig.Loaded(kB);
    rig.Tick(3);   // B is newer, so A is evicted
    CHECK(rig.library.State(a) != PLUGIN_IMAGE_READY);
    CHECK(rig.library.State(b) == PLUGIN_IMAGE_READY);
    const size_t freed = rig.host.freed.size();
    CHECK(freed == 1);

    rig.library.Get(a);   // drawn again
    CHECK(rig.library.State(a) == PLUGIN_IMAGE_WAITING);
    rig.library.SetCaps(ImageLibrary::kByteCapMax, 2);
    rig.Tick(3);
    CHECK(rig.library.State(a) == PLUGIN_IMAGE_READY);
    CHECK(rig.host.reads.size() == 3);
}

static void ACopyEvictedAtOnceIsFreedOnce()
{
    Rig rig;
    const PluginImageId a = rig.Loaded(kA);
    rig.Tick(2);
    rig.library.SetCaps(ImageLibrary::kByteCapMax, 1);
    rig.host.clockUs += 1000;
    const PluginImageId b = rig.Loaded(kB);
    rig.host.clockUs += 1000;
    rig.pump.Tick();                   // B starts reading
    rig.host.clockUs += 1000;
    rig.library.Get(a);                // A is the more recently seen
    rig.pump.Tick();                   // B's copy is done and published: B is the one to go
    CHECK(rig.library.State(b) != PLUGIN_IMAGE_READY);
    CHECK(rig.library.State(a) == PLUGIN_IMAGE_READY);
    rig.Tick(3);
    // The shrinks made A's texture, then B's.
    CHECK(rig.host.made.size() == 2);
    const PluginTextureHandle textureB = rig.host.made[1];
    CHECK(rig.host.Freed(textureB) == 1);
    CHECK(rig.host.freed.size() == 1);
}

static void TheGpuGoingAwayDropsEverythingWithoutFreeing()
{
    Rig rig;
    const PluginImageId a = rig.Loaded(kA);
    rig.Tick(2);
    CHECK(rig.library.State(a) == PLUGIN_IMAGE_READY);
    const PluginImageId b = rig.Loaded(kB);
    rig.host.copyState = CopyState::Pending;
    rig.Tick();   // B in flight
    const PluginImageId c = rig.library.Path(kC);

    rig.host.ready = false;
    rig.Tick();
    CHECK(rig.library.State(a) == PLUGIN_IMAGE_NONE);
    CHECK(rig.library.Get(a) == nullptr);
    CHECK(rig.library.State(b) == PLUGIN_IMAGE_NONE);
    CHECK(rig.library.State(c) == PLUGIN_IMAGE_NONE);
    CHECK(rig.library.CountKept() == 0);
    CHECK(rig.library.BytesKept() == 0);
    CHECK(rig.host.freed.empty());   // the texture API releases its own

    const size_t reads = rig.host.reads.size();
    const PluginImageId late = rig.Loaded("/Game/UI/T_Late.T_Late");
    rig.Tick(4);
    CHECK(rig.host.reads.size() == reads);
    CHECK(rig.library.State(late) == PLUGIN_IMAGE_NONE);
    CHECK(rig.host.freed.empty());
}

static void ShutdownFreesWhatTheLibraryKeeps()
{
    Rig rig;
    const PluginImageId a = rig.Loaded(kA);
    const PluginImageId b = rig.Loaded(kB);
    rig.Tick(3);
    rig.host.copyState = CopyState::Pending;
    const PluginImageId c = rig.Loaded(kC);
    rig.Tick();   // C in flight
    CHECK(rig.library.State(a) == PLUGIN_IMAGE_READY);

    rig.pump.Shutdown();
    CHECK(rig.host.freed.size() == 3);   // A, B and the copy in flight
    CHECK(rig.library.State(a) == PLUGIN_IMAGE_NONE);
    CHECK(rig.library.Get(a) == nullptr);
    CHECK(rig.library.State(c) == PLUGIN_IMAGE_NONE);
    CHECK(rig.library.CountKept() == 0);

    const size_t reads = rig.host.reads.size();
    rig.library.Path("/Game/UI/T_Late.T_Late");
    rig.Tick(4);
    CHECK(rig.host.reads.size() == reads);   // does nothing after shutdown

    rig.pump.Shutdown();
    CHECK(rig.host.freed.size() == 3);       // and nothing is freed twice
}

static void ShutdownAlsoFreesTheEvictedWaitingForTheNextTick()
{
    Rig rig;
    rig.Loaded(kA);
    rig.Tick(2);
    rig.library.SetCaps(ImageLibrary::kByteCapMax, 1);
    rig.host.clockUs += 1000;
    rig.Loaded(kB);
    rig.host.clockUs += 1000;
    rig.pump.Tick();
    rig.pump.Tick();   // B published, A evicted, not freed yet
    CHECK(rig.host.freed.empty());
    rig.pump.Shutdown();
    CHECK(rig.host.freed.size() == 2);   // B kept, A evicted
}

static void ShutdownAfterTheGpuWentFreesNothing()
{
    Rig rig;
    rig.Loaded(kA);
    rig.Tick(2);
    rig.host.ready = false;
    rig.Tick();
    rig.pump.Shutdown();
    CHECK(rig.host.freed.empty());
}

static void ShutdownBeforeTheGpuWasUpIsQuiet()
{
    Rig rig(false);
    rig.Loaded(kA);
    rig.Tick(2);
    rig.pump.Shutdown();
    CHECK(rig.host.freed.empty());
    CHECK(rig.host.SummaryLines() == 0);
}

static void TheSummaryLineComesOnceAMinute()
{
    Rig rig;
    rig.host.readCostUs = 700;
    rig.Loaded(kA);
    rig.Loaded(kB);
    rig.Tick(3);
    CHECK(rig.host.SummaryLines() == 0);   // the first minute is not over

    rig.host.clockUs += 61ull * 1000 * 1000;
    rig.Tick();
    CHECK(rig.host.SummaryLines() == 1);
    const std::string line = rig.host.logs.back();
    CHECK(line.find("requests=2") != std::string::npos);
    CHECK(line.find("published=2") != std::string::npos);
    CHECK(line.find("evicted=0") != std::string::npos);
    CHECK(line.find("failed=0") != std::string::npos);
    CHECK(line.find("kept=2") != std::string::npos);
    CHECK(line.find("longest pump tick=0.700 ms") != std::string::npos);

    // Nothing more inside the next minute, however many ticks.
    rig.host.clockUs += 30ull * 1000 * 1000;
    rig.Tick(50);
    CHECK(rig.host.SummaryLines() == 1);

    // A minute on with nothing new to say: no line.
    rig.host.clockUs += 40ull * 1000 * 1000;
    rig.Tick();
    CHECK(rig.host.SummaryLines() == 1);

    // And with something new: one.
    rig.Loaded(kC);
    rig.Tick(2);
    rig.host.clockUs += 61ull * 1000 * 1000;
    rig.Tick();
    CHECK(rig.host.SummaryLines() == 2);
}

static void NothingIsLoggedPerTick()
{
    Rig rig;
    rig.Loaded(kA);
    rig.Loaded(kB);
    rig.Loaded(kC);
    rig.Tick(30);
    CHECK(rig.host.logs.size() <= 1);
}

// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Preload

static const uint32_t kItemsSet     = PLUGIN_IMAGE_SET_ITEMS;
static const uint32_t kBuildingsSet = PLUGIN_IMAGE_SET_BUILDINGS;

static const IPluginSelf* Self(int i)
{
    static char selves[4];
    return reinterpret_cast<const IPluginSelf*>(&selves[i]);
}

static std::string PathN(int n)
{
    const std::string s = std::to_string(n);
    return "/Game/UI/T_" + s + ".T_" + s;
}

static void AddIcons(FakeItems& source, int first, int count, const char* prefix)
{
    for (int i = first; i < first + count; ++i)
        source.Add(prefix + std::to_string(i), PathN(i).c_str());
}

static void PutInMemory(Rig& rig, int first, int count)
{
    for (int i = first; i < first + count; ++i)
        rig.host.loaded.insert(PathN(i));
}

// The setting on, a roomy video budget, and a plugin using the sets.
static void Prepare(Rig& rig, uint32_t sets)
{
    rig.host.preload = true;
    rig.host.budget  = 8ull * 1024 * 1024 * 1024;
    rig.library.UseSets(Self(0), sets);
}

static size_t MemberReads(const Rig& rig, int first, int count)
{
    size_t n = 0;
    for (const std::string& path : rig.host.reads)
        for (int i = first; i < first + count; ++i)
            n += path == PathN(i);
    return n;
}

static int ReadyCount(Rig& rig, int first, int count, uint32_t sets)
{
    int n = 0;
    for (int i = first; i < first + count; ++i)
        n += rig.library.State(rig.library.PathInSet(PathN(i).c_str(), sets)) == PLUGIN_IMAGE_READY;
    return n;
}

static bool Logged(const Rig& rig, const char* text)
{
    for (const std::string& line : rig.host.logs)
        if (line.find(text) != std::string::npos)
            return true;
    return false;
}

static std::string LastSummary(const Rig& rig)
{
    for (size_t i = rig.host.logs.size(); i-- > 0;)
        if (rig.host.logs[i].rfind("[Images] requests=", 0) == 0)
            return rig.host.logs[i];
    return std::string();
}

static void ListsAreBuiltOnlyWhenTheSettingIsOn()
{
    Rig rig;
    AddIcons(rig.items, 0, 4, "Item");
    PutInMemory(rig, 0, 4);
    rig.library.UseSets(Self(0), kItemsSet);

    rig.Tick(20);
    CHECK(rig.items.lists == 0);
    CHECK(rig.host.reads.empty());

    rig.host.preload = true;
    rig.Tick(20);
    CHECK(rig.items.lists == 1);
    CHECK(Logged(rig, "[Images] Preload set: 4 item images"));
}

static void NoSetInUseMeansNoListAndNoReads()
{
    Rig rig;
    AddIcons(rig.items, 0, 4, "Item");
    PutInMemory(rig, 0, 4);
    rig.host.preload = true;

    rig.Tick(20);
    CHECK(rig.items.lists == 0);
    CHECK(rig.host.reads.empty());

    rig.host.clockUs += 61ull * 1000 * 1000;
    rig.Tick();
    CHECK(LastSummary(rig).find("paused=no set in use") != std::string::npos);

    // A plugin that asks for a set after the pump has seen none.
    rig.library.UseSets(Self(0), kItemsSet);
    rig.Tick(20);
    CHECK(rig.items.lists == 1);
}

static void TheItemsSetPreloads()
{
    Rig rig;
    AddIcons(rig.items, 0, 4, "Item");
    PutInMemory(rig, 0, 4);
    Prepare(rig, kItemsSet);

    rig.Tick(40);
    CHECK(ReadyCount(rig, 0, 4, kItemsSet) == 4);
    CHECK(rig.library.CountKept() == 4);
    CHECK(rig.buildings.lists == 0);   // buildings are not in use
}

static void TheBuildingsSetPreloads()
{
    Rig rig;
    AddIcons(rig.buildings, 10, 3, "Building");
    PutInMemory(rig, 10, 3);
    Prepare(rig, kBuildingsSet);

    rig.Tick(40);
    CHECK(ReadyCount(rig, 10, 3, kBuildingsSet) == 3);
    CHECK(rig.items.lists == 0);
    CHECK(Logged(rig, "[Images] Preload set: 3 building images"));
}

static void BothSetsAreListedAndPreloaded()
{
    Rig rig;
    AddIcons(rig.items, 0, 3, "Item");
    AddIcons(rig.buildings, 10, 3, "Building");
    PutInMemory(rig, 0, 3);
    PutInMemory(rig, 10, 3);
    Prepare(rig, kItemsSet | kBuildingsSet);

    rig.Tick(80);
    CHECK(ReadyCount(rig, 0, 3, kItemsSet) == 3);
    CHECK(ReadyCount(rig, 10, 3, kBuildingsSet) == 3);
    CHECK(rig.items.lists == 1);
    CHECK(rig.buildings.lists == 1);
}

static void AnIconTwoItemsShareIsOneMember()
{
    Rig rig;
    rig.items.Add("Iron", PathN(0).c_str());
    rig.items.Add("Iron Plate", PathN(0).c_str());
    rig.items.Add("Copper", PathN(1).c_str());
    PutInMemory(rig, 0, 2);
    Prepare(rig, kItemsSet);

    rig.Tick(40);
    CHECK(Logged(rig, "[Images] Preload set: 2 item images"));
    CHECK(MemberReads(rig, 0, 2) == 2);
}

static void DroppingInterestStopsPreloadOfThatSet()
{
    Rig rig;
    AddIcons(rig.items, 0, 8, "Item");
    PutInMemory(rig, 0, 8);
    Prepare(rig, kItemsSet);
    rig.library.UseSets(Self(1), kItemsSet);

    rig.Tick(8);
    const size_t before = MemberReads(rig, 0, 8);
    CHECK(before > 0 && before < 8);

    // One of two plugins lets go: the set is still in use.
    rig.library.ForgetPlugin(Self(0));
    rig.Tick(6);
    CHECK(MemberReads(rig, 0, 8) > before);

    // The last one lets go: nothing more is read.
    rig.library.UseSets(Self(1), 0);
    const size_t after = MemberReads(rig, 0, 8);
    rig.Tick(40);
    CHECK(MemberReads(rig, 0, 8) == after);
    CHECK(ReadyCount(rig, 0, 8, kItemsSet) < 8);

    // And it picks up again where it was.
    rig.library.UseSets(Self(2), kItemsSet);
    rig.Tick(60);
    CHECK(ReadyCount(rig, 0, 8, kItemsSet) == 8);
}

static void AtMostOneReadInTwoTicks()
{
    Rig rig;
    AddIcons(rig.items, 0, 40, "Item");
    PutInMemory(rig, 0, 40);
    Prepare(rig, kItemsSet);

    size_t last = 0;
    bool twoInARow = false, previousRead = false;
    for (int i = 0; i < 30; ++i)
    {
        rig.Tick();
        const size_t now = rig.host.reads.size();
        CHECK(now - last <= 1);
        const bool read = now > last;
        twoInARow = twoInARow || (read && previousRead);
        previousRead = read;
        last = now;
    }
    CHECK(!twoInARow);
    CHECK(MemberReads(rig, 0, 40) >= 10);
}

static void PreloadPausesWhileAnImageSeenIsWaitingAndResumes()
{
    Rig rig;
    AddIcons(rig.items, 0, 6, "Item");
    PutInMemory(rig, 0, 6);
    Prepare(rig, kItemsSet);

    // A plugin draws an image that is not readable yet.
    rig.host.loaded.insert(kA);
    rig.host.readResult[kA] = ReadResult::NotYet;
    const PluginImageId a = rig.library.Path(kA);
    rig.library.Get(a);

    rig.Tick(30);
    CHECK(rig.library.State(a) == PLUGIN_IMAGE_WAITING);
    CHECK(MemberReads(rig, 0, 6) == 0);

    // It is answered: preload goes on.
    rig.host.readResult[kA] = ReadResult::Started;
    rig.Tick(40);
    CHECK(rig.library.State(a) == PLUGIN_IMAGE_READY);
    CHECK(ReadyCount(rig, 0, 6, kItemsSet) == 6);
}

static void PreloadStopsWhenTheGameHoldsTooMuchAndResumesWhenItIsLetGo()
{
    Rig rig;
    AddIcons(rig.items, 0, 24, "Item");
    Prepare(rig, kItemsSet);
    rig.host.sourceSize = 1024;   // 4 MiB each at 4 bytes a pixel; the limit is 64 MiB

    // The images are not in memory: the library makes the game load each one.
    rig.Tick(200);
    CHECK(rig.host.loads.size() == 16);
    CHECK(ReadyCount(rig, 0, 24, kItemsSet) == 16);

    rig.host.clockUs += 61ull * 1000 * 1000;   // inside the window still
    rig.Tick(20);
    CHECK(rig.host.loads.size() == 16);
    CHECK(LastSummary(rig).find("paused=game holds loaded images") != std::string::npos);

    // The window passes: the game has let go of the first sixteen.
    rig.host.clockUs += 61ull * 1000 * 1000;
    rig.Tick(100);
    CHECK(rig.host.loads.size() == 24);
    CHECK(ReadyCount(rig, 0, 24, kItemsSet) == 24);
}

static void AnImageTheGameAlreadyHeldCostsNothingAgainstTheLimit()
{
    Rig rig;
    AddIcons(rig.items, 0, 24, "Item");
    PutInMemory(rig, 0, 24);
    Prepare(rig, kItemsSet);
    rig.host.sourceSize = 1024;

    rig.Tick(200);
    CHECK(rig.host.loads.empty());
    CHECK(ReadyCount(rig, 0, 24, kItemsSet) == 24);
}

static void PreloadWaitsWhileVideoMemoryIsAtNinetyPercent()
{
    Rig rig;
    AddIcons(rig.items, 0, 6, "Item");
    PutInMemory(rig, 0, 6);
    Prepare(rig, kItemsSet);
    rig.host.videoKnown  = true;
    rig.host.videoBudget = 1000;
    rig.host.videoUsage  = 900;

    rig.Tick(20);
    CHECK(MemberReads(rig, 0, 6) == 0);
    CHECK(rig.host.videoChecks == 1);

    // Room again, but the adapter is asked once a second.
    rig.host.videoUsage = 100;
    rig.Tick(20);
    CHECK(MemberReads(rig, 0, 6) == 0);
    CHECK(rig.host.videoChecks == 1);

    rig.host.clockUs += 1100 * 1000;
    rig.Tick(40);
    CHECK(rig.host.videoChecks >= 2);
    CHECK(ReadyCount(rig, 0, 6, kItemsSet) == 6);

    // Under the line by one unit is room.
    Rig under;
    AddIcons(under.items, 0, 2, "Item");
    PutInMemory(under, 0, 2);
    Prepare(under, kItemsSet);
    under.host.videoKnown  = true;
    under.host.videoBudget = 1000;
    under.host.videoUsage  = 899;
    under.Tick(20);
    CHECK(ReadyCount(under, 0, 2, kItemsSet) == 2);
}

static void PreloadGoesOnWhenVideoMemoryCannotBeRead()
{
    Rig rig;
    AddIcons(rig.items, 0, 3, "Item");
    PutInMemory(rig, 0, 3);
    Prepare(rig, kItemsSet);
    rig.host.videoKnown  = false;
    rig.host.videoUsage  = 999;
    rig.host.videoBudget = 1000;

    rig.Tick(30);
    CHECK(ReadyCount(rig, 0, 3, kItemsSet) == 3);
}

static void PreloadStopsAtTheCapsAndNeverEvicts()
{
    Rig rig;
    AddIcons(rig.items, 0, 6, "Item");
    PutInMemory(rig, 0, 6);
    Prepare(rig, kItemsSet);
    rig.host.budget = 25000000;   // a byte cap of 1,000,000: two stored images, not three

    rig.Tick(100);
    CHECK(rig.library.CountKept() == 2);
    CHECK(rig.pump.GetStats().evicted == 0);
    CHECK(rig.host.freed.empty());
    const size_t reads = rig.host.reads.size();

    rig.Tick(100);
    CHECK(rig.host.reads.size() == reads);

    rig.host.clockUs += 61ull * 1000 * 1000;
    rig.Tick();
    CHECK(LastSummary(rig).find("paused=cache full") != std::string::npos);
}

static void DrawingAnIdleMemberJumpsTheQueue()
{
    Rig rig;
    AddIcons(rig.items, 0, 6, "Item");
    PutInMemory(rig, 0, 6);
    Prepare(rig, kItemsSet);

    for (int i = 0; i < 10 && !Logged(rig, "[Images] Preload set"); ++i)
        rig.Tick();
    CHECK(Logged(rig, "[Images] Preload set"));
    CHECK(MemberReads(rig, 5, 1) == 0);

    const PluginImageId last = rig.library.PathInSet(PathN(5).c_str(), kItemsSet);
    rig.library.Get(last);
    rig.Tick();
    CHECK(!rig.host.reads.empty() && rig.host.reads.back() == PathN(5));

    rig.Tick(2);
    CHECK(rig.library.State(last) == PLUGIN_IMAGE_READY);
}

static void TheSettingTakesEffectAtOnce()
{
    Rig rig;
    AddIcons(rig.items, 0, 12, "Item");
    PutInMemory(rig, 0, 12);
    Prepare(rig, kItemsSet);

    rig.Tick(8);
    const size_t on = MemberReads(rig, 0, 12);
    CHECK(on > 0 && on < 12);

    rig.host.preload = false;
    rig.Tick(40);
    CHECK(MemberReads(rig, 0, 12) == on);

    rig.host.preload = true;
    rig.Tick(80);
    CHECK(ReadyCount(rig, 0, 12, kItemsSet) == 12);
    CHECK(rig.items.lists == 1);   // the list is kept, not read again
}

static void TheSummaryLineHasThePreloadFigures()
{
    Rig rig;
    AddIcons(rig.items, 0, 6, "Item");
    PutInMemory(rig, 0, 6);
    Prepare(rig, kItemsSet);

    rig.Tick(8);
    rig.host.clockUs += 61ull * 1000 * 1000;
    rig.Tick();
    const std::string mid = LastSummary(rig);
    CHECK(mid.find("preload queued=") != std::string::npos);
    CHECK(mid.find("done=") != std::string::npos);
    CHECK(mid.find("paused=") != std::string::npos);
    CHECK(mid.find("preload queued=0 ") == std::string::npos);

    rig.Tick(60);
    rig.host.clockUs += 61ull * 1000 * 1000;
    rig.Tick();
    CHECK(LastSummary(rig).find("preload queued=0 done=6 paused=no") != std::string::npos);

    rig.host.preload = false;
    rig.host.clockUs += 61ull * 1000 * 1000;
    rig.Tick();
    CHECK(LastSummary(rig).find("paused=setting off") != std::string::npos);
}

static void AnImageNotReadableYetIsDeferredAndDoesNotBlockTheRest()
{
    Rig rig;
    AddIcons(rig.items, 0, 4, "Item");
    PutInMemory(rig, 0, 4);
    rig.host.readResult[PathN(1)] = ReadResult::NotYet;
    Prepare(rig, kItemsSet);

    rig.Tick(80);
    CHECK(rig.library.State(rig.library.PathInSet(PathN(0).c_str(), kItemsSet)) == PLUGIN_IMAGE_READY);
    CHECK(rig.library.State(rig.library.PathInSet(PathN(2).c_str(), kItemsSet)) == PLUGIN_IMAGE_READY);
    CHECK(rig.library.State(rig.library.PathInSet(PathN(3).c_str(), kItemsSet)) == PLUGIN_IMAGE_READY);
    CHECK(rig.library.State(rig.library.PathInSet(PathN(1).c_str(), kItemsSet)) != PLUGIN_IMAGE_READY);

    // Once it can be read it is.
    rig.host.readResult[PathN(1)] = ReadResult::Started;
    rig.Tick(20);
    CHECK(ReadyCount(rig, 0, 4, kItemsSet) == 4);
}

static void AListThatCannotBeReadLeavesTheOtherSetWorking()
{
    Rig rig;
    rig.items.listable = false;
    AddIcons(rig.buildings, 10, 3, "Building");
    PutInMemory(rig, 10, 3);
    Prepare(rig, kItemsSet | kBuildingsSet);

    rig.Tick(80);
    CHECK(Logged(rig, "[Images] The item list cannot be read here"));
    CHECK(ReadyCount(rig, 10, 3, kBuildingsSet) == 3);
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    RunTest("nothing happens before the GPU is up", NothingBeforeTheGpuIsUp);
    RunTest("caps follow the video budget", CapsFollowTheVideoBudget);
    RunTest("a started read is published when the copy is done", AStartedReadIsPublishedWhenTheCopyIsDone);
    RunTest("the shrink makes the stored size and counts its levels", TheShrinkMakesTheStoredSizeAndCountsItsLevels);
    RunTest("the temporary copy is let go once the shrink starts", TheTemporaryCopyIsLetGoOnceTheShrinkStarts);
    RunTest("a shrink with no room is tried again next tick", AShrinkWithNoRoomIsTriedAgainNextTick);
    RunTest("a shrink that fails ends as none", AShrinkThatFailsEndsAsNone);
    RunTest("the request keeps the name it was asked with", TheRequestKeepsTheNameItWasAskedWith);
    RunTest("a copy that is gone ends as none", ACopyThatIsGoneEndsAsNone);
    RunTest("a copy with no size is refused", ACopyWithNoSizeIsRefused);
    RunTest("not yet and busy are requeued", NotYetAndBusyAreRequeued);
    RunTest("busy ends the tick", BusyEndsTheTick);
    RunTest("failed is not asked again", FailedIsNotAskedAgain);
    RunTest("unavailable turns the library off", UnavailableTurnsTheLibraryOff);
    RunTest("a path not in memory is loaded, then read", APathNotInMemoryIsLoadedThenRead);
    RunTest("a loaded texture that is not readable yet is retried", ALoadedTextureThatIsNotReadableYetIsRetried);
    RunTest("load results end the request", LoadResultsEndTheRequest);
    RunTest("a load that finds nothing is no image", ALoadThatFindsNothingIsNoImage);
    RunTest("one load per tick", OneLoadPerTick);
    RunTest("one read started per tick", OneReadStartedPerTick);
    RunTest("requests looked at per tick are capped", RequestsLookedAtPerTickAreCapped);
    RunTest("an item becomes the image of its icon texture", AnItemBecomesTheImageOfItsIconTexture);
    RunTest("an item is asked for by any case", AnItemIsAskedForByAnyCase);
    RunTest("an item with no icon is no image", AnItemWithNoIconIsNoImage);
    RunTest("an item whose icon is a placeholder is no image", AnItemWhoseIconIsAPlaceholderIsNoImage);
    RunTest("a path to a placeholder is no image", APathToAPlaceholderIsNoImage);
    RunTest("an item that is not in the game is none once the list is read", AnItemThatIsNotInTheGameIsNoneOnceTheListIsRead);
    RunTest("an item waits while the list is still being read", AnItemWaitsWhileTheListIsStillBeingRead);
    RunTest("the item index reads nothing until an item is asked for", TheItemIndexReadsNothingUntilAnItemIsAskedFor);
    RunTest("the item index is built once and stops when nobody waits", TheItemIndexIsBuiltOnceAndStopsWhenNobodyWaits);
    RunTest("the item index spends the budget per tick", TheItemIndexSpendsTheBudgetPerTick);
    RunTest("many items asked for are held without costing reads", ManyItemsAskedForAreHeldWithoutCostingReads);
    RunTest("when the list cannot be made items are none", WhenTheListCannotBeMadeItemsAreNone);
    RunTest("an item the engine cannot read ends the index", AnItemTheEngineCannotReadEndsTheIndex);
    RunTest("an item that cannot be read is skipped", AnItemThatCannotBeReadIsSkipped);
    RunTest("waiting items end when the GPU goes", WaitingItemsEndWhenTheGpuGoes);
    RunTest("waiting items end at shutdown", WaitingItemsEndAtShutdown);
    RunTest("evicted copies are freed on the next tick", EvictedCopiesAreFreedOnTheNextTick);
    RunTest("an evicted image is read again when drawn", AnEvictedImageIsReadAgainWhenDrawn);
    RunTest("a copy evicted at once is freed once", ACopyEvictedAtOnceIsFreedOnce);
    RunTest("the GPU going away drops everything without freeing", TheGpuGoingAwayDropsEverythingWithoutFreeing);
    RunTest("shutdown frees what the library keeps", ShutdownFreesWhatTheLibraryKeeps);
    RunTest("shutdown also frees the evicted waiting for the next tick", ShutdownAlsoFreesTheEvictedWaitingForTheNextTick);
    RunTest("shutdown after the GPU went frees nothing", ShutdownAfterTheGpuWentFreesNothing);
    RunTest("shutdown before the GPU was up is quiet", ShutdownBeforeTheGpuWasUpIsQuiet);
    RunTest("the summary line comes once a minute", TheSummaryLineComesOnceAMinute);
    RunTest("nothing is logged per tick", NothingIsLoggedPerTick);
    RunTest("preload lists are built only when the setting is on", ListsAreBuiltOnlyWhenTheSettingIsOn);
    RunTest("no set in use means no list and no reads", NoSetInUseMeansNoListAndNoReads);
    RunTest("the items set preloads", TheItemsSetPreloads);
    RunTest("the buildings set preloads", TheBuildingsSetPreloads);
    RunTest("both sets are listed and preloaded", BothSetsAreListedAndPreloaded);
    RunTest("an icon two items share is one member", AnIconTwoItemsShareIsOneMember);
    RunTest("dropping interest stops preload of that set", DroppingInterestStopsPreloadOfThatSet);
    RunTest("at most one read in two ticks", AtMostOneReadInTwoTicks);
    RunTest("preload pauses while an image seen is waiting and resumes", PreloadPausesWhileAnImageSeenIsWaitingAndResumes);
    RunTest("preload stops when the game holds too much", PreloadStopsWhenTheGameHoldsTooMuchAndResumesWhenItIsLetGo);
    RunTest("an image the game already held costs nothing against the limit", AnImageTheGameAlreadyHeldCostsNothingAgainstTheLimit);
    RunTest("preload waits while video memory is at 90 percent", PreloadWaitsWhileVideoMemoryIsAtNinetyPercent);
    RunTest("preload goes on when video memory cannot be read", PreloadGoesOnWhenVideoMemoryCannotBeRead);
    RunTest("preload stops at the caps and never evicts", PreloadStopsAtTheCapsAndNeverEvicts);
    RunTest("drawing an idle member jumps the queue", DrawingAnIdleMemberJumpsTheQueue);
    RunTest("the setting takes effect at once", TheSettingTakesEffectAtOnce);
    RunTest("the summary line has the preload figures", TheSummaryLineHasThePreloadFigures);
    RunTest("an image not readable yet is deferred", AnImageNotReadableYetIsDeferredAndDoesNotBlockTheRest);
    RunTest("a list that cannot be read leaves the other set working", AListThatCannotBeReadLeavesTheOtherSetWorking);

    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
