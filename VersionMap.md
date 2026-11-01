# Version map

How the mod loader finds game functions and game-class members across game builds, how a new
build reaches players without a loader release, and what each failure looks like. For plugin
authors the request side is in the
[SDK guide](https://github.com/AlienXAXS/StarRupture-Plugin-SDK/blob/main/PluginDevelopment.md);
for players the short version is the README's [Game updates](README.md#game-updates).

A game update moves functions and fields. The version map is the one place that says where they
are on each build, so a new build means new rows in the map and nothing else: no loader release
and no plugin release, as long as the plugins take their game data from the map.

## What the map holds

The map has one row per key and game build (a build is its CL, plus the exe's PE time stamp and
image size). There are four kinds of row.

| Row | Key | Holds |
|---|---|---|
| function | `Class::Function` or a global's name (`GWorld`) | A pattern, `resultOffset`, `followRel32At` and `followRipDispAt` as in a `PluginScanRequest`, the `rva` CI verified, and the `kind` it was verified with |
| function (site) | `<owner>/<hookName>` | The same, plus `fp` |
| data | a global's name | A function row of kind `DATA` |
| field | `Class::Member` | `offset`, `size`, a bool mask, and the member's new name when it was renamed |
| absent | any function key | No locator: CI could not place the key on that build |

**Key.** A key names a function, a global or a member in Dumper-7 naming. A global key (kinds
`FUNCTION_START` and `DATA`) names one game symbol, the same for every plugin, and has no `/`. A
site key (kinds `IN_FUNCTION`, `CODE`, `VTABLE`, `ANY`) names no symbol. It is
`<owner>/<hookName>`, where the owner is the plugin's `PluginInfo` name or `ModLoader` for the
loader's own. Only that owner reads it.

**Kind.** A row is used only for a request of the same kind. Otherwise it is skipped with a log
line, so the map can never weaken a kind check.

**fp.** A site's address is right only for the request it was verified with, and old plugin
builds stay in the field after a newer one re-anchors a site under the same name. So a site row
carries `fp`, an FNV-1a 32 fingerprint of the request: its kind, its pattern as the loader parses
it, `resultOffset`, `followRel32At`, `followRipDispAt` and, for the loader's own entries, a site
arithmetic revision. The loader computes `fp` for the incoming request the same way and uses the
row only when the two are equal. A site row with no `fp` matches nothing, and in a feed it
rejects the whole file. Global and field rows carry none.

**Absent.** An `absent` row records that CI tried and could not place the key on that build. For
a global key it lists the owners whose requests CI tested; for a site key it carries the `fp` it
tested. It binds only those requests: they fail with no fallback, because their locators were
found unsafe on that build. A verified row replaces an `absent` row; nothing else does.

The map lives in `hooks/game/version_map.generated.h`, written by CI
(`ci/version-map.mjs` in [starrupture-ci](https://github.com/msantoro12/starrupture-ci), format in
its `ci/VERSION-MAP.md`). Every loader build carries the map as it stood when it was built, and
works offline on every build in it. The signed feed (below) adds to that.

## Selection

A plugin asks for a function with `PLUGIN_SCAN_FLAG_MAP_KEY` and for a field with a field
request. The order for a function key is:

1. **The loader's row for the running CL.** On the row's own exe (CL, stamp and size all equal)
   the result must equal the row's `rva`; a mismatch is a failure with no fallback. On another
   exe of the same CL the row's pattern must match. A site row must also have the request's `fp`,
   or it is skipped.
2. **The plugin's own row for the running CL** (`candidates`, or its pattern on its built CL).
   When it is for the running exe, its `rva` must equal the loader's pick, or the result is
   AMBIGUOUS.
3. **Agreement** over every remaining locator: other CLs' rows from the loader, the plugin's
   rows, and its pattern. They must all resolve to one address.
4. Otherwise the request fails, and the plugin is refused with the reason in the failure window.

An `absent` row for the running CL ends the search for the requests it binds.

**AMBIGUOUS.** Two locators for the same build that resolve to different addresses. The loader
picks neither, fails the request, and the log lists each locator and where it landed
(`AMBIGUOUS: the locators that resolve disagree:`).

**Shared names.** A plugin may not be named `ModLoader`, and a plugin whose name contains a `/`
reads no site keys. When two loaded plugins share a name, neither reads site keys, because
`<owner>/` cannot tell them apart. Each of these fails the request with a log line saying why.

A key that is not in the map at all (a third-party plugin, or a hook newer than the feed) falls
back to the plugin's own row and pattern, logged as `not in the loader's map; own pattern`.

Fields follow the same shape: the map's row for the running CL first, then the plugin's, then
`builtOffset` on its `builtCL`. Two rows for one CL that disagree fail the request. A reflected
field is still read live from reflection; its row is a cross-check. A native offset plus its
size must lie inside the class's live size. See the SDK guide for the `[Map]` log lines.

## Admission

Each plugin is judged against the build it was compiled for, not the loader's. `PluginInfo` ends
in a tail from interface v72: `gameBuiltCL` (the build its Game-SDK came from,
`PLUGIN_GAME_BUILT_CL`) and `mapFlags`.

- On its own built CL a plugin loads as it always did.
- On any other supported CL it is admitted only if it is interface 72 or later, sets
  `PLUGIN_MAP_MIGRATED` in `mapFlags`, and took non-null globals from `GetSdkGlobals` in
  `PluginInit`. Otherwise the Plugins tab shows it as **Incompatible**, with "Needs an update to
  run on CL-...", and it still loads on its own build.
- A plugin before v72 has no tail and counts as built for `kLegacyPluginCL` (127004), the build
  the last loader without a map required. So does a v72 plugin whose `gameBuiltCL` is 0, with the
  warning "declares no game build". A tail that sets `PLUGIN_MAP_MIGRATED` and names no build is
  refused on every build.

`PLUGIN_MAP_MIGRATED` is the plugin's promise that it reads no game data from compiled-in
offsets or addresses (the checklist is in the SDK guide). The loader does not check that
promise; starrupture-ci checks it for the plugins it tracks.

## The feed

Between loader releases the map can be refreshed from a signed file, so a new game build is
supported without a new loader.

**Format.** A tab-separated text file, one row per line, in the order `format`, `seq`, `build`,
`function` / `absent`, `field`, then a last line `sig<TAB><keyId><TAB><base64>`. `format` is `1`;
the loader reads only the format it was built for. `seq` is the UTC second of signing. `sig` is an
Ed25519 signature (RFC 8032) over every byte before that line. The row layout is in
starrupture-ci's `ci/VERSION-MAP.md`.

**Asset.** `version-map-client.v1.txt`, on the loader's Latest release. The name carries the
format version, so a later format never strands a loader that reads this one. Latest is where it
sits because `prune_releases.yml` deletes the others: `release.yml` attaches the current signed
file to each new release, and between releases `vmap-publish.yml` replaces it. There is no
server feed: only the client fetches.

**Fetch.** The URL is the compiled define `VMAP_FEED_URL` (`/p:VmapFeedUrl=` in `Shared.props`),
never read from `modloader.ini`. An empty define, or no compiled public key, means no fetch. The
loader fetches only on the client, with `[AutoUpdate] Enabled` on, and only:

- at startup, when the running exe is in neither the built-in map nor the cache, or has an
  `absent` row and the cache is over a day old. The splash shows "Downloading game data for
  CL-n". This happens with the game's main thread parked, where WinHTTP is safe;
- after the version check refuses the game build, for the next launch, before the process exits;
- from the auto-update thread, at most once a day, for the next launch.

**Request.** One HTTPS GET. A URL that is not `https` is refused, redirects from HTTPS to HTTP are
disallowed, each operation has a 2 s timeout, the whole request has an 8 s deadline, and the body
is capped at 1 MiB while it is read (`feed_fetch.h`, `http_get.cpp`).

**Cache.** The signed file itself, `ModLoader\version-map-client.v1.cache`, written to a temp
file and then renamed over. It is verified again on every launch and only ever replaced by a file
with a higher `seq`, so an old signed file cannot be replayed over a newer one.

**Signature.** The file is verified before anything in it is read. It is rejected whole if it is
over 1 MiB, has a malformed line, has a `sig` line that is not last, names a key the loader does
not hold, or has a bad signature. Verification uses vendored Monocypher (Ed25519 with SHA-512),
which has no handles and behaves the same on Windows and Wine.

**Keys.** Two public-key slots are compiled in (`version_feed.cpp`), so a key can be rotated by a
loader release that adds the new key and a later one that drops the old.

| Slot | Key | Trusted by |
|---|---|---|
| `k1` | TEST key | only a build that defines `VMAP_TRUST_TEST_KEY` (the unit tests and `sr-aobscan`) |
| `k2` | release key | every build |

A release build therefore trusts only `k2`. A feed signed with `k1` is rejected by it. The
private halves are CI secrets (`VMAP_SIGN_KEY_TEST`, `VMAP_SIGN_KEY`); a leaked key is answered by
a loader release that drops its slot.

**Merge.** The loader merges the built-in map and the feed into one row per key and CL (per `fp`
for a site key), once at startup, and the result stays unchanged for the process. For each group
it takes the built-in row for the running exe if there is one, otherwise the feed's, otherwise
the built-in. The feed can add builds and keys and fill a shipped `absent` row; it can never
change a locator the loader shipped with for the running exe. A CL known only from the feed is
supported only if the union holds a locator for every required loader entry and loader field on
it; otherwise it is dropped and the build stays refused.

**What a bad feed can do.** It cannot run code. Every row still has to pass its kind check, the
uniqueness rule, equality with its `rva` on its own exe, and for a native field the class-size
bound. The worst a wrong signed row can do is point a plugin's hook at the wrong game function,
which is why the file is signed.

## How a build reaches players (CI)

[starrupture-ci](https://github.com/msantoro12/starrupture-ci) watches Steam and does the work.
"Path M" is the update that is only rows in the map. The tools are described in its
`ci/MAP-MODE.md`.

```
new Steam build
  -> client check (build, scan patterns, PDB layouts)
  -> decide.mjs          path M / B / C, from the gate results
  -> map-mode.mjs        function and site rows from the harness (sr-aobscan)
  -> field-rows.mjs      field rows from the layouts
  -> staging.mjs         rows on the loader fork's auto/update-<CL> branch
  -> export-feed         the header as the unsigned feed
  -> vmap-sign.mjs       sign (k2 for a release, k1 for a dry run), verify
  -> promote.mjs         only when public is the same build as the staged one
  -> publish             the vmap-feed release in starrupture-ci
  -> vmap-publish.yml    in the loader repo: verify, upload to Latest
  -> players             fetch on their next launch
```

1. **Client check.** Checks the build and scans the loader's and the plugins' patterns on it.
2. **Decide.** `decide.mjs` turns the gates into a path. M is a clean update: rows only. B is a
   required pattern missed, repaired by a source patch. C is a used class that moved, held until
   that path is proven. A used class that moved is settled under M when the field rows cover it
   with a row, an absent row or a class-size move.
3. **Rows.** `map-mode.mjs` writes a function row per key (the request's own pattern if it
   resolved, otherwise a high-confidence suggestion unique on the build, otherwise an `absent`
   row) and `field-rows.mjs` writes the field rows.
4. **Export, sign, verify.** The header is exported as the feed and signed. `seq` is the larger
   of the clock and the last published `seq` plus one, and the signer refuses a `seq` more than a
   day ahead.
5. **Promote.** A beta build is staged, and promoted when public flips to the same build (same
   build id, exe SHA-256 and CL), after the DLLs are scanned again on the public exe.
6. **Publish.** The `publish` job signs with `k2`, in the `vmap-sign` environment, verifies the
   result without the test key, and replaces the asset on the `vmap-feed` release.
   `vmap-publish.yml` in this repository pulls it every 15 minutes, checks it against the keys in
   `version_feed.cpp`, and uploads it to Latest only if it verifies and its `seq` is higher than
   the one already there. This repository holds none of the signing secrets.

**Publishing is off by default.** The `publish` input of `sr-client-check.yml` defaults to false,
and `state/autopilot.json` decides what may run unattended: its `enabled` switch, the `paths` it
allows (`M`) and `max_releases_per_day`. With `enabled: false`, `decide.mjs` returns HOLD
("autopilot is disabled") on a clean update, and nothing is exported, signed or published until
the owner turns it on. `vmap-watchdog.yml` opens an issue when Latest's feed lags the last signed
`seq` by more than two hours.

## Failure modes

What a player sees, and what the log says (`ModLoader\Logs\ModLoader.log`).

| Case | Result |
|---|---|
| The running build is in the map | Starts with no network request. This is the normal case. |
| New build, no feed published yet, or no network | The gate refuses the build. The player gets the "Start Game Without ModLoader" dialog and can play unmodded or quit. The loader tries again for the next launch. |
| New build, feed published | The splash shows "Downloading game data for CL-n" for a moment, then the game starts modded. |
| A fetch fails (404, timeout, over 1 MiB, not https) | As if there were no feed: a known build runs, an unknown one is refused. At most one fetch per day while a cache would help. |
| The feed's signature is bad, the key unknown or the file malformed | The file is discarded whole, and the cache is untouched. Same as no feed. |
| The feed is older than the cache (`seq` not higher) | Ignored. The cache stays. |
| A release build is handed a `k1` (TEST) feed | Rejected as a bad key. |
| `[AutoUpdate] Enabled` off, or a server build | No fetch. The built-in map is used. A server stays exact on the build it was written for. |
| The feed has the build, but a row is `absent` for something a plugin needs | That plugin's request fails, and the plugin is refused with the reason in the failure window. Other plugins and the game run. |
| The feed has the build but misses a required loader entry | That CL is dropped from the feed, and the build is refused as above. |
| A plugin built for another build does not declare `PLUGIN_MAP_MIGRATED` | **Incompatible**: "Needs an update to run on CL-...". It loads on its own build. |
| Two keys for one request disagree (AMBIGUOUS) | The request fails. The plugin is refused with the locators listed. |
| A game class a plugin reads moved | The loader's layout check turns every plugin off for the session, with a report, as before. |

A fetched map is read once at startup and never changes mid-session, so a RELOAD sees the same
rows the loader's own hooks were installed from.
