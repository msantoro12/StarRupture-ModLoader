// Image lookup test: the rules of UI/image_lookup.cpp on a fake game. The images
// that count as no image, the item index and its budget per tick, and the
// texture lookup with its cache and how it is invalidated.
//
// A console program: no game. The lookups and the library are the loader's own
// sources; a texture here is an address that stands in for a texture object.
#include "UI/image_lookup.h"

#include <cstdio>
#include <map>
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

using namespace ImageLookup;
using ImageLibrary::Library;

// ---------------------------------------------------------------------------
// No image

static void PlaceholdersAreNoImage()
{
    CHECK(IsPlaceholderImage("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture"));
    CHECK(IsPlaceholderImage("/Engine/EngineMaterials/DefaultWhiteGrid.DefaultWhiteGrid"));
    CHECK(IsPlaceholderImage("/Game/Scene_UnfinishedBuilding/Textures/WhitePlaceholder.WhitePlaceholder"));
    CHECK(IsPlaceholderImage("/Game/UI/Icons/T_NoItem_White_Icon.T_NoItem_White_Icon"));
}

static void PlaceholdersAreRecognisedInEveryFormAPathComesIn()
{
    CHECK(IsPlaceholderImage("/ENGINE/engineresources/whitesquaretexture.WHITESQUARETEXTURE"));
    CHECK(IsPlaceholderImage("/Engine/EngineResources/WhiteSquareTexture"));   // the package path alone
    CHECK(IsPlaceholderImage("Texture2D'/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture'"));
    CHECK(IsPlaceholderImage("  /Engine/EngineMaterials/DefaultWhiteGrid.DefaultWhiteGrid  "));
}

static void OtherTexturesAreNotPlaceholders()
{
    CHECK(!IsPlaceholderImage("/Game/UI/Icons/T_IronPlate_Icon.T_IronPlate_Icon"));
    // The name has to be the asset's, not part of it or of the folder.
    CHECK(!IsPlaceholderImage("/Game/UI/T_WhiteSquareTextureIcon.T_WhiteSquareTextureIcon"));
    CHECK(!IsPlaceholderImage("/Game/WhiteSquareTexture/T_A.T_A"));
    CHECK(!IsPlaceholderImage("/Game/UI/T_A.WhiteSquareTexture_Extra"));
    CHECK(!IsPlaceholderImage("/Game/UI/T_NoItem_White_Icon_Big.T_NoItem_White_Icon_Big"));
    CHECK(!IsPlaceholderImage(""));
    CHECK(!IsPlaceholderImage("WhiteSquareTexture"));   // not a path
    CHECK(!IsPlaceholderImage(nullptr));
}

// ---------------------------------------------------------------------------
// Items

class FakeItems : public ItemSource
{
public:
    struct Item
    {
        std::string name;
        std::string path;
        ItemRead    read = ItemRead::Icon;
    };

    std::vector<Item>   items;
    bool                listable   = true;
    uint64_t            clockUs    = 0;
    uint64_t            readCostUs = 0;
    int                 lists      = 0;
    std::vector<size_t> reads;

    bool List() override
    {
        ++lists;
        return listable;
    }
    size_t Count() const override { return items.size(); }
    ItemRead Read(size_t index, ItemRecord& out) override
    {
        reads.push_back(index);
        clockUs += readCostUs;
        out.name = items[index].name;
        out.texturePath = items[index].path;
        return items[index].read;
    }

    void Add(const std::string& name, const std::string& path, ItemRead read = ItemRead::Icon)
    {
        items.push_back({ name, path, read });
    }
};

struct IndexRig
{
    FakeItems items;
    ItemIndex index;
    IndexRig() : index(items) {}

    void Step(uint64_t budgetUs = kItemBudgetUs)
    {
        index.Step(budgetUs, [this] { return items.clockUs; });
    }
};

