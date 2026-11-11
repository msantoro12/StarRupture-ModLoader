#pragma once

#ifdef MODLOADER_CLIENT_BUILD

// Test build: what the game's item and building images are, and what reading them costs.
// Run 1 is the main menu alone; run 2 is the main menu, then a loaded world. Log tag [ImgTrial].
namespace ImageTrial
{
    // Main menu reached: the menu part is due in 20 s.
    void OnMenu();

    // A world other than the menu began: a menu part still waiting or running is dropped.
    void OnGameWorld();

    // Experience load complete: the world part is due in 10 s if a world is running.
    void OnWorldLoaded();

    // Game thread, every tick.
    void Tick();
}

#endif
