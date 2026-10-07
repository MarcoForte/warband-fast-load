# warband-fast-load

Makes Mount & Blade: Warband (Steam, macOS) reach the main menu in ~2 s instead of ~1 minute.

Measured on an M3 Pro with the Floris Expanded mod pack, warm file cache: launch → main menu
went from ~60 s to ~1.8 s. The first launch after installing takes about 1 s longer while
Rosetta translates the patched binary.

## Requirements

- Warband **1.174** for macOS from Steam (the Intel build; it runs under Rosetta on Apple
  Silicon). The patcher checks the binary and refuses anything else.
- Installed at the default Steam location:
  `~/Library/Application Support/Steam/steamapps/common/MountBlade Warband`
- Xcode Command Line Tools (`xcode-select --install`), for `clang` and `python3`.

## Install

```sh
git clone https://github.com/MarcoForte/warband-fast-load.git
cd warband-fast-load
./install.sh
```

Then launch the game from Steam as usual. Run the game once before installing if it has
never run on this machine, so that `rgl_config.txt` exists (or rerun `install.sh` afterwards).

**Rerun `./install.sh` after a Steam update or "Verify integrity of game files"**, which
restore the original executable. `./install.sh --uninstall` reverts the game files
(the two `rgl_config.txt` settings stay; change them in the launcher if you like).

## What it changes

`install.sh` builds `fastswap.dylib` and a small wrapper, and inside
`Mount and Blade.app/Contents/MacOS`:

- renames the game executable to `Mount and Blade.real` (kept untouched);
- writes `Mount and Blade.patched`, a copy with 4 function entry points redirected through
  pointer slots (`make_patched.py`);
- puts the wrapper in place as `Mount and Blade`. It runs the patched copy with the dylib
  injected, and falls back to the original if the dylib is missing.

It also sets `max_framerate = 10000` and `force_vsync = 0` in
`~/Library/Application Support/MBWarband/rgl_config.txt`. The frame limiter otherwise
throttles the loading loop, and macOS still syncs frames to the display.

## Why it was slow, and what each part does

The loading screen does one small step per rendered frame, and macOS makes every OpenGL
frame wait for the display refresh. That alone was ~25 s. Reverse engineering the binary
(Ghidra) found the rest.

| Source | Fix |
|---|---|
| `fastswap.c` | Skips loading-screen presents (≤ 20/s while loading); caches directory listings, since the port finds files case-insensitively by re-listing directories ~40k times |
| `texhook.c` | With on-demand textures the engine reads every DDS file in full (2.3 GB) only to upload low-res placeholders; this reads just the small mip levels at the end of each file (8 MB) |
| `loadstep.c` | Runs many loading steps per frame instead of one, and keeps loading while waiting for the render thread |
| `fopenhook.c` | Hashed case-insensitive file lookup instead of linear directory scans |
| `shaderdefer.c` | All 293 shader programs were compiled at startup but the menu uses 3; they now compile on first use, and the rest in the background once loading is done |
| `prefetch.c` | Warms the file cache for resource, sound and texture files in parallel at launch |
| `wrapper.c` | Sets `SDL_VIDEO_MAC_FULLSCREEN_SPACES=0`, which skips the ~0.4 s fullscreen Space animation |

Everything except the deferred background shader compile is active only until the engine
logs the end of loading. After that the hooks pass calls straight through.

## Troubleshooting

Each part can be switched off with an environment variable, which helps narrow down a
problem: `FASTSWAP_NOTEX`, `FASTSWAP_NOBATCH`, `FASTSWAP_NOOVERLAP`, `FASTSWAP_NOFOPEN`,
`FASTSWAP_NOSHADER`, `FASTSWAP_NOPREFETCH`, `FASTSWAP_NODIR`, `FASTSWAP_NOTAKEOVER`.
`FASTSWAP_TRACE=/path/to/file` writes a timeline of the load.

## Caveats

- While loading, the deferred shaders report "compiled successfully" without checking. That
  is true for every shader in this game today. If a future macOS release broke one, it would
  show up as a rendering glitch rather than an error message.
- Tested with Floris Expanded 2.54 and checked in the main menu and a custom battle. Other
  modules should work (the hooks are in the engine, not the module) but are untested.

## Mod: companion takeover

When you fall in battle, you immediately continue as the nearest living companion (a hero
from your party), with no loading or cutscene. If that companion falls too, you move on to the
next one. Tab still offers to leave the battle.

```sh
./install.sh   # the dylib part (src/takeover.c)
python3 mods/companion_takeover/install_mod.py \
  "$HOME/Library/Application Support/Steam/steamapps/common/MountBlade Warband/Modules/Floris Expanded Mod Pack 2.54"
```

`--uninstall` restores the module's original `mission_templates.txt` and
`quick_strings.txt`, which the first run saves as `*.takeover-orig`. `--test` also lets you
take over plain soldiers (custom battles have no companions) and makes F9 knock you out.

How it works: the mod adds two battle triggers that find the nearest companion and call the
script operation `player_control_agent`. The engine only allows that operation in
multiplayer, so `src/takeover.c` hooks its argument check and, in single player, switches the
player agent itself. This mirrors what the engine does for the local multiplayer player.

## Reverse engineering helpers

`tools/scripts/Decomp.java` is a Ghidra headless script, and `tools/decomp.sh` runs it on
a Ghidra project of the game binary (decompile by address, by referenced string, or list
references/callers). Ghidra, the JDK and the project are not part of this repo.
