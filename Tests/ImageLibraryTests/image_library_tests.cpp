// Image library table test: ids and key normalisation, the four states, the
// order requests are served in, the byte and count caps with least-recently-seen
// eviction, set interest, and State and Get read from other threads.
//
// A console program: no game, no GPU. UI/image_library.cpp is the loader's own
// source; a texture here is a number standing in for the handle the pump makes.
#include "UI/image_library.h"

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
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

using namespace ImageLibrary;

// The texture handle the pump would have made for an image.
static PluginTextureHandle Texture(PluginImageId id)
{
    return reinterpret_cast<PluginTextureHandle>(static_cast<uintptr_t>(id) * 16);
}

// Takes the next request and publishes a texture of the given size for it.
static PluginImageId ReadNext(Library& lib, uint64_t bytes, std::vector<Released>& evicted)
{
    Request request;
    if (!lib.TakeNext(request))
        return 0;
    lib.Publish(request.id, Texture(request.id), bytes, evicted);
    return request.id;
}

static const IPluginSelf* Self(int i)
{
    static char selves[4];
    return reinterpret_cast<const IPluginSelf*>(&selves[i]);
}

// ---------------------------------------------------------------------------

static void NormalisesItemNames()
{
    CHECK(NormalizeItemName("Iron_Plate") == "iron_plate");
    CHECK(NormalizeItemName("  Iron_Plate\t") == "iron_plate");
    CHECK(NormalizeItemName("") == "");
    CHECK(NormalizeItemName("   ") == "");
    CHECK(NormalizeItemName(nullptr) == "");
    CHECK(NormalizeItemName(std::string(300, 'a').c_str()) == "");
    CHECK(NormalizeItemName("a\x01" "b") == "");
}

static void NormalisesObjectPaths()
{
    CHECK(NormalizeObjectPath("/Game/UI/T_X.T_X") == "/game/ui/t_x.t_x");
    CHECK(NormalizeObjectPath("/Game/UI/T_X") == "/game/ui/t_x.t_x");
    CHECK(NormalizeObjectPath(" /Game/UI/T_X.T_X ") == "/game/ui/t_x.t_x");
    CHECK(NormalizeObjectPath("Texture2D'/Game/UI/T_X.T_X'") == "/game/ui/t_x.t_x");
    CHECK(NormalizeObjectPath("Texture2D'/Game/UI/T_X'") == "/game/ui/t_x.t_x");
    CHECK(NormalizeObjectPath("") == "");
    CHECK(NormalizeObjectPath(nullptr) == "");
    CHECK(NormalizeObjectPath("Game/UI/T_X.T_X") == "");
    CHECK(NormalizeObjectPath("/Game/UI/") == "");
    CHECK(NormalizeObjectPath("/") == "");
    CHECK(NormalizeObjectPath("Texture2D'/Game/UI/T_X.T_X") == "");
    CHECK(NormalizeObjectPath(std::string(2000, '/').c_str()) == "");
}

static void IdsAreStableAndNeverZero()
{
    Library lib;
    const PluginImageId a = lib.Item("Iron_Plate");
    const PluginImageId b = lib.Item("  iron_plate ");
    const PluginImageId c = lib.Item("Copper_Plate");
    CHECK(a != 0);
    CHECK(a == b);
    CHECK(c != 0 && c != a);

    const PluginImageId p = lib.Path("/Game/UI/T_X.T_X");
    CHECK(p != 0 && p != a && p != c);
    CHECK(lib.Path("Texture2D'/game/ui/t_x'") == p);

    // An item and a path with the same text are different keys.
    CHECK(lib.Item("/Game/UI/T_X.T_X") != p);

    CHECK(lib.Item("") == 0);
    CHECK(lib.Path("not a path") == 0);
    CHECK(lib.State(0) == PLUGIN_IMAGE_NONE);
    CHECK(lib.Get(0) == nullptr);
    CHECK(lib.State(9999) == PLUGIN_IMAGE_NONE);
    CHECK(lib.Get(9999) == nullptr);
}

static void TheRequestKeepsTheNameAsAskedFor()
{
    Library lib;
    const PluginImageId id = lib.Path("Texture2D'/Game/UI/T_Icon.T_Icon'");
    const PluginImageId item = lib.Item("  Iron_Plate ");
    (void)lib.Item("iron_plate");

    Request first;
    Request second;
    CHECK(lib.TakeNext(first) && lib.TakeNext(second));
    const Request& pathRequest = first.id == id ? first : second;
    const Request& itemRequest = first.id == id ? second : first;
    CHECK(pathRequest.kind == Kind::Path && pathRequest.name == "/Game/UI/T_Icon.T_Icon");
    CHECK(itemRequest.id == item && itemRequest.kind == Kind::Item && itemRequest.name == "Iron_Plate");
}

static void IdsRunOutAtTheTableSize()
{
    Library lib;
    uint32_t last = 0;
    for (uint32_t i = 0; i < kMaxIds; ++i)
        last = lib.Item(("item_" + std::to_string(i)).c_str());
    CHECK(last == kMaxIds);
    CHECK(lib.Item("one_too_many") == 0);
    CHECK(lib.Item("item_5") == 6);
    CHECK(lib.ImageCount() == kMaxIds);
}