static void AnItemIsFoundByItsNormalizedName()
{
    IndexRig rig;
    rig.items.Add("Item_IronPlate", "/Game/UI/T_Iron.T_Iron");
    rig.Step();

    std::string path;
    CHECK(rig.index.Find("item_ironplate", path) && path == "/Game/UI/T_Iron.T_Iron");
    CHECK(!rig.index.Find("item_copper", path));
    // The key is the library's: the same name in any case finds it.
    CHECK(rig.index.Find(ImageLibrary::NormalizeItemName("  ITEM_ironPLATE"), path));
}

static void AnItemWithNoIconIsKnownWithNoPath()
{
    IndexRig rig;
    rig.items.Add("Item_Empty", "", ItemRead::NoIcon);
    rig.Step();

    std::string path = "left over";
    CHECK(rig.index.Find("item_empty", path));
    CHECK(path.empty());
}

static void SkippedItemsAreNotInTheIndex()
{
    IndexRig rig;
    rig.items.Add("Item_Broken", "", ItemRead::Skipped);
    rig.items.Add("Item_Fine", "/Game/UI/T_A.T_A");
    rig.Step();

    std::string path;
    CHECK(!rig.index.Find("item_broken", path));
    CHECK(rig.index.Find("item_fine", path));
    CHECK(rig.index.GetState() == ItemIndex::State::Done);
}

static void TheFirstItemUnderANameIsKept()
{
    IndexRig rig;
    rig.items.Add("Item_X", "/Game/UI/T_First.T_First");
    rig.items.Add("item_x", "/Game/UI/T_Second.T_Second");
    rig.Step();

    std::string path;
    CHECK(rig.index.Find("item_x", path) && path == "/Game/UI/T_First.T_First");
}

static void AnItemWithoutAUsableNameIsNotKept()
{
    IndexRig rig;
    rig.items.Add("   ", "/Game/UI/T_A.T_A");
    rig.Step();

    std::string path;
    CHECK(!rig.index.Find("", path));
    CHECK(rig.index.GetState() == ItemIndex::State::Done);
}

static void TheListIsMadeOnTheFirstStepOnly()
{
    IndexRig rig;
    rig.items.Add("Item_A", "/Game/UI/T_A.T_A");
    CHECK(rig.index.GetState() == ItemIndex::State::Idle);
    CHECK(rig.items.lists == 0);

    rig.Step();
    rig.Step();
    CHECK(rig.items.lists == 1);
    CHECK(rig.index.GetState() == ItemIndex::State::Done);
    CHECK(rig.items.reads.size() == 1);   // nothing is read again once done
}

static void AnIndexWhoseListCannotBeMadeFailsOnce()
{
    IndexRig rig;
    rig.items.listable = false;
    rig.items.Add("Item_A", "/Game/UI/T_A.T_A");
    rig.Step();
    CHECK(rig.index.GetState() == ItemIndex::State::Failed);
    rig.Step();
    CHECK(rig.items.lists == 1);
    CHECK(rig.items.reads.empty());
}

static void AnUnavailableReadFailsTheIndex()
{
    IndexRig rig;
    rig.items.Add("Item_A", "/Game/UI/T_A.T_A");
    rig.items.Add("Item_B", "", ItemRead::Unavailable);
    rig.items.Add("Item_C", "/Game/UI/T_C.T_C");
    rig.Step();
    CHECK(rig.index.GetState() == ItemIndex::State::Failed);
    CHECK(rig.items.reads.size() == 2);   // nothing after it

    // What was read before stays known.
    std::string path;
    CHECK(rig.index.Find("item_a", path));
    CHECK(!rig.index.Find("item_c", path));
}

static void AStepStopsWhenTheBudgetIsSpent()
{
    IndexRig rig;
    rig.items.readCostUs = 1000;
    for (int i = 0; i < 10; ++i)
        rig.items.Add("Item_" + std::to_string(i), "/Game/UI/T_A.T_A");

    rig.Step(3000);
    CHECK(rig.items.reads.size() == 3);
    CHECK(rig.index.GetState() == ItemIndex::State::Scanning);
    rig.Step(3000);
    CHECK(rig.items.reads.size() == 6);
    rig.Step(3000);
    rig.Step(3000);
    CHECK(rig.items.reads.size() == 10);
    CHECK(rig.index.GetState() == ItemIndex::State::Done);

    std::string path;
    CHECK(rig.index.Find("item_9", path));
}

