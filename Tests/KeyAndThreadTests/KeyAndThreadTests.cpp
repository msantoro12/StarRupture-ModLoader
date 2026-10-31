// IsKeyHeld and IsGameThread test: the two v72 queries a plugin's tick polls.
//
// A console program: no game, no overlay. keybind_registry.cpp and
// game_thread_dispatch.cpp are the loader's own source files; the keyboard and
// the foreground window are test doubles (pch.h, test_doubles.cpp), so the
// tests hold keys and move focus without touching the real desktop.
#include "hooks/input/keybind_registry.h"
#include "utils/game_thread_dispatch.h"
#include "test_doubles.h"

#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>
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

// Every allocation in the process, so a test can see that IsKeyHeld makes none.
static std::atomic<long> g_allocations{ 0 };

void* operator new(size_t size)
{
    ++g_allocations;
    if (void* p = std::malloc(size ? size : 1))
        return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }

using Hooks::Input::IsKeyHeld;
using TestDoubles::SetKeyDown;

// Two windows that are never created; only their identity is compared.
static const HWND kGame  = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x1000));
static const HWND kOther = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x2000));

// The game window exists and has focus, and nothing is held.
static void Reset()
{
    TestDoubles::ReleaseAllKeys();
    TestDoubles::gameWindow = kGame;
    TestDoubles::foreground = kGame;
}

// ---------------------------------------------------------------------------
// IsKeyHeld
// ---------------------------------------------------------------------------

static void SingleKey()
{
    Reset();
    CHECK(!IsKeyHeld("F5"));
    SetKeyDown(VK_F5, true);
    CHECK(IsKeyHeld("F5"));
    CHECK(IsKeyHeld("f5"));                // names are case-insensitive, as the registry's are
    CHECK(!IsKeyHeld("F6"));
    SetKeyDown('K', true);
    CHECK(IsKeyHeld("K"));
    SetKeyDown(VK_F5, false);
    CHECK(!IsKeyHeld("F5"));
    CHECK(IsKeyHeld("K"));
}

static void ModifierChords()
{
    Reset();
    SetKeyDown('K', true);
    CHECK(!IsKeyHeld("Shift+K"));          // K alone is not Shift+K
    SetKeyDown(VK_LSHIFT, true);
    CHECK(IsKeyHeld("Shift+K"));
    CHECK(IsKeyHeld("shift + k"));         // the registry's parser trims and ignores case
    SetKeyDown(VK_LSHIFT, false);
    SetKeyDown(VK_RSHIFT, true);
    CHECK(IsKeyHeld("Shift+K"));           // either side counts, as SampleCurrentModifiers says

    Reset();
    SetKeyDown(VK_F5, true);
    SetKeyDown(VK_LCONTROL, true);
    CHECK(!IsKeyHeld("Ctrl+Shift+F5"));    // every modifier named must be held
    SetKeyDown(VK_RSHIFT, true);
    CHECK(IsKeyHeld("Ctrl+Shift+F5"));
    CHECK(IsKeyHeld("Shift+Control+F5"));  // token order and "Control", as the registry reads them
    SetKeyDown(VK_F5, false);
    CHECK(!IsKeyHeld("Ctrl+Shift+F5"));    // modifiers alone are not the chord

    Reset();
    SetKeyDown(VK_F7, true);
    SetKeyDown(VK_LMENU, true);
    CHECK(IsKeyHeld("Alt+F7"));
}

// DispatchCombo's rule for a named entry: modifiers must match exactly, and a
// name without modifiers ignores them.
static void ExtraModifierHeld()
{
    Reset();
    SetKeyDown('K', true);
    SetKeyDown(VK_LSHIFT, true);
    SetKeyDown(VK_LCONTROL, true);
    CHECK(!IsKeyHeld("Shift+K"));          // Ctrl is held as well: not exactly Shift
    CHECK(IsKeyHeld("Ctrl+Shift+K"));
    CHECK(IsKeyHeld("K"));                 // no modifiers named: held whatever else is
    SetKeyDown(VK_LCONTROL, false);
    CHECK(IsKeyHeld("Shift+K"));
}

static void ModifierAsKey()
{
    Reset();
    CHECK(!IsKeyHeld("LeftShift"));
    SetKeyDown(VK_LSHIFT, true);
    CHECK(IsKeyHeld("LeftShift"));
    CHECK(!IsKeyHeld("RightShift"));
    CHECK(!IsKeyHeld("Shift"));            // a modifier token alone names no key
}

static void MouseButtons()
{
    Reset();
    CHECK(!IsKeyHeld("LeftMouseButton"));
    SetKeyDown(VK_LBUTTON, true);
    CHECK(IsKeyHeld("LeftMouseButton"));
    CHECK(!IsKeyHeld("RightMouseButton"));
    SetKeyDown(VK_XBUTTON2, true);
    CHECK(IsKeyHeld("ThumbMouseButton2"));
    CHECK(!IsKeyHeld("ThumbMouseButton"));
    SetKeyDown(VK_LCONTROL, true);
    CHECK(IsKeyHeld("Ctrl+LeftMouseButton"));
}

// The key below Escape, whose VK depends on the keyboard layout.
static void TildeFollowsLayout()
{
    Reset();
    const int vk = Hooks::Input::ModKeyToVK(EModKey::Tilde);
    CHECK(vk != 0);
    SetKeyDown(vk, true);
    CHECK(IsKeyHeld("Tilde"));
}