static void StatesFollowTheRead()
{
    Library lib;
    std::vector<Released> evicted;

    const PluginImageId a = lib.Item("a");
    CHECK(lib.State(a) == PLUGIN_IMAGE_WAITING);
    CHECK(lib.Get(a) == nullptr);

    Request request;
    CHECK(lib.TakeNext(request) && request.id == a);
    CHECK(lib.State(a) == PLUGIN_IMAGE_WAITING);   // being read
    CHECK(!lib.TakeNext(request));                 // not handed out twice

    CHECK(lib.Publish(a, Texture(a), 100, evicted));
    CHECK(lib.State(a) == PLUGIN_IMAGE_READY);
    CHECK(lib.Get(a) == Texture(a));
    CHECK(lib.BytesKept() == 100 && lib.CountKept() == 1);
    CHECK(evicted.empty());

    // Only a taken request can be answered, and only once.
    CHECK(!lib.Publish(a, Texture(a), 100, evicted));
    CHECK(lib.BytesKept() == 100);

    const PluginImageId none = lib.Item("none");
    CHECK(lib.TakeNext(request) && request.id == none);
    lib.PublishNoImage(none);
    CHECK(lib.State(none) == PLUGIN_IMAGE_NO_IMAGE);
    CHECK(lib.Get(none) == nullptr);

    const PluginImageId bad = lib.Item("bad");
    CHECK(lib.TakeNext(request) && request.id == bad);
    lib.Fail(bad);
    CHECK(lib.State(bad) == PLUGIN_IMAGE_NONE);
    CHECK(lib.Get(bad) == nullptr);
    CHECK(lib.State(bad) == PLUGIN_IMAGE_NONE);   // Get does not ask for a failed image again
    CHECK(!lib.TakeNext(request));

    // A read that cannot be done now goes back in the queue, still waiting.
    const PluginImageId later = lib.Item("later");
    CHECK(lib.TakeNext(request) && request.id == later);
    lib.Requeue(later);
    CHECK(lib.State(later) == PLUGIN_IMAGE_WAITING);
    CHECK(lib.TakeNext(request) && request.id == later);

    // A null texture is a failed read.
    CHECK(!lib.Publish(later, nullptr, 10, evicted));
    CHECK(lib.State(later) == PLUGIN_IMAGE_NONE);
    CHECK(lib.BytesKept() == 100);
}

static void StateDoesNotMarkSeen()
{
    Library lib;
    lib.SetNow(0);
    const PluginImageId a = lib.Item("a");
    lib.SetNow(kSeenLatelyMs + 1);
    CHECK(lib.State(a) == PLUGIN_IMAGE_WAITING);
    Request request;
    CHECK(!lib.TakeNext(request));
    CHECK(lib.State(a) == PLUGIN_IMAGE_NONE);
}

static void SeenImagesAreReadFirst()
{
    Library lib;
    const PluginImageId a = lib.Item("a");
    const PluginImageId b = lib.Item("b");
    const PluginImageId c = lib.Item("c");
    const PluginImageId d = lib.Item("d");

    lib.SetNow(1000);
    lib.Get(a);
    lib.SetNow(2000);
    lib.Get(c);
    lib.SetNow(3000);

    // c was seen last, then a. b and d were only asked for, at time 0; the
    // lower id goes first among equals.
    Request request;
    CHECK(lib.TakeNext(request) && request.id == c);
    CHECK(lib.TakeNext(request) && request.id == a);
    CHECK(lib.TakeNext(request) && request.id == b);
    CHECK(lib.TakeNext(request) && request.id == d);
    CHECK(!lib.TakeNext(request));
}

static void RequestsNobodyDrewAreDropped()
{
    Library lib;
    const PluginImageId seen = lib.Item("seen");
    const PluginImageId unseen = lib.Item("unseen");

    lib.SetNow(kSeenLatelyMs);
    lib.Get(seen);
    lib.SetNow(kSeenLatelyMs + 1);   // unseen was asked for at 0: just past "lately"

    Request request;
    CHECK(lib.TakeNext(request) && request.id == seen);
    CHECK(!lib.TakeNext(request));
    CHECK(lib.State(unseen) == PLUGIN_IMAGE_NONE);
    CHECK(lib.State(seen) == PLUGIN_IMAGE_WAITING);

    // Drawing it again asks again.
    CHECK(lib.Get(unseen) == nullptr);
    CHECK(lib.State(unseen) == PLUGIN_IMAGE_WAITING);
    CHECK(lib.TakeNext(request) && request.id == unseen);
    CHECK(!lib.TakeNext(request));

    // Dropped, drawn, dropped, drawn: always back to one request.
    lib.Requeue(unseen);
    lib.SetNow(10 * kSeenLatelyMs);
    CHECK(!lib.TakeNext(request));
    lib.Get(unseen);
    lib.Get(unseen);
    CHECK(lib.TakeNext(request) && request.id == unseen);
    CHECK(!lib.TakeNext(request));
}

static void SetMembersAreKeptAndServedAfterSeenOnes()
{
    Library lib;
    const PluginImageId member = lib.Item("member");
    const PluginImageId other = lib.Item("other");
    const PluginImageId seen = lib.Item("seen");
    lib.AddToSets(member, PLUGIN_IMAGE_SET_ITEMS);

    lib.SetNow(10 * kSeenLatelyMs);
    lib.Get(seen);

    // No plugin uses the item set yet: member is dropped like other.
    Request request;
    CHECK(lib.TakeNext(request) && request.id == seen);
    CHECK(!lib.TakeNext(request));
    CHECK(lib.State(member) == PLUGIN_IMAGE_NONE);

    // Asked for again, then nobody draws it, but a plugin uses its set.
    lib.Get(member);
    lib.UseSets(Self(0), PLUGIN_IMAGE_SET_ITEMS);
    lib.Requeue(seen);
    lib.SetNow(20 * kSeenLatelyMs);
    lib.Get(seen);
    CHECK(lib.TakeNext(request) && request.id == seen);   // seen lately, first
    CHECK(lib.TakeNext(request) && request.id == member); // kept: its set is used
    CHECK(!lib.TakeNext(request));
    CHECK(lib.State(other) == PLUGIN_IMAGE_NONE);
}