static void AStepReadsAtLeastOneItem()
{
    IndexRig rig;
    rig.items.readCostUs = 50 * 1000;   // far over the budget
    for (int i = 0; i < 3; ++i)
        rig.items.Add("Item_" + std::to_string(i), "/Game/UI/T_A.T_A");

    rig.Step();
    CHECK(rig.items.reads.size() == 1);
    rig.Step(0);
    CHECK(rig.items.reads.size() == 2);
}

static void TheBudgetIsTheLoadersThreeMilliseconds()
{
    CHECK(kItemBudgetUs == 3000);
}

static void AStepKnowsWhenTheListIsDone()
{
    IndexRig rig;
    rig.items.Add("Item_A", "/Game/UI/T_A.T_A");
    rig.items.Add("Item_B", "/Game/UI/T_B.T_B");
    rig.Step();
    CHECK(rig.index.GetState() == ItemIndex::State::Done);   // both fit in one slice

    IndexRig empty;
    empty.Step();
    CHECK(empty.index.GetState() == ItemIndex::State::Done);
}

static void TheTexturePathsAreSortedOnceAndSkipTheEmptyAndPlaceholders()
{
    IndexRig rig;
    rig.items.Add("Item_B", "/Game/UI/T_B.T_B");
    rig.items.Add("Item_A", "/Game/UI/T_A.T_A");
    rig.items.Add("Item_A2", "/Game/UI/T_A.T_A");
    rig.items.Add("Item_Empty", "", ItemRead::NoIcon);
    rig.items.Add("Item_White", "/Game/UI/Icons/T_NoItem_White_Icon.T_NoItem_White_Icon");
    rig.items.Add("Item_Broken", "", ItemRead::Skipped);
    rig.Step();

    const std::vector<std::string> paths = rig.index.TexturePaths();
    CHECK(paths.size() == 2);
    CHECK(paths.size() == 2 && paths[0] == "/Game/UI/T_A.T_A" && paths[1] == "/Game/UI/T_B.T_B");

    IndexRig none;
    none.Step();
    CHECK(none.index.TexturePaths().empty());
}

// ---------------------------------------------------------------------------
// Textures

class FakeEngine : public TextureEngine
{
public:
    struct Texture
    {
        TextureSlot slot;
        std::string path;
    };

    bool                           gameThread = true;
    std::map<const void*, Texture> textures;
    int                            slotCalls = 0, pathCalls = 0;
    std::vector<std::string>       logs;

    bool OnGameThread() override { return gameThread; }
    bool SlotOf(const void* texture, TextureSlot& out) override
    {
        ++slotCalls;
        const auto it = textures.find(texture);
        if (it == textures.end())
            return false;
        out = it->second.slot;
        return true;
    }
    bool PathOf(const void* texture, std::string& out) override
    {
        ++pathCalls;
        const auto it = textures.find(texture);
        if (it == textures.end() || it->second.path.empty())
            return false;
        out = it->second.path;
        return true;
    }
    void Log(const std::string& line) override { logs.push_back(line); }
};

struct TextureRig
{
    Library      library;
    FakeEngine   engine;
    TextureLookup lookup;
    int          a = 0, b = 0, c = 0;   // addresses of three textures

    TextureRig() : lookup(engine, library) {}

    void Put(const void* texture, int32_t index, int32_t serial, const char* path)
    {
        engine.textures[texture] = { { index, serial }, path };
    }
};

static void ATextureIsTheImageOfItsPath()
{
    TextureRig rig;
    rig.Put(&rig.a, 7, 100, "/Game/UI/T_A.T_A");
    const PluginImageId id = rig.lookup.FromTexture(&rig.a);
    CHECK(id != 0);
    CHECK(id == rig.library.Path("/Game/UI/T_A.T_A"));   // the id a plugin asking by path gets
    CHECK(rig.library.State(id) == PLUGIN_IMAGE_WAITING);
}

