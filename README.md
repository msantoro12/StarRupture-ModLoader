# StarRupture ModLoader
[![Build and Release](https://github.com/AlienXAXS/StarRupture-ModLoader/actions/workflows/release.yml/badge.svg)](https://github.com/AlienXAXS/StarRupture-ModLoader/actions/workflows/release.yml)

A plugin-based mod loader for [StarRupture](https://store.steampowered.com/app/1631270/StarRupture/). Loads automatically via DLL proxy injection — no game files are modified.

Like this project? Give it a star here on GitHub!

---

## Discord

[Join our Discord server](https://discord.gg/G5a4GYsvrz) to chat about the mod loader and plugins.

---

## Installation

### 1. Download the right ZIP

Two builds are published on the [releases page](https://github.com/AlienXAXS/StarRupture-ModLoader/releases):

| ZIP | Use for |
|---|---|
| `StarRupture-ModLoader-Client-*.zip` | Playing the game |
| `StarRupture-ModLoader-Server-*.zip` | Running a dedicated server |

### 2. Extract into the game's binary folder

That's the folder containing `StarRuptureGameSteam-Win64-Shipping.exe`, for example:

```
StarRupture\Binaries\Win64\
```

The ZIP is already laid out correctly, so extract it **directly into that folder** — no subfolders
need creating by hand. Afterwards you should see:

```
StarRupture\Binaries\Win64\
├── StarRuptureGameSteam-Win64-Shipping.exe
├── dwmapi.dll                        ← the proxy the game loads
└── ModLoader\
    ├── StarRupture-ModLoader-Core.dll   ← the mod loader itself
    ├── StarRupture-ImGui.dll            ← client builds only
    ├── modloader.ini                    ← created on first launch
    ├── Logs\                            ← created on first launch
    ├── Plugins\                         ← put plugin DLLs here
    └── Preload\                         ← put preload plugin DLLs here
```

Everything the mod loader owns lives under `ModLoader\`; `dwmapi.dll` is the only file that sits
next to the game executable.

### 3. Launch as normal

The mod loader initialises automatically via `dwmapi.dll`. No game files are modified.

**Linux users:** the mod will not load unless the environment variable `WINEDLLOVERRIDES` is set to
`dwmapi=n,b`.

---

## Installing Plugins

The mod loader ships with no plugins — download the ones you want and drop their `.dll` into
`Binaries\Win64\ModLoader\Plugins\`. Client plugins only load on a client, server plugins only on
a dedicated server; the mod loader window says so if you get one the wrong way round.

**Plugins are disabled by default.** The first time the game runs with a plugin present, a config
file is generated for it. To enable it:

1. Run the game once, then close it.
2. Open `Binaries\Win64\ModLoader\Plugins\config\`.
3. Open the `.ini` named after the plugin.
4. Set `Enabled=1` and save.
5. Launch again — the plugin is now active.

Repeat for each plugin you want to use. On a client you can do all of this from the mod loader
window instead, without editing files.

If a plugin ships a `.json` file alongside its `.dll`, it can update itself: the mod loader checks
for a new version at startup and downloads it. The **Plugins** tab marks those with a green dot
(amber means the plugin has no update manifest and has to be updated by hand). Auto-updating can be
turned off in the **Settings** tab.

---

## The Mod Loader Window (client)

Press **F2** in game to open it, or pick **MOD LOADER** in the game's main menu or pause menu, between
OPTIONS and CREDITS. **Esc** closes it. Down the left is an icon for each screen:

| Screen | What it's for |
|---|---|
| **Plugins** | Load, unload and reload installed plugins without restarting |
| **Config** | Edit each plugin's own settings and keybinds — see below |
| **Settings** | HUD overlays (FPS, world name, position), DLSS frame generation, network timeout, auto-updates, developer console, diagnostics |
| **Logging** | Log levels for the mod loader, the game, and each plugin — see below |
| **Theme** | Font, text size and every UI colour, saved as named themes |
| **About** | Build tag and the plugins currently loaded |

A developer console is on **`~`** (tilde). Both keys can be changed in
`ModLoader\modloader.ini` — `[UI] OpenKey` and `[Console] OpenKey`. The **MOD LOADER** menu row can be
removed with `[UI] GameMenuEntry=0` in the same file.

### Plugins tab

**UNLOAD**, **LOAD** and **RELOAD** run on the game thread. Unloading a plugin also removes the panels,
widgets and config callbacks it registered, even if it did not unregister them itself, so a RELOAD
cannot leave the overlay calling into code that is gone.

If the overlay is still drawing one of the plugin's panels a second after the request, the unload is
skipped rather than risk a crash: the log says so, the plugin is shut down but stays loaded, and
**RELOAD** tries again.

### Config tab

Pick a plugin in the list on the left (drag the divider to resize it) and its settings appear on the
right, grouped by their `.ini` section. Each row has the setting's name, its description in grey, and a
control:

- **On/off settings** are toggles.
- **Settings with a range** are sliders. The **-** and **+** buttons beside a slider step by 1% of the
  range, repeating while held; hold **Ctrl** for 10%. **Ctrl+Click** a slider to type an exact value.
- **Other numbers and text** are fields, applied on **Enter** or when you click away.
- **Keybinds** show the current key with **Rebind** and **Block**. **Rebind** waits for the next key:
  hold Ctrl, Shift or Alt for a combo, press a modifier on its own to bind just that, or **Esc** to
  cancel. **Block** stops the game also reacting to the combo.
- The **reset** arrow puts a setting back to its default. It is not shown on `Enabled`.

The plugin hears about a change as you make it, through its config-changed callback; the `.ini` is
written when you let go of a slider or confirm a field, not on every frame. If a plugin writes one of
its own settings while the page is open, the new value shows up straight away. Reloading a plugin picks
up settings its new build added without reselecting it.

### Settings tab

- **HUD Overlays** — FPS, current world name and player position in the bottom corner, and
  *ModLoader Debug Values* (see [Troubleshooting](#troubleshooting)).
- **Rendering** — *Pause Frame Generation While Windows Are Open*, on by default. DLSS Frame
  Generation's interpolated frames never pass through the overlay, so with it running every other
  frame has no overlay on it, which looks like flicker. This turns it off while the mod loader window,
  the console or any plugin panel is open and restores it once they are all closed. Saved as
  `PauseFrameGenWhileOpen` under `[Rendering]`.
- **Networking** — the connection timeout, see [Net timeouts](#net-timeouts).
- **Auto Update** — the mod loader and plugin update check at startup.
- **Developer** and **Diagnostics** — open the developer console, and a live tick profiler.

### Theme tab

Switching theme applies at once. **Default** and **Star Rupture** are built in and read-only; **Save As**
stores your colours as `ModLoader\Themes\<name>.ini`, and **Save**, **Delete** and **Reload** work on
your own themes. Share the file to share the theme.

---

## The Server Console (dedicated server)

A dedicated server has no UI, so launch it with `-console` for a console window with the same
commands. `help` lists them; `plugins`, `reload`, `load`, `unload` and `rescan` manage plugins
without restarting the server, and `stop` shuts it down cleanly.

---

## Log Levels

The mod loader writes to `Binaries\Win64\ModLoader\Logs\ModLoader.log`, keeping the last ten runs.
Open the mod loader window and go to the **Logging** tab to change how much detail it records:

- **Log Level** — the mod loader's own output. Saved, so it applies on every launch.
- **Game Log Verbosity** — the game's own log categories, written to `StarRupture.log`. Also saved.
- **Per plugin** — a grid with one row per plugin. Turn a single plugin up to read its output
  without the rest of the log burying it, or down to silence a noisy one without quieting
  everything else. `Default` means "follow the Log Level above".

Per-plugin levels are **not saved** — every plugin is back on `Default` next launch.

### Setting a plugin's level before the game starts

A plugin that logs heavily while the game is still loading has already filled the log by the time
you can open the Logging tab. Add a launch option to have the level in place before it loads.
In Steam: right-click the game, *Properties* → *General* → *Launch Options*.

```
-PluginLogLevel=<plugin>:<level>
```

`<plugin>` is the name as it appears in `[Plugin:NAME]` in the log — the same name the Logging tab
shows. `<level>` is `trace`, `debug`, `info`, `warn`, `error`, or `default`.

Several at once, comma separated:

```
-PluginLogLevel=NoisyPlugin:error,OtherPlugin:trace
```

`*` sets the level for every plugin you have not named:

```
-PluginLogLevel=*:warn,PluginImDebugging:trace
```

Quote the whole option if a plugin name contains a space:

```
-PluginLogLevel="My Plugin:error"
```

The Logging tab shows when a launch option is in effect, and *Reset All To Default* clears it for
the rest of the session.

---

## Developing Your Own Plugins

Use the [StarRupture-Plugin-SDK](https://github.com/AlienXAXS/StarRupture-Plugin-SDK) — it has everything you need (headers, UE5 SDK, an example plugin, and a pre-built `dwmapi.dll`) without requiring a fork of this repo.

### Unloading and RELOAD

The mod loader removes a plugin's panels, widgets and config-changed callbacks when it unloads, so a
plugin that fails part-way through `PluginShutdown` cannot leave the overlay pointing into freed
memory. Still unregister them in `PluginShutdown`, and still check the thread before touching game
objects there: shutdown runs on the game thread from the overlay's RELOAD button, and also at process
exit.

### Preload plugins

A **preload plugin** is a DLL in `ModLoader\Preload\` that runs *before the game executable's
entry point*, so it can patch code the engine touches on its way up. There is no engine at that
point — no `GMalloc`, no `UObject`, no config — so a preload plugin resolves
patterns, patches bytes, installs detours, and returns. Its detours fire later, once the game is
running.

One failing can never stop the game launching: it is unloaded, reported, and the game boots
without it. That is deliberately the opposite of the loader's own pattern preflight, and it is
what makes an outdated preload plugin safe to leave installed across a game update.

- SDK and starter project: [StarRupture-PreLoadPlugin-SDK](https://github.com/AlienXAXS/StarRupture-PreLoadPlugin-SDK)
- Design and rules: [Preload.md](Preload.md)

If you only need engine events, UI, config or networking, use an ordinary plugin — you get
the whole API and you can hot-reload it while the game runs.

---

## Troubleshooting

| Problem | Solution |
|---|---|
| Nothing loads at all | `dwmapi.dll` must sit next to the game `.exe` with the `ModLoader\` folder beside it — not in a subfolder of its own. On Linux, set `WINEDLLOVERRIDES=dwmapi=n,b`. |
| Plugins not loading | Make sure the DLLs are in `ModLoader\Plugins\` and `Enabled=1` is set in each plugin's `.ini`. The **Plugins** tab shows why a plugin was rejected. |
| Preload plugin not running | Run `preload` in the console, or check the `[Preload]` lines in the log. A preload plugin that fails is skipped and the game starts without it. |
| Game will not start after adding a preload plugin | Launch with `-NoPreload`, or set `Enabled=0` under `[Preload]` in `modloader.ini`. The loader also skips any preload plugin that did not finish loading last launch, and says which it was. |
| "Cannot Load" next to a plugin | It was built for the other target — a server plugin on a client, or the reverse. |
| "Needs Update" next to a plugin | The plugin and the mod loader were built against different plugin interface versions. The tab says which of the two to update. |
| Logs / diagnostics | Check `ModLoader\Logs\ModLoader.log`. Raise the detail in the **Logging** tab, or set `Level=DEBUG` under `[Logging]` in `ModLoader\modloader.ini`. |
| One plugin is flooding the log | See [Log Levels](#log-levels) — turn that plugin down on its own, or use `-PluginLogLevel=` if it happens during startup. |
| The *game's* log is flooding on a dedicated server | Hundreds of `LogTemp` / `LogSkinnedMeshComp` warnings a second come from the game itself, not the mod loader. Server builds turn those two categories down to errors automatically; edit `GameLogCategories` under `[Logging]` in `ModLoader\modloader.ini` to change the list, or blank it to leave the game log alone. `-LogCmds=` has no effect on this build. |
| A plugin will not unload from the **Plugins** tab | The log says "the overlay is still drawing its panel". The plugin has been shut down but stays loaded; press **RELOAD** to try again. |
| Game stops responding to input | Turn on *ModLoader Debug Values* in the **Settings** tab — it names the plugin holding input open. |
| Players dropped during a long map load | The mod loader raises the engine's net timeouts to 300s by default — see [Net timeouts](#net-timeouts). Both ends of the session need the mod loader for this to help. |

---

## Net timeouts

A player can be dropped while the host loads a map. The host's game thread does not
tick during the load, so nothing is sent — not even keepalives — and the client sees
silence for the whole load. Against the engine's stock 60s connection timeout, a load
longer than that disconnects everyone still joining.

The mod loader raises both net timeouts to **300 seconds** by default. The connection
timeout has a slider in the **Settings** tab under *Networking*, which also shows what
the live net driver is actually using. Both are in `ModLoader\modloader.ini`:

```ini
[Network]
ConnectionTimeout=300.0
InitialConnectTimeout=300.0
```

Set a value to `0` to leave the engine's own default alone. Accepted range is 1–3600
seconds; anything outside it is ignored with a warning in the log.

Two things to know:

- **Both ends need it.** Each side times the other out against its own clock, so the
  side that gives up is the side that needs the higher value. A vanilla client cannot
  be helped by a modded host, and vice versa.
- **It is not free.** A genuinely dead connection — a crashed client, a pulled network
  cable — now holds its player slot for the full timeout instead of a minute.

This cannot be set from Steam launch options. The engine does read
`?ConnectionTimeout=` out of the URL it connects with, but there is no way to get an
option into that URL from outside the process in a shipping build.

---

## Author

**AlienX** — [GitHub](https://github.com/AlienXAXS) · [AGNGaming](https://www.agngaming.com)

## Credits

- **[Dumper-7](https://github.com/Encryqed/Dumper-7)** — Unreal Engine SDK generation
- **[MinHook](https://github.com/TsudaKageyu/minhook)** — Function hooking library
- **[nlohmann/json](https://github.com/nlohmann/json)** — JSON parsing

Additional features by @msantoro12, built with Claude Code.

---

## Tools Used

- **[IDA Pro](https://hex-rays.com/)** by Hex-Rays — reverse engineering the game binary to find hook targets and AOB patterns
- **[Claude Code](https://claude.com/claude-code)** — AI coding assistant used throughout development

---

## Disclaimer

This is a modding tool for educational purposes. Use at your own risk. The authors are not responsible for any damage caused by using this software. While every effort is made to ensure save file compatibility across updates, this cannot be guaranteed.

---

**Game:** Star Rupture · **Engine:** Unreal Engine 5