static void SetInterestIsRecordedAndDropped()
{
    Library lib;
    const PluginImageId member = lib.Item("member");
    lib.AddToSets(member, PLUGIN_IMAGE_SET_BUILDINGS);
    lib.SetNow(10 * kSeenLatelyMs);

    Request request;

    // Items are not what member belongs to.
    lib.UseSets(Self(0), PLUGIN_IMAGE_SET_ITEMS);
    CHECK(!lib.TakeNext(request));
    lib.Get(member);

    // Two plugins use it; the second is unloaded, the first still does.
    lib.UseSets(Self(1), PLUGIN_IMAGE_SET_BUILDINGS);
    lib.UseSets(Self(2), PLUGIN_IMAGE_SET_BUILDINGS);
    lib.SetNow(20 * kSeenLatelyMs);
    lib.ForgetPlugin(Self(1));
    CHECK(lib.TakeNext(request) && request.id == member);
    lib.Requeue(member);

    // 0 clears.
    lib.UseSets(Self(2), 0);
    CHECK(!lib.TakeNext(request));
    CHECK(lib.State(member) == PLUGIN_IMAGE_NONE);

    // Set again and unloaded without clearing: nothing is left.
    lib.Get(member);
    lib.UseSets(Self(2), PLUGIN_IMAGE_SET_BUILDINGS);
    lib.ForgetPlugin(Self(2));
    lib.SetNow(30 * kSeenLatelyMs);
    CHECK(!lib.TakeNext(request));
    CHECK(lib.State(member) == PLUGIN_IMAGE_NONE);

    // Bits that are not sets are ignored.
    lib.Get(member);
    lib.UseSets(Self(2), 0x80000000u);
    lib.SetNow(40 * kSeenLatelyMs);
    CHECK(!lib.TakeNext(request));
}

static void ByteCapEvictsLeastRecentlySeen()
{
    Library lib;
    lib.SetCaps(300, 100);
    std::vector<Released> evicted;

    const PluginImageId a = lib.Item("a");
    const PluginImageId b = lib.Item("b");
    const PluginImageId c = lib.Item("c");
    const PluginImageId d = lib.Item("d");

    // Read in id order, then draw them in the order c, a, b so that c is the
    // oldest by the time the cap is hit.
    lib.SetNow(10);
    for (int i = 0; i < 3; ++i)
        ReadNext(lib, 100, evicted);
    CHECK(evicted.empty() && lib.BytesKept() == 300);
    lib.SetNow(20); lib.Get(c);
    lib.SetNow(30); lib.Get(a);
    lib.SetNow(40); lib.Get(b);

    lib.SetNow(50);
    lib.Get(d);
    CHECK(ReadNext(lib, 100, evicted) == d);
    CHECK(evicted.size() == 1);
    CHECK(evicted[0].id == c && evicted[0].handle == Texture(c) && evicted[0].bytes == 100);
    CHECK(lib.State(c) == PLUGIN_IMAGE_NONE);
    CHECK(lib.State(a) == PLUGIN_IMAGE_READY && lib.State(b) == PLUGIN_IMAGE_READY && lib.State(d) == PLUGIN_IMAGE_READY);
    CHECK(lib.BytesKept() == 300 && lib.CountKept() == 3);

    // A big one pushes out as many as it takes, oldest first: a (30), b (40), d (50).
    const PluginImageId big = lib.Item("big");
    lib.SetNow(60);
    lib.Get(big);
    evicted.clear();
    CHECK(ReadNext(lib, 250, evicted) == big);
    CHECK(evicted.size() == 3 && evicted[0].id == a && evicted[1].id == b && evicted[2].id == d);
    CHECK(lib.State(big) == PLUGIN_IMAGE_READY);
    CHECK(lib.BytesKept() == 250 && lib.CountKept() == 1);
}

static void CountCapEvictsLeastRecentlySeen()
{
    Library lib;
    lib.SetCaps(1000000, 2);
    std::vector<Released> evicted;

    const PluginImageId a = lib.Item("a");
    const PluginImageId b = lib.Item("b");
    const PluginImageId c = lib.Item("c");
    lib.SetNow(1);
    ReadNext(lib, 10, evicted);
    ReadNext(lib, 10, evicted);
    lib.SetNow(2); lib.Get(b);
    lib.SetNow(3); lib.Get(a);
    lib.SetNow(4); lib.Get(c);
    ReadNext(lib, 10, evicted);

    CHECK(evicted.size() == 1 && evicted[0].id == b);
    CHECK(lib.CountKept() == 2 && lib.BytesKept() == 20);
    CHECK(lib.State(a) == PLUGIN_IMAGE_READY && lib.State(c) == PLUGIN_IMAGE_READY);
}

static void EqualStampsEvictTheLowerIdFirst()
{
    Library lib;
    lib.SetCaps(1000000, 2);
    std::vector<Released> evicted;
    const PluginImageId a = lib.Item("a");
    lib.Item("b");
    lib.Item("c");
    for (int i = 0; i < 3; ++i)
        ReadNext(lib, 10, evicted);
    CHECK(evicted.size() == 1 && evicted[0].id == a);
}

static void ACopyOverTheByteCapIsRefused()
{
    Library lib;
    lib.SetCaps(100, 10);
    std::vector<Released> evicted;
    const PluginImageId small = lib.Item("small");
    const PluginImageId huge = lib.Item("huge");
    ReadNext(lib, 50, evicted);

    Request request;
    CHECK(lib.TakeNext(request) && request.id == huge);
    CHECK(!lib.Publish(huge, Texture(huge), 101, evicted));
    CHECK(evicted.empty());                       // nothing else is sacrificed for it
    CHECK(lib.State(huge) == PLUGIN_IMAGE_NONE);  // and it is not asked for again
    lib.Get(huge);
    CHECK(lib.State(huge) == PLUGIN_IMAGE_NONE);
    CHECK(lib.State(small) == PLUGIN_IMAGE_READY && lib.BytesKept() == 50);
}