static void ATextureIsAskedForOnceInTheEngine()
{
    TextureRig rig;
    rig.Put(&rig.a, 7, 100, "/Game/UI/T_A.T_A");
    const PluginImageId first = rig.lookup.FromTexture(&rig.a);
    const PluginImageId second = rig.lookup.FromTexture(&rig.a);
    CHECK(first == second);
    CHECK(rig.engine.pathCalls == 1);
    CHECK(rig.engine.slotCalls == 2);   // the slot is checked every time: the pointer is not trusted
}

static void ASlotUsedByAnotherObjectIsLookedUpAgain()
{
    TextureRig rig;
    rig.Put(&rig.a, 7, 100, "/Game/UI/T_A.T_A");
    const PluginImageId before = rig.lookup.FromTexture(&rig.a);

    // The texture died and its slot went to another one: same index, new serial.
    rig.engine.textures.erase(&rig.a);
    rig.Put(&rig.b, 7, 101, "/Game/UI/T_B.T_B");
    const PluginImageId after = rig.lookup.FromTexture(&rig.b);
    CHECK(after != 0 && after != before);
    CHECK(rig.engine.pathCalls == 2);
    CHECK(after == rig.library.Path("/Game/UI/T_B.T_B"));
}

static void ASlotUsedAgainByTheSameAddressIsLookedUpAgain()
{
    // The address came back for another texture, in the same slot, with a new serial.
    TextureRig rig;
    rig.Put(&rig.a, 7, 100, "/Game/UI/T_A.T_A");
    const PluginImageId before = rig.lookup.FromTexture(&rig.a);
    rig.Put(&rig.a, 7, 101, "/Game/UI/T_B.T_B");
    const PluginImageId after = rig.lookup.FromTexture(&rig.a);
    CHECK(after != before);
    CHECK(rig.engine.pathCalls == 2);
}

static void TwoSlotsWithTheSamePathAreOneImage()
{
    TextureRig rig;
    rig.Put(&rig.a, 7, 100, "/Game/UI/T_A.T_A");
    rig.Put(&rig.b, 9, 200, "/Game/UI/T_A.T_A");
    CHECK(rig.lookup.FromTexture(&rig.a) == rig.lookup.FromTexture(&rig.b));
}

static void ADeadTextureGivesNothing()
{
    TextureRig rig;
    rig.Put(&rig.a, 7, 100, "/Game/UI/T_A.T_A");
    CHECK(rig.lookup.FromTexture(&rig.a) != 0);

    // The slot no longer holds it: the answer from before is not given.
    rig.engine.textures.erase(&rig.a);
    CHECK(rig.lookup.FromTexture(&rig.a) == 0);
    CHECK(rig.lookup.FromTexture(nullptr) == 0);
    CHECK(rig.lookup.FromTexture(&rig.c) == 0);   // never a texture
}

static void ATextureWithNoPathGivesNothingAndIsAskedAgain()
{
    TextureRig rig;
    rig.Put(&rig.a, 7, 100, "");
    CHECK(rig.lookup.FromTexture(&rig.a) == 0);
    CHECK(rig.lookup.FromTexture(&rig.a) == 0);
    CHECK(rig.engine.pathCalls == 2);   // nothing was remembered

    rig.Put(&rig.a, 7, 100, "/Game/UI/T_A.T_A");
    CHECK(rig.lookup.FromTexture(&rig.a) != 0);
}

static void OtherThreadsGetZeroAndOneLogLine()
{
    TextureRig rig;
    rig.Put(&rig.a, 7, 100, "/Game/UI/T_A.T_A");
    rig.engine.gameThread = false;
    CHECK(rig.lookup.FromTexture(&rig.a) == 0);
    CHECK(rig.lookup.FromTexture(&rig.a) == 0);
    CHECK(rig.engine.logs.size() == 1);
    CHECK(rig.engine.slotCalls == 0 && rig.engine.pathCalls == 0);   // the engine was not touched

    rig.engine.gameThread = true;
    CHECK(rig.lookup.FromTexture(&rig.a) != 0);
    CHECK(rig.engine.logs.size() == 1);
}