static void UnknownNames()
{
    Reset();
    for (int vk = 1; vk < 256; ++vk)
        SetKeyDown(vk, true);              // everything is down: only the name can say no
    CHECK(!IsKeyHeld(nullptr));
    CHECK(!IsKeyHeld(""));
    CHECK(!IsKeyHeld("NotAKey"));
    CHECK(!IsKeyHeld("Ctrl+NotAKey"));
    CHECK(!IsKeyHeld("+"));
    CHECK(!IsKeyHeld("Ctrl+Shift"));
}

static void NoFocus()
{
    Reset();
    SetKeyDown(VK_F5, true);
    SetKeyDown('K', true);
    SetKeyDown(VK_LSHIFT, true);
    TestDoubles::foreground = kOther;      // the player alt-tabbed out
    CHECK(!IsKeyHeld("F5"));
    CHECK(!IsKeyHeld("Shift+K"));
    TestDoubles::foreground = nullptr;     // nothing has focus (a desktop switch)
    CHECK(!IsKeyHeld("F5"));
    TestDoubles::foreground = kGame;
    CHECK(IsKeyHeld("F5"));
}

static void NullWindow()
{
    Reset();
    SetKeyDown(VK_F5, true);
    TestDoubles::gameWindow = nullptr;     // before the overlay's first Present
    CHECK(!IsKeyHeld("F5"));
    TestDoubles::foreground = nullptr;     // null must not match a null foreground
    CHECK(!IsKeyHeld("F5"));
}

static void AllocatesNothing()
{
    Reset();
    SetKeyDown('K', true);
    SetKeyDown(VK_LSHIFT, true);
    const long before = g_allocations.load();
    bool any = false;
    for (int i = 0; i < 1000; ++i)
        any |= IsKeyHeld("Shift+K") | IsKeyHeld("Ctrl+Alt+F12") | IsKeyHeld("NotAKey");
    CHECK(any);
    CHECK(g_allocations.load() == before);
}

// Called from several threads at once while another moves the focus: every
// answer is one a single-threaded call could give, and nothing faults.
static void AnyThread()
{
    Reset();
    SetKeyDown('K', true);
    SetKeyDown(VK_LSHIFT, true);
    std::atomic<bool> stop{ false };
    std::atomic<long> held{ 0 }, notHeld{ 0 }, wrong{ 0 };
    std::thread focus([&] {
        for (int i = 0; i < 20000; ++i)
            TestDoubles::foreground = (i & 1) ? kOther : kGame;
        TestDoubles::foreground = kGame;
        stop = true;
    });
    std::thread readers[4];
    for (auto& t : readers)
        t = std::thread([&] {
            while (!stop.load())
            {
                if (IsKeyHeld("Shift+K")) ++held; else ++notHeld;
                if (IsKeyHeld("Ctrl+K")) ++wrong;      // Ctrl is never held
            }
        });
    focus.join();
    for (auto& t : readers)
        t.join();
    CHECK(wrong.load() == 0);
    CHECK(held.load() + notHeld.load() > 0);
    CHECK(IsKeyHeld("Shift+K"));                       // settled back on the game window
}

// ---------------------------------------------------------------------------
// IsGameThread (GameThreadDispatch: the thread that calls Drain each tick)
// ---------------------------------------------------------------------------

// Must run first: it checks the state before any tick.
static void GameThreadBeforeFirstTick()
{
    CHECK(!GameThreadDispatch::HasTicked());
    CHECK(!GameThreadDispatch::IsGameThread());       // the thread that will tick included
    bool other = true;
    std::thread([&] { other = GameThreadDispatch::IsGameThread(); }).join();
    CHECK(!other);
}

static void GameThreadAfterTick()
{
    // A thread that stands in for the game thread: it ticks (Drain) and stays
    // alive, so its id cannot be reused by another thread mid-test.
    std::atomic<int> phase{ 0 };
    bool onTickThread = false, inTask = false;
    std::thread game([&] {
        GameThreadDispatch::PostVoid([&] { inTask = GameThreadDispatch::IsGameThread(); });
        GameThreadDispatch::Drain();
        onTickThread = GameThreadDispatch::IsGameThread();
        phase = 1;
        while (phase.load() != 2)
            Sleep(1);
    });
    while (phase.load() != 1)
        Sleep(1);

    CHECK(GameThreadDispatch::HasTicked());
    CHECK(onTickThread);
    CHECK(inTask);                                     // a posted task runs on the game thread
    CHECK(!GameThreadDispatch::IsGameThread());       // this thread did not tick
    bool worker = true;
    std::thread([&] { worker = GameThreadDispatch::IsGameThread(); }).join();
    CHECK(!worker);

    phase = 2;
    game.join();

    // Whichever thread ticks last is the game thread.
    GameThreadDispatch::Drain();
    CHECK(GameThreadDispatch::IsGameThread());
}

int main()
{
    RunTest("IsGameThread: false on every thread before the first tick", GameThreadBeforeFirstTick);
    RunTest("IsGameThread: true only on the thread that ticks", GameThreadAfterTick);

    RunTest("IsKeyHeld: single keys", SingleKey);
    RunTest("IsKeyHeld: modifier chords", ModifierChords);
    RunTest("IsKeyHeld: Shift+K with Ctrl also held", ExtraModifierHeld);
    RunTest("IsKeyHeld: a modifier bound as a plain key", ModifierAsKey);
    RunTest("IsKeyHeld: mouse buttons", MouseButtons);
    RunTest("IsKeyHeld: Tilde follows the keyboard layout", TildeFollowsLayout);
    RunTest("IsKeyHeld: unknown names", UnknownNames);
    RunTest("IsKeyHeld: no focus", NoFocus);
    RunTest("IsKeyHeld: no game window yet", NullWindow);
    RunTest("IsKeyHeld: allocates nothing", AllocatesNothing);
    RunTest("IsKeyHeld: any thread", AnyThread);

    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