static void AnEvictedImageIsAskedForAgainByGet()
{
    Library lib;
    lib.SetCaps(100, 1);
    std::vector<Released> evicted;

    const PluginImageId a = lib.Item("a");
    const PluginImageId b = lib.Item("b");
    lib.SetNow(1);
    ReadNext(lib, 10, evicted);
    lib.SetNow(2); lib.Get(b);
    ReadNext(lib, 10, evicted);
    CHECK(evicted.size() == 1 && evicted[0].id == a);
    CHECK(lib.State(a) == PLUGIN_IMAGE_NONE);

    Request request;
    CHECK(!lib.TakeNext(request));   // nobody asked yet

    lib.SetNow(3);
    CHECK(lib.Get(a) == nullptr);
    CHECK(lib.State(a) == PLUGIN_IMAGE_WAITING);
    evicted.clear();
    CHECK(ReadNext(lib, 10, evicted) == a);
    CHECK(lib.Get(a) == Texture(a));
    CHECK(evicted.size() == 1 && evicted[0].id == b);

    // The id is the same one throughout.
    CHECK(lib.Item("a") == a);
}

static void ReleaseAllHandsBackEveryKeptCopy()
{
    Library lib;
    std::vector<Released> evicted;
    const PluginImageId a = lib.Item("a");
    const PluginImageId b = lib.Item("b");
    const PluginImageId c = lib.Item("c");
    lib.SetNow(1);
    lib.Get(a); lib.Get(b); lib.Get(c);
    ReadNext(lib, 10, evicted);
    ReadNext(lib, 20, evicted);   // c stays queued
    CHECK(lib.CountKept() == 2);

    std::vector<Released> released;
    lib.ReleaseAll(released);
    CHECK(released.size() == 2);
    CHECK(lib.CountKept() == 0);
    CHECK(lib.BytesKept() == 0);
    CHECK(lib.State(a) == PLUGIN_IMAGE_NONE);
    CHECK(lib.State(b) == PLUGIN_IMAGE_NONE);
    CHECK(lib.Get(a) == nullptr);

    // A second call finds nothing to give back.
    released.clear();
    lib.ReleaseAll(released);
    CHECK(released.empty());
}

static void ANewCapEvictsOnTheNextPublish()
{
    Library lib;
    std::vector<Released> evicted;
    lib.Item("a");
    lib.Item("b");
    ReadNext(lib, 100, evicted);
    ReadNext(lib, 100, evicted);
    CHECK(evicted.empty());
    lib.SetCaps(150, 10);
    lib.Item("c");
    ReadNext(lib, 10, evicted);
    CHECK(evicted.size() == 1 && evicted[0].id == 1);   // equal stamps: the lower id
    CHECK(lib.BytesKept() == 110 && lib.CountKept() == 2);
}

static void ByteCapFollowsTheVideoBudget()
{
    CHECK(ByteCapForBudget(0) == kByteCapMax);
    CHECK(ByteCapForBudget(2ull << 30) == (2ull << 30) / 100 * 4);
    CHECK(ByteCapForBudget(100ull << 30) == kByteCapMax);
    CHECK(ByteCapForBudget(6ull << 30) == (6ull << 30) / 100 * 4);   // just under 256 MB
    CHECK(ByteCapForBudget(8ull << 30) == kByteCapMax);
}

static void AliasedIdsAreOneImage()
{
    Library lib;
    std::vector<Released> evicted;

    const PluginImageId item = lib.Item("iron_plate");
    const PluginImageId path = lib.Path("/Game/UI/T_Iron.T_Iron");
    CHECK(lib.Alias(item, path));
    CHECK(!lib.Alias(item, path));   // once

    // One request, not two.
    Request request;
    CHECK(lib.TakeNext(request) && request.id == path);
    CHECK(!lib.TakeNext(request));
    CHECK(lib.State(item) == PLUGIN_IMAGE_WAITING);
    CHECK(lib.Publish(path, Texture(path), 100, evicted));

    CHECK(lib.State(item) == PLUGIN_IMAGE_READY && lib.State(path) == PLUGIN_IMAGE_READY);
    CHECK(lib.Get(item) == Texture(path) && lib.Get(path) == Texture(path));
    CHECK(lib.BytesKept() == 100 && lib.CountKept() == 1);

    // Drawing through either id keeps the one image seen.
    lib.SetCaps(100, 10);
    lib.SetNow(5);
    const PluginImageId other = lib.Item("other");
    lib.SetNow(6); lib.Get(item);
    lib.SetNow(7); lib.Get(other);
    CHECK(ReadNext(lib, 100, evicted) == other);   // the image seen through item is the older
    CHECK(evicted.size() == 1 && evicted[0].id == path);
    CHECK(lib.State(item) == PLUGIN_IMAGE_NONE);

    // Asking again through the alias asks for the target.
    lib.Get(item);
    CHECK(lib.State(path) == PLUGIN_IMAGE_WAITING);
    CHECK(lib.TakeNext(request) && request.id == path);
    CHECK(!lib.TakeNext(request));
}

