// Image shrink test: the size rules of UI/image_shrink.h. Which size an image is
// stored at, how many levels the chain has, and how many bytes it takes.
//
// A console program: no game, no GPU.
#include "UI/image_shrink.h"
#include "UI/image_library.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

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

using namespace ImageShrink;

static bool Is(Size size, uint32_t width, uint32_t height)
{
    return size.width == width && size.height == height;
}

static void TheStoredSizeConstantIs256()
{
    CHECK(ImageLibrary::kStoredSize == 256);
}

static void ALargerImageIsScaledToTheStoredSize()
{
    CHECK(Is(StoredSize(512, 512, 256), 256, 256));
    CHECK(Is(StoredSize(1024, 1024, 256), 256, 256));
    CHECK(Is(StoredSize(257, 257, 256), 256, 256));
}

static void TheProportionsAreKept()
{
    CHECK(Is(StoredSize(1024, 512, 256), 256, 128));
    CHECK(Is(StoredSize(512, 1024, 256), 128, 256));
    CHECK(Is(StoredSize(1024, 256, 256), 256, 64));
    // 841 x 1188 (a real texture in the game): 841 * 256 / 1188 = 181.2
    CHECK(Is(StoredSize(841, 1188, 256), 181, 256));
    // 1339 x 746: 746 * 256 / 1339 = 142.6
    CHECK(Is(StoredSize(1339, 746, 256), 256, 143));
    CHECK(Is(StoredSize(300, 200, 256), 256, 171));
}

static void ASmallerImageKeepsItsOwnSize()
{
    CHECK(Is(StoredSize(256, 256, 256), 256, 256));
    CHECK(Is(StoredSize(128, 128, 256), 128, 128));
    CHECK(Is(StoredSize(64, 64, 256), 64, 64));
    CHECK(Is(StoredSize(32, 32, 256), 32, 32));
    CHECK(Is(StoredSize(200, 100, 256), 200, 100));
    CHECK(Is(StoredSize(256, 17, 256), 256, 17));
}

static void ASideNeverShrinksToNothing()
{
    CHECK(Is(StoredSize(4096, 4, 256), 256, 1));
    CHECK(Is(StoredSize(10000, 1, 256), 256, 1));
}

static void AnEmptySizeStaysEmpty()
{
    CHECK(Is(StoredSize(0, 0, 256), 0, 0));
    CHECK(Is(StoredSize(0, 512, 256), 0, 512));
}

static void TheStoredSizeFollowsMaxSize()
{
    CHECK(Is(StoredSize(512, 512, 128), 128, 128));
    CHECK(Is(StoredSize(256, 256, 128), 128, 128));
    CHECK(Is(StoredSize(100, 100, 128), 100, 100));
}

static void TheChainRunsDownToOnePixel()
{
    CHECK(LevelCount(256, 256) == 9);
    CHECK(LevelCount(256, 128) == 9);
    CHECK(LevelCount(128, 128) == 8);
    CHECK(LevelCount(64, 64) == 7);
    CHECK(LevelCount(32, 32) == 6);
    CHECK(LevelCount(1, 1) == 1);
    CHECK(LevelCount(2, 1) == 2);
    CHECK(LevelCount(181, 256) == 9);
    CHECK(LevelCount(256, 1) == 9);
    CHECK(LevelCount(0, 0) == 0);
}

static void ALevelNeverShrinksBelowOnePixel()
{
    CHECK(Is(LevelSize({ 256, 128 }, 0), 256, 128));
    CHECK(Is(LevelSize({ 256, 128 }, 1), 128, 64));
    CHECK(Is(LevelSize({ 256, 128 }, 7), 2, 1));
    CHECK(Is(LevelSize({ 256, 128 }, 8), 1, 1));
}

static void BytesCountEveryLevelAtFourBytesAPixel()
{
    CHECK(Bytes({ 256, 256 }, 1) == 262144);
    CHECK(Bytes({ 256, 256 }, 9) == 349524);
    CHECK(Bytes({ 256, 128 }, 9) == 174764);
    CHECK(Bytes({ 128, 128 }, 8) == 87380);
    CHECK(Bytes({ 32, 32 }, 6) == 5460);
    CHECK(Bytes({ 1, 1 }, 1) == 4);
    CHECK(Bytes({ 256, 256 }, 0) == 0);
}

static void TheWholeChainCostsAboutAThirdMoreThanTopLevel()
{
    // The chain of a square image is 4/3 of its top level, give or take the rounding of the small ones.
    const uint64_t top = Bytes({ 256, 256 }, 1);
    const uint64_t all = Bytes({ 256, 256 }, LevelCount(256, 256));
    CHECK(all > top && all < top * 4 / 3 + 16);
}