static void APlaceholderTextureIsStillAnImageIdForThePumpToEnd()
{
    // FromTexture names the texture; ending it as no image is the pump's rule.
    TextureRig rig;
    rig.Put(&rig.a, 7, 100, "/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture");
    CHECK(rig.lookup.FromTexture(&rig.a) != 0);
}

static void TheCacheIsBoundedAndStillAnswers()
{
    TextureRig rig;
    std::vector<int> textures(kTextureCacheMax + 10);
    for (size_t i = 0; i < textures.size(); ++i)
        rig.Put(&textures[i], static_cast<int32_t>(i), 1, ("/Game/UI/T_" + std::to_string(i) + ".T_" + std::to_string(i)).c_str());
    for (size_t i = 0; i < textures.size(); ++i)
        CHECK(rig.lookup.FromTexture(&textures[i]) != 0);
    CHECK(rig.engine.pathCalls == static_cast<int>(textures.size()));

    // The oldest were dropped from the memory; they are looked up again, and the id is the same.
    const PluginImageId again = rig.lookup.FromTexture(&textures[0]);
    CHECK(again == rig.library.Path("/Game/UI/T_0.T_0"));
    CHECK(rig.engine.pathCalls == static_cast<int>(textures.size()) + 1);
}

// ---------------------------------------------------------------------------

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    RunTest("placeholders are no image", PlaceholdersAreNoImage);
    RunTest("placeholders are recognised in every form a path comes in", PlaceholdersAreRecognisedInEveryFormAPathComesIn);
    RunTest("other textures are not placeholders", OtherTexturesAreNotPlaceholders);
    RunTest("an item is found by its normalized name", AnItemIsFoundByItsNormalizedName);
    RunTest("an item with no icon is known with no path", AnItemWithNoIconIsKnownWithNoPath);
    RunTest("skipped items are not in the index", SkippedItemsAreNotInTheIndex);
    RunTest("the first item under a name is kept", TheFirstItemUnderANameIsKept);
    RunTest("an item without a usable name is not kept", AnItemWithoutAUsableNameIsNotKept);
    RunTest("the list is made on the first step only", TheListIsMadeOnTheFirstStepOnly);
    RunTest("an index whose list cannot be made fails once", AnIndexWhoseListCannotBeMadeFailsOnce);
    RunTest("an unavailable read fails the index", AnUnavailableReadFailsTheIndex);
    RunTest("a step stops when the budget is spent", AStepStopsWhenTheBudgetIsSpent);
    RunTest("a step reads at least one item", AStepReadsAtLeastOneItem);
    RunTest("the budget is three milliseconds", TheBudgetIsTheLoadersThreeMilliseconds);
    RunTest("a step knows when the list is done", AStepKnowsWhenTheListIsDone);
    RunTest("the texture paths are sorted once and skip the empty and placeholders", TheTexturePathsAreSortedOnceAndSkipTheEmptyAndPlaceholders);
    RunTest("a texture is the image of its path", ATextureIsTheImageOfItsPath);
    RunTest("a texture is asked for once in the engine", ATextureIsAskedForOnceInTheEngine);
    RunTest("a slot used by another object is looked up again", ASlotUsedByAnotherObjectIsLookedUpAgain);
    RunTest("a slot used again by the same address is looked up again", ASlotUsedAgainByTheSameAddressIsLookedUpAgain);
    RunTest("two slots with the same path are one image", TwoSlotsWithTheSamePathAreOneImage);
    RunTest("a dead texture gives nothing", ADeadTextureGivesNothing);
    RunTest("a texture with no path gives nothing and is asked again", ATextureWithNoPathGivesNothingAndIsAskedAgain);
    RunTest("other threads get zero and one log line", OtherThreadsGetZeroAndOneLogLine);
    RunTest("a placeholder texture is still an image id for the pump to end", APlaceholderTextureIsStillAnImageIdForThePumpToEnd);
    RunTest("the cache is bounded and still answers", TheCacheIsBoundedAndStillAnswers);

    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