static void AliasRefusals()
{
    Library lib;
    std::vector<Released> evicted;
    const PluginImageId a = lib.Item("a");
    const PluginImageId b = lib.Item("b");
    const PluginImageId c = lib.Item("c");
    CHECK(!lib.Alias(a, a));
    CHECK(!lib.Alias(a, 0));
    CHECK(!lib.Alias(0, a));
    CHECK(!lib.Alias(a, 777));

    // Already being read: too late.
    Request request;
    CHECK(lib.TakeNext(request) && request.id == a);
    CHECK(!lib.Alias(a, b));
    lib.Publish(a, Texture(a), 10, evicted);
    // Already kept.
    CHECK(!lib.Alias(a, b));

    // An alias of an alias lands on the end of the chain.
    CHECK(lib.Alias(b, c));
    const PluginImageId d = lib.Item("d");
    CHECK(lib.Alias(d, b));
    CHECK(lib.TakeNext(request) && request.id == c);
    CHECK(lib.Publish(c, Texture(c), 10, evicted));
    CHECK(lib.Get(d) == Texture(c));
    CHECK(!lib.Alias(c, d));   // d is c
    CHECK(!lib.Alias(b, a));   // b is an alias already
}

static void InterfaceReachesTheSameTable()
{
    IPluginImages* table = GetInterface();
    CHECK(table != nullptr);
    CHECK(table == GetInterface());

    const PluginImageId id = table->Item(Self(0), "Interface_Item");
    CHECK(id != 0 && id == table->Item(Self(1), "interface_item"));
    CHECK(id == Instance().Item("interface_item"));
    CHECK(table->Path(Self(0), "/Game/UI/T_I") == Instance().Path("/game/ui/t_i.t_i"));
    CHECK(table->State(id) == PLUGIN_IMAGE_WAITING);
    CHECK(table->Get(id) == nullptr);
    CHECK(table->Get(0) == nullptr && table->State(0) == PLUGIN_IMAGE_NONE);
    CHECK(table->FromTexture(Self(0), nullptr) == 0);

    table->UseSets(Self(0), PLUGIN_IMAGE_SET_ITEMS);
    table->UseSets(Self(0), 0);
    ImageLibrary::ForgetPlugin(Self(0));
}

// A reader that only ever calls Get and State, while the table creates ids,
// publishes and evicts under it. Every handle Get returns must be the one that
// belongs to that image, and the state must be one of the four.
static void ReadersSeeConsistentStatesWhileTheTableChanges()
{
    Library lib;
    lib.SetCaps(40 * 10, 40);
    constexpr uint32_t kImages = 200;
    std::vector<PluginImageId> ids;
    for (uint32_t i = 0; i < kImages; ++i)
        ids.push_back(lib.Item(("img_" + std::to_string(i)).c_str()));

    std::atomic<bool> stop{ false };
    std::atomic<int> wrong{ 0 };
    std::atomic<long> reads{ 0 };

    auto reader = [&]()
    {
        uint32_t n = 0;
        while (!stop.load())
        {
            const PluginImageId id = ids[n++ % kImages];
            const PluginTextureHandle handle = lib.Get(id);
            const int state = lib.State(id);
            if (handle && handle != Texture(id))
                ++wrong;
            if (state < PLUGIN_IMAGE_NONE || state > PLUGIN_IMAGE_NO_IMAGE)
                ++wrong;
            // An image id that does not exist reads as nothing.
            if (lib.Get(kMaxIds + 1 + n % 7) != nullptr || lib.State(kMaxIds + 1) != PLUGIN_IMAGE_NONE)
                ++wrong;
            ++reads;
        }
    };

    std::vector<std::thread> readers;
    for (int i = 0; i < 4; ++i)
        readers.emplace_back(reader);

    while (reads.load() == 0)
        std::this_thread::yield();

    // The pump: serves what the readers ask for, evicting as it goes, while
    // more ids appear.
    std::vector<Released> evicted;
    uint64_t now = 0;
    for (int round = 0; round < 20000; ++round)
    {
        lib.SetNow(++now);
        if (round % 50 == 0)
            lib.Item(("extra_" + std::to_string(round)).c_str());
        Request request;
        if (lib.TakeNext(request))
        {
            if (request.id % 7 == 0)
                lib.PublishNoImage(request.id);
            else if (request.id % 11 == 0)
                lib.Fail(request.id);
            else
                lib.Publish(request.id, Texture(request.id), 10, evicted);
        }
    }

    stop.store(true);
    for (auto& t : readers)
        t.join();

    CHECK(wrong.load() == 0);
    CHECK(reads.load() > 0);
    CHECK(lib.CountKept() <= 40 && lib.BytesKept() <= 400);
    CHECK(!evicted.empty());
}

// ---------------------------------------------------------------------------
// Sets and preload

static const char* kPathA = "/Game/UI/T_A.T_A";
static const char* kPathB = "/Game/UI/T_B.T_B";
static const char* kPathC = "/Game/UI/T_C.T_C";

static void AMemberOfASetIsIdleUntilPreloadReachesIt()
{
    Library lib;
    lib.SetNow(100000);
    const PluginImageId a = lib.PathInSet(kPathA, PLUGIN_IMAGE_SET_ITEMS);
    CHECK(a != 0);
    // The same image under another spelling, in another set: one id, in both sets.
    CHECK(lib.PathInSet("Texture2D'/game/ui/t_a'", PLUGIN_IMAGE_SET_BUILDINGS) == a);
    CHECK(lib.Path(kPathA) == a);
    CHECK(lib.PathInSet("not a path", PLUGIN_IMAGE_SET_ITEMS) == 0);

    // It is not asked for.
    CHECK(lib.State(a) == PLUGIN_IMAGE_NONE);
    Request request;
    CHECK(!lib.TakeNext(request));
    // No plugin uses a set: nothing to preload.
    CHECK(!lib.TakePreload(request));

    lib.UseSets(Self(0), PLUGIN_IMAGE_SET_BUILDINGS);
    CHECK(lib.TakePreload(request));
    CHECK(request.id == a && request.kind == Kind::Path && request.name == kPathA);
    CHECK(lib.State(a) == PLUGIN_IMAGE_WAITING);
    CHECK(!lib.TakePreload(request));   // taken: not handed out twice

    std::vector<Released> evicted;
    CHECK(lib.Publish(a, Texture(a), 100, evicted));
    CHECK(lib.State(a) == PLUGIN_IMAGE_READY);
    CHECK(!lib.TakePreload(request));   // kept
}