static void TheItemSetFitsTheByteCap()
{
    // 412 items at 256 x 256 with their chains: the plan's memory figure for the default cap.
    const uint64_t perImage = Bytes({ 256, 256 }, LevelCount(256, 256));
    CHECK(412 * perImage < ImageLibrary::kByteCapMax);
}

// A shrink asked for before the set-up has ended is Busy; it is Ready after.
static void AShrinkBeforeTheSetupEndsIsBusyAndReadyAfter()
{
    Setup setup;
    uint32_t generation = 0;
    CHECK(setup.Get() == Setup::Answer::Busy);
    CHECK(setup.Begin(generation));
    CHECK(!setup.Begin(generation));   // one thread only
    CHECK(setup.Get() == Setup::Answer::Busy);
    CHECK(setup.Get() == Setup::Answer::Busy);

    CHECK(setup.End(generation, true));
    CHECK(setup.Get() == Setup::Answer::Ready);
    CHECK(!setup.Begin(generation));
    CHECK(!setup.End(generation, true));   // the outcome is taken once
}

// A set-up that fails is Failed for good, and its outcome is taken once, so it is logged once.
static void AFailedSetupIsUnavailableOnce()
{
    Setup setup;
    uint32_t generation = 0;
    CHECK(setup.Begin(generation));
    CHECK(setup.End(generation, false));
    CHECK(!setup.End(generation, false));
    for (int i = 0; i < 5; ++i)
        CHECK(setup.Get() == Setup::Answer::Failed);
    CHECK(!setup.Begin(generation));

    // A new device does not clear it: the same compile would fail again.
    setup.Reset();
    CHECK(setup.Get() == Setup::Answer::Failed);
    CHECK(!setup.Begin(generation));
}

// A device that goes away or a shutdown while the set-up runs: Reset returns at once, the late End
// is dropped, and a set-up for the next device can start.
static void AResetDuringTheSetupDoesNotWaitAndDropsTheLateEnd()
{
    Setup setup;
    uint32_t generation = 0;
    CHECK(setup.Begin(generation));

    std::atomic<bool> release{ false };
    std::atomic<int> ended{ -1 };
    std::thread worker([&]
    {
        while (!release.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ended = setup.End(generation, true) ? 1 : 0;
    });

    const auto start = std::chrono::steady_clock::now();
    setup.Reset();                                   // the worker is still running
    const auto waited = std::chrono::steady_clock::now() - start;
    CHECK(waited < std::chrono::milliseconds(500));
    CHECK(setup.Get() == Setup::Answer::Busy);

    uint32_t next = 0;
    CHECK(setup.Begin(next));                        // the new device's own set-up
    CHECK(next != generation);

    release = true;
    worker.join();
    CHECK(ended.load() == 0);                        // the old one is dropped
    CHECK(setup.Get() == Setup::Answer::Busy);       // and did not touch the new one
    CHECK(setup.End(next, true));
    CHECK(setup.Get() == Setup::Answer::Ready);

    // Ready is cleared by a reset too: the objects went with the device.
    setup.Reset();
    CHECK(setup.Get() == Setup::Answer::Busy);
    CHECK(setup.Begin(next));
}

int main()
{
    RunTest("the stored size constant is 256", TheStoredSizeConstantIs256);
    RunTest("a larger image is scaled to the stored size", ALargerImageIsScaledToTheStoredSize);
    RunTest("the proportions are kept", TheProportionsAreKept);
    RunTest("a smaller image keeps its own size", ASmallerImageKeepsItsOwnSize);
    RunTest("a side never shrinks to nothing", ASideNeverShrinksToNothing);
    RunTest("an empty size stays empty", AnEmptySizeStaysEmpty);
    RunTest("the stored size follows max size", TheStoredSizeFollowsMaxSize);
    RunTest("the chain runs down to one pixel", TheChainRunsDownToOnePixel);
    RunTest("a level never shrinks below one pixel", ALevelNeverShrinksBelowOnePixel);
    RunTest("bytes count every level at four bytes a pixel", BytesCountEveryLevelAtFourBytesAPixel);
    RunTest("the whole chain costs about a third more than the top level", TheWholeChainCostsAboutAThirdMoreThanTopLevel);
    RunTest("the item set fits the byte cap", TheItemSetFitsTheByteCap);
    RunTest("a shrink before the set-up ends is busy and ready after", AShrinkBeforeTheSetupEndsIsBusyAndReadyAfter);
    RunTest("a failed set-up is unavailable once", AFailedSetupIsUnavailableOnce);
    RunTest("a reset during the set-up does not wait and drops the late end", AResetDuringTheSetupDoesNotWaitAndDropsTheLateEnd);

    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
