#pragma once

#ifdef MODLOADER_CLIENT_BUILD

// ---------------------------------------------------------------------------
// Native settings spike (throwaway, P0)
//
// Answers four questions before the native settings page is designed for
// real, by doing each thing once and logging what happened:
//
//   S1  Where the game's Custom Game page class and its option row classes
//       live, which widget stack holds the menu, the layer names, and whether
//       the world pauses while the menu is open.
//   S2  Whether the stock option rows (toggle, slider, rotator) can be built
//       and driven outside the Custom Game page: labels, headings, the
//       description pane, the handler members, and whether anything reaches
//       the pending Custom Game settings.
//   S3  Back, Escape, gamepad B, focus, closing, and a plugin reload while the
//       page is open.
//   S4  Whether the game's "press any key" panel can be pushed on its own and
//       the key it captured read back.
//
// Nothing here is wired to a settings store. Rows only log. Every row is
// written with Option = None, and only the plain fields of its OptionData.
// The page opens at the main menu only.
//
// Threading: every UObject touch is on the game thread (the engine tick, or a
// handler detour, which the game calls from Slate input on the game thread).
// The MODS click and a plugin unload only set atomics.
// ---------------------------------------------------------------------------

namespace NativeSettingsSpike
{
	// Resolves the four option-row handlers and hooks them, and starts watching
	// for hover events. Runs with the other plugin event hooks, after GObjects
	// is populated.
	bool Install();
	void Remove();
	bool IsInstalled();

	// MODS row click. Game thread, inside the menu's click handler: sets a flag.
	void RequestOpen();

	// Every engine tick, game thread.
	void Tick(float deltaSeconds);

	// PluginManager calls this before FreeLibrary, next to the other Forget
	// calls. Any thread; it only records the event for the next tick.
	void ForgetPlugin(const char* pluginName);
}

#endif // MODLOADER_CLIENT_BUILD