static void UsedSetsAreTheUnionAndFollowThePlugins()
{
    Library lib;
    CHECK(lib.UsedSets() == 0);
    lib.UseSets(Self(0), PLUGIN_IMAGE_SET_ITEMS);
    CHECK(lib.UsedSets() == PLUGIN_IMAGE_SET_ITEMS);
    lib.UseSets(Self(1), PLUGIN_IMAGE_SET_BUILDINGS);
    CHECK(lib.UsedSets() == (PLUGIN_IMAGE_SET_ITEMS | PLUGIN_IMAGE_SET_BUILDINGS));
    lib.UseSets(Self(1), PLUGIN_IMAGE_SET_ITEMS);
    CHECK(lib.UsedSets() == PLUGIN_IMAGE_SET_ITEMS);

    // A plugin that unloads drops its interest; so does one that clears it.
    lib.UseSets(Self(1), PLUGIN_IMAGE_SET_BUILDINGS);
    lib.ForgetPlugin(Self(0));
    CHECK(lib.UsedSets() == PLUGIN_IMAGE_SET_BUILDINGS);
    lib.UseSets(Self(1), 0);
    CHECK(lib.UsedSets() == 0);

    // Bits that are not sets are not kept.
    lib.UseSets(Self(2), 0xF0u | PLUGIN_IMAGE_SET_ITEMS);
    CHECK(lib.UsedSets() == PLUGIN_IMAGE_SET_ITEMS);
}

static void PreloadOnlyTakesMembersOfSetsInUse()
{
    Library lib;
    lib.SetNow(100000);
    const PluginImageId items = lib.PathInSet(kPathA, PLUGIN_IMAGE_SET_ITEMS);
    const PluginImageId buildings = lib.PathInSet(kPathB, PLUGIN_IMAGE_SET_BUILDINGS);
    lib.UseSets(Self(0), PLUGIN_IMAGE_SET_BUILDINGS);

    Request request;
    CHECK(lib.TakePreload(request) && request.id == buildings);
    CHECK(!lib.TakePreload(request));   // the items are not in use

    // The plugin changes its mind: the items now, and the buildings are left.
    lib.Defer(buildings);
    lib.UseSets(Self(0), PLUGIN_IMAGE_SET_ITEMS);
    CHECK(lib.TakePreload(request) && request.id == items);
    CHECK(!lib.TakePreload(request));
}

static void PreloadGoesRoundTheMembersAndDeferGivesOneBack()
{
    Library lib;
    lib.SetNow(100000);
    const PluginImageId a = lib.PathInSet(kPathA, PLUGIN_IMAGE_SET_ITEMS);
    const PluginImageId b = lib.PathInSet(kPathB, PLUGIN_IMAGE_SET_ITEMS);
    const PluginImageId c = lib.PathInSet(kPathC, PLUGIN_IMAGE_SET_ITEMS);
    lib.UseSets(Self(0), PLUGIN_IMAGE_SET_ITEMS);

    // In the order added.
    Request request;
    CHECK(lib.TakePreload(request) && request.id == a);
    // One that cannot be read now goes back to idle, and does not block the rest.
    lib.Defer(a);
    CHECK(lib.State(a) == PLUGIN_IMAGE_NONE);
    CHECK(lib.TakePreload(request) && request.id == b);
    CHECK(lib.TakePreload(request) && request.id == c);
    CHECK(lib.TakePreload(request) && request.id == a);   // round the list
    CHECK(!lib.TakePreload(request));

    // Deferring what is not taken does nothing.
    lib.Defer(a);
    lib.Defer(a);
    lib.Defer(0);
    CHECK(lib.TakePreload(request) && request.id == a);
}

static void ADeferredImageThatWasDrawnGoesToTheQueue()
{
    Library lib;
    lib.SetNow(100000);
    const PluginImageId a = lib.PathInSet(kPathA, PLUGIN_IMAGE_SET_ITEMS);
    lib.UseSets(Self(0), PLUGIN_IMAGE_SET_ITEMS);

    Request request;
    CHECK(lib.TakePreload(request));
    lib.SetNow(100010);
    CHECK(lib.Get(a) == nullptr);   // drawn while it is being read
    lib.Defer(a);
    CHECK(lib.State(a) == PLUGIN_IMAGE_WAITING);
    CHECK(!lib.TakePreload(request));   // it is the on-demand queue's now
    CHECK(lib.TakeNext(request) && request.id == a);
}

static void DrawingAMemberAsksForItAndJumpsTheQueue()
{
    Library lib;
    lib.SetNow(100000);
    const PluginImageId a = lib.PathInSet(kPathA, PLUGIN_IMAGE_SET_ITEMS);
    const PluginImageId b = lib.PathInSet(kPathB, PLUGIN_IMAGE_SET_ITEMS);
    lib.UseSets(Self(0), PLUGIN_IMAGE_SET_ITEMS);

    lib.SetNow(100500);
    CHECK(lib.Get(b) == nullptr);
    Request request;
    CHECK(lib.TakeNext(request) && request.id == b);
    CHECK(!lib.TakeNext(request));
    // The rest is still preload's.
    CHECK(lib.TakePreload(request) && request.id == a);
}

static void SeenWaitingCountsOnlyImagesDrawnLately()
{
    Library lib;
    lib.SetNow(100000);
    const PluginImageId member = lib.PathInSet(kPathA, PLUGIN_IMAGE_SET_ITEMS);
    lib.UseSets(Self(0), PLUGIN_IMAGE_SET_ITEMS);
    CHECK(!lib.SeenWaiting());   // idle

    Request request;
    CHECK(lib.TakePreload(request));
    CHECK(!lib.SeenWaiting());   // being preloaded, never drawn

    lib.SetNow(100100);
    lib.Get(member);
    CHECK(lib.SeenWaiting());
    lib.SetNow(100100 + kSeenLatelyMs + 1);
    CHECK(!lib.SeenWaiting());   // not drawn for a while

    // A request that was asked for and drawn counts until it is answered.
    const PluginImageId drawn = lib.Path(kPathB);
    CHECK(lib.SeenWaiting());
    CHECK(lib.TakeNext(request) && request.id == drawn);
    CHECK(lib.SeenWaiting());   // taken, not answered
    std::vector<Released> evicted;
    CHECK(lib.Publish(drawn, Texture(drawn), 100, evicted));
    CHECK(!lib.SeenWaiting());
}

static void PreloadStopsWhenAnotherImageWouldNotFit()
{
    // Room for two full images (349524 bytes each) and not three.
    Library lib;
    lib.SetCaps(1000000, 100);
    CHECK(lib.PreloadHasRoom());
    std::vector<Released> evicted;
    lib.SetNow(100000);
    const PluginImageId a = lib.Path(kPathA);
    const PluginImageId b = lib.Path(kPathB);
    Request request;
    CHECK(lib.TakeNext(request) && lib.Publish(request.id, Texture(request.id), 349524, evicted));
    CHECK(lib.PreloadHasRoom());
    CHECK(lib.TakeNext(request) && lib.Publish(request.id, Texture(request.id), 349524, evicted));
    CHECK(!lib.PreloadHasRoom());
    CHECK(a != b && evicted.empty());

    // The count cap too.
    Library few;
    few.SetCaps(1000000000, 1);
    few.SetNow(100000);
    few.Path(kPathA);
    CHECK(few.TakeNext(request) && few.Publish(request.id, Texture(request.id), 100, evicted));
    CHECK(!few.PreloadHasRoom());
}

static void PreloadStaysStoppedOnceAnImageHasBeenEvicted()
{
    Request request;
    std::vector<Released> evicted;

    // Two kept under a count cap of two: room is gone while the table is full, and comes back with a higher cap.
    Library roomy;
    roomy.SetCaps(1000000000, 2);
    roomy.SetNow(100000);
    roomy.Path(kPathA);
    roomy.SetNow(100001);
    roomy.Path(kPathB);
    CHECK(roomy.TakeNext(request) && roomy.Publish(request.id, Texture(request.id), 100, evicted));
    CHECK(roomy.TakeNext(request) && roomy.Publish(request.id, Texture(request.id), 100, evicted));
    roomy.SetCaps(1000000000, 100);
    CHECK(roomy.PreloadHasRoom());

    // The same, but an image had to be evicted on the way: preload stays stopped, so it never churns.
    Library full;
    full.SetCaps(1000000000, 2);
    full.SetNow(100000);
    full.Path(kPathA);
    full.SetNow(100001);
    full.Path(kPathB);
    full.SetNow(100002);
    full.Path(kPathC);
    // The most recently asked is read first, so the third read is the oldest and is the one pushed out.
    for (int i = 0; i < 3; ++i)
    {
        CHECK(full.TakeNext(request));
        full.Publish(request.id, Texture(request.id), 100, evicted);
    }
    CHECK(evicted.size() == 1);
    CHECK(full.CountKept() == 2);
    full.SetCaps(1000000000, 100);
    CHECK(!full.PreloadHasRoom());
}

static void PreloadedImagesNobodyDrewAreEvictedFirst()
{
    Library lib;
    lib.SetCaps(1000000, 2);
    lib.SetNow(100000);
    const PluginImageId drawn = lib.Path(kPathA);
    const PluginImageId member = lib.PathInSet(kPathB, PLUGIN_IMAGE_SET_ITEMS);
    lib.UseSets(Self(0), PLUGIN_IMAGE_SET_ITEMS);
    std::vector<Released> evicted;

    Request request;
    CHECK(lib.TakeNext(request) && request.id == drawn);
    CHECK(lib.Publish(drawn, Texture(drawn), 100, evicted));
    CHECK(lib.TakePreload(request) && request.id == member);
    CHECK(lib.Publish(member, Texture(member), 100, evicted));

    lib.SetNow(100100);
    const PluginImageId newer = lib.Path(kPathC);
    CHECK(lib.TakeNext(request) && request.id == newer);
    CHECK(lib.Publish(newer, Texture(newer), 100, evicted));
    CHECK(evicted.size() == 1 && evicted[0].id == member);   // never drawn: first out, though read later
}

static void PreloadProgressCountsTheMembersOfSetsInUse()
{
    Library lib;
    lib.SetNow(100000);
    const PluginImageId a = lib.PathInSet(kPathA, PLUGIN_IMAGE_SET_ITEMS);
    const PluginImageId b = lib.PathInSet(kPathB, PLUGIN_IMAGE_SET_ITEMS);
    lib.PathInSet(kPathC, PLUGIN_IMAGE_SET_BUILDINGS);

    Library::PreloadProgress none = lib.GetPreloadProgress();
    CHECK(none.queued == 0 && none.done == 0);

    lib.UseSets(Self(0), PLUGIN_IMAGE_SET_ITEMS);
    Library::PreloadProgress progress = lib.GetPreloadProgress();
    CHECK(progress.queued == 2 && progress.done == 0);

    std::vector<Released> evicted;
    Request request;
    CHECK(lib.TakePreload(request) && request.id == a);
    progress = lib.GetPreloadProgress();
    CHECK(progress.queued == 2 && progress.done == 0);   // being read
    CHECK(lib.Publish(a, Texture(a), 100, evicted));
    progress = lib.GetPreloadProgress();
    CHECK(progress.queued == 1 && progress.done == 1);

    CHECK(lib.TakePreload(request) && request.id == b);
    lib.PublishNoImage(b);
    progress = lib.GetPreloadProgress();
    CHECK(progress.queued == 0 && progress.done == 2);

    lib.UseSets(Self(0), PLUGIN_IMAGE_SET_ITEMS | PLUGIN_IMAGE_SET_BUILDINGS);
    progress = lib.GetPreloadProgress();
    CHECK(progress.queued == 1 && progress.done == 2);
}

static void AMemberThatFailsIsNotTakenAgain()
{
    Library lib;
    lib.SetNow(100000);
    const PluginImageId a = lib.PathInSet(kPathA, PLUGIN_IMAGE_SET_ITEMS);
    lib.UseSets(Self(0), PLUGIN_IMAGE_SET_ITEMS);
    Request request;
    CHECK(lib.TakePreload(request));
    lib.Fail(a);
    CHECK(!lib.TakePreload(request));
    CHECK(lib.GetPreloadProgress().queued == 0);
    CHECK(lib.GetPreloadProgress().done == 0);
}

static void AnAliasCarriesItsSetsToTheImage()
{
    Library lib;
    lib.SetNow(100000);
    const PluginImageId item = lib.Item("iron_plate");
    lib.AddToSets(item, PLUGIN_IMAGE_SET_ITEMS);
    Request request;
    CHECK(lib.TakeNext(request) && request.id == item);
    const PluginImageId path = lib.Path(kPathA);
    CHECK(lib.TakeNext(request) && request.id == path);
    lib.Requeue(path);
    lib.Requeue(item);
    CHECK(lib.Alias(item, path));
    lib.UseSets(Self(0), PLUGIN_IMAGE_SET_ITEMS);
    // The image is now in the set.
    CHECK(lib.GetPreloadProgress().queued == 1);
}

int main()
{
    RunTest("normalises item names", NormalisesItemNames);
    RunTest("normalises object paths", NormalisesObjectPaths);
    RunTest("ids are stable and never zero", IdsAreStableAndNeverZero);
    RunTest("the request keeps the name as asked for", TheRequestKeepsTheNameAsAskedFor);
    RunTest("ids run out at the table size", IdsRunOutAtTheTableSize);
    RunTest("states follow the read", StatesFollowTheRead);
    RunTest("State does not mark an image seen", StateDoesNotMarkSeen);
    RunTest("seen images are read first", SeenImagesAreReadFirst);
    RunTest("requests nobody drew are dropped", RequestsNobodyDrewAreDropped);
    RunTest("set members are kept and served after seen ones", SetMembersAreKeptAndServedAfterSeenOnes);
    RunTest("set interest is recorded and dropped", SetInterestIsRecordedAndDropped);
    RunTest("the byte cap evicts the least recently seen", ByteCapEvictsLeastRecentlySeen);
    RunTest("the count cap evicts the least recently seen", CountCapEvictsLeastRecentlySeen);
    RunTest("equal stamps evict the lower id first", EqualStampsEvictTheLowerIdFirst);
    RunTest("a copy over the byte cap is refused", ACopyOverTheByteCapIsRefused);
    RunTest("an evicted image is asked for again by Get", AnEvictedImageIsAskedForAgainByGet);
    RunTest("a lower cap evicts on the next publish", ANewCapEvictsOnTheNextPublish);
    RunTest("ReleaseAll hands back every kept copy", ReleaseAllHandsBackEveryKeptCopy);
    RunTest("the byte cap follows the video budget", ByteCapFollowsTheVideoBudget);
    RunTest("aliased ids are one image", AliasedIdsAreOneImage);
    RunTest("alias refusals", AliasRefusals);
    RunTest("the interface reaches the same table", InterfaceReachesTheSameTable);
    RunTest("readers see consistent states while the table changes", ReadersSeeConsistentStatesWhileTheTableChanges);
    RunTest("a member of a set is idle until preload reaches it", AMemberOfASetIsIdleUntilPreloadReachesIt);
    RunTest("used sets are the union and follow the plugins", UsedSetsAreTheUnionAndFollowThePlugins);
    RunTest("preload only takes members of sets in use", PreloadOnlyTakesMembersOfSetsInUse);
    RunTest("preload goes round the members and defer gives one back", PreloadGoesRoundTheMembersAndDeferGivesOneBack);
    RunTest("a deferred image that was drawn goes to the queue", ADeferredImageThatWasDrawnGoesToTheQueue);
    RunTest("drawing a member asks for it and jumps the queue", DrawingAMemberAsksForItAndJumpsTheQueue);
    RunTest("seen waiting counts only images drawn lately", SeenWaitingCountsOnlyImagesDrawnLately);
    RunTest("preload stops when another image would not fit", PreloadStopsWhenAnotherImageWouldNotFit);
    RunTest("preload stays stopped once an image has been evicted", PreloadStaysStoppedOnceAnImageHasBeenEvicted);
    RunTest("preloaded images nobody drew are evicted first", PreloadedImagesNobodyDrewAreEvictedFirst);
    RunTest("preload progress counts the members of sets in use", PreloadProgressCountsTheMembersOfSetsInUse);
    RunTest("a member that fails is not taken again", AMemberThatFailsIsNotTakenAgain);
    RunTest("an alias carries its sets to the image", AnAliasCarriesItsSetsToTheImage);

    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
