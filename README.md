# plutonium-mac-aio

Play [Plutonium](https://plutonium.pw) Call of Duty: Black Ops (T5) and
Black Ops II (T6) on an Apple Silicon Mac, with free and open-source software
only.

Setup downloads a patched Wine engine (built from source by this project),
puts it in [Sikarugir](https://github.com/Sikarugir-App/Sikarugir)'s wrapper
template, and installs a Wine prefix and a fresh Plutonium, all inside this
folder. No CrossOver needed.
Nothing else on the Mac is changed: not Steam, not your games.

Tested on an M1 Pro. The older version that runs on your own copy of
CrossOver (v0.1.0) is on the
[`crossover`](https://github.com/LuidgiAlt/plutonium-mac-aio/tree/crossover)
branch.

## WARNING
This repository is the only one OFFICIAL way to play BO1 (multiplayer and zombies) and BO2 (multiplayer, zombies and campaign) on Apple Silicon using Plutonium (Online and Offline).
THERE WILL NEVER BE A PUBLIC WEBSITE ! The real source is this repo !
BE CAREFUL !

## Requirements

- Apple Silicon Mac with Rosetta 2 (`softwareupdate --install-rosetta`)
- Black Ops and/or Black Ops II on Steam, and a Plutonium account

## Install

```bash
git clone https://github.com/LuidgiAlt/plutonium-mac-aio.git
cd plutonium-mac-aio
./setup.sh
```

1. Setup downloads the engine from this repository's Releases, Sikarugir's
   template, DXMT and Plutonium's installer (each checked by SHA-256 in
   `manifest.txt`, except Plutonium's).
2. When Plutonium's installer window opens, log in, then close it.
3. Setup prints two `Z:\...` paths. The first time the Plutonium launcher asks
   where a game is, paste the matching path.

Running `./setup.sh` again resumes or repairs a setup. `./setup.sh uninstall`
removes everything setup made.

## Play

```bash
./launch.sh
```

The Plutonium launcher opens; pick BO1 or BO2 there. Run it from Terminal.

| Option | Effect |
|---|---|
| `HUD=1` | Apple's Metal performance HUD (frame rate) |
| `MAXFPS=<n>` | Cap the frame rate of both games, e.g. `MAXFPS=60` (see Frame pacing) |
| `WINDOWED=1` / `FULLSCREEN=1` | Display mode for both games (BO2: its own options win) |
| `TWEAKS=0` | Keep BO1's blur, distortion and frame cap (turned off by default for speed) |
| `XAUDIO=builtin` | Wine's XAudio2 instead of Microsoft's (BO1 gunfire is then silent) |
| `MOUSE_COALESCING=1` | macOS's batching of mouse moves, one batch per display refresh (off by default from the engine after 0.2.0: every mouse report reaches the game, which matters most with 500–1000 Hz mice) |
| `X87=0` | Without the x87 acceleration (see How it works) |
| `NTDLL=legacy` | Older breakpoint emulation, if Plutonium hangs at "Loading fastfile" |
| `PLUTONIUM_START=launcher` | Skip Plutonium's updater |
| `VERBOSE=1` | More Wine errors in the log |

Example: `HUD=1 MAXFPS=120 ./launch.sh`

| Command | |
|---|---|
| `./launch.sh kill` | Stop everything running in the prefix |
| `./launch.sh update` | Run Plutonium's updater only |
| `./launch.sh log` | Open the last run's log |
| `./launch.sh selftest` | Check the installation without a game |

## Updating

```bash
git pull
./setup.sh
```

`git pull` brings script fixes, which the next `./launch.sh` uses. `./setup.sh`
then installs only what changed in `manifest.txt` (a new engine, Sikarugir's
template or DXMT) and skips the rest: your prefix, Plutonium login and game
settings stay. No uninstall needed.

**From 0.1.0 (CrossOver):** the same two commands. Setup makes the new runner
and keeps your prefix, Plutonium login and game settings; CrossOver is no
longer used. The old runner can then be removed:

```bash
rm -rf runner/CrossOver runner/.binaries
```

## Troubleshooting

| Problem | What to do |
|---|---|
| "Run ./setup.sh first" | Setup has not finished: run it. |
| "build failed" (`BUILD=1`) | Read the log it names; check the tools in Building from source. |
| "Plutonium is running" | A launch is still running; *Quit and restart* stops it. |
| "Plutonium stopped right away" | Read the log shown; try `VERBOSE=1 ./launch.sh`. |
| A crash or glitch that goes away with `X87=0` | Please report it with the log; play with `X87=0 ./launch.sh` meanwhile. |
| Hangs at "Loading fastfile" | `NTDLL=legacy ./launch.sh` |
| BO2 crashes on start | Usually a BO2 profile asking for 8x MSAA (see Known issues). |
| Stutter in the first half-minute | Shaders are compiled and cached; wait in the menu, it gets better as you play. |

## Known issues

- **BO2 and 8x MSAA:** BO2 takes its anti-aliasing and fullscreen setting
  from a binary profile (`players/hardware_mp.chp` / `hardware_zm.chp`). One
  written by another renderer (e.g. Steam in another Wine setup) can ask for
  8x, which Apple GPUs cannot do, and the game crashes ("Oops! The game ran
  into a problem", `E_INVALIDARG`). `./launch.sh` makes BO2 create its own
  safe profile in Plutonium's folder the first time, so a fresh install
  starts windowed without MSAA: switch to fullscreen in BO2's options.
- **BO1 shadows** are kept off in new configs (`sm_enable "0"`).
- **Plutonium's installer** cannot be pinned by hash (Plutonium updates it);
  setup checks that it is a signed Windows program.

## Frame pacing

A display shows a new frame only at its refresh (every 8.3 ms at 120 Hz).
A game that renders at, say, 100 FPS has some frames shown after one refresh
and others after two, so motion moves in uneven steps even at a high frame
rate. `HUD=1 ./launch.sh` shows the frame times as a graph.

If the frame rate swings below the refresh rate, cap it at a whole fraction
of the refresh that the game always holds, e.g. `MAXFPS=60 ./launch.sh` on a
120 Hz display: every frame then stays on screen for exactly two refreshes.

## How it works

`build/build.sh` builds the whole Wine from CodeWeavers' published CrossOver
26.3 source with the patches below, as a Sikarugir engine
(`build/out/wswine.bundle`): MoltenVK 1.4.2, DXVK's `d3d9.dll` and a second,
older breakpoint ntdll (`NTDLL=legacy`) are inside. Its 32-bit Windows code
uses SSE maths: the x87 code mingw makes by default is slow under Rosetta.

The games' own physics and effects code uses x87 maths too, which Rosetta
translates slowly: in heavy moments the game's threads, not the GPU, set the
frame rate. The engine starts the Plutonium bootstrapper, the process both
games run in, under x87sidecar (`x87sidecar/`, a reduced and hardened build
of [x87sidecar](https://github.com/athei/x87sidecar) 1.9.1): it hooks
Rosetta's translator and turns runs of x87 instructions into native ARM64
code (patch 0023). Measured on an M1 Pro in
back-to-back sessions: BO2 Zombies' lowest second went from 37 to 100 FPS,
BO1 multiplayer from 80 to 112 FPS on average, and the BO1 campaign from 72
to 104 FPS with 2 seconds under 60 FPS instead of 134 (its sound, choppy
before, is clean). Each game's log says `x87 acceleration on`; if it cannot
hook Rosetta (after a macOS update, for example), or the x87sidecar stops,
the game keeps running at the old speed.
`X87=0 ./launch.sh` turns it off.

The engine links against the x86_64 libraries of Sikarugir's wrapper
template (FreeType, gnutls, SDL2), pinned in `manifest.txt`.

Setup copies the template to `runner/Sikarugir.app` with the engine in its
`Contents/Frameworks`, where the engine finds those libraries, and
`runner/engine` links to it. BO2 renders through
[DXMT](https://github.com/3Shain/dxmt), BO1 through DXVK on
[MoltenVK](https://github.com/KhronosGroup/MoltenVK). Sikarugir's own engines
do not have the Rosetta breakpoint patches, so Plutonium does not run on
them.

## What is patched

| Patch | Fixes |
|---|---|
| wine 0001–0002 | Debug registers and execution breakpoints under Rosetta (Plutonium needs them) |
| wine 0004 | Pages become executable on first use instead of trapping every write (stutter) |
| wine 0015 | PulseEvent yields only when needed (BO2 multiplayer) |
| wine 0021 | Empty socket receives answered without wineserver (online stutter) |
| wine 0022 | Every mouse report reaches the game instead of one batch per display refresh (high-rate mice) |
| wine 0023 | The games' process starts under x87sidecar: faster x87 maths (heavy-moment frame drops) |
| wine 0003, 0005–0019 | wined3d's Vulkan renderer for Direct3D 9 (BO1 uses DXVK) |
| dxvk 0001–0003 | DXVK 1.10.3 on MoltenVK 1.4.2 for BO1 |

Each patch in `patches/` starts with a header explaining what it changes and
why.

## Building from source

The release's engine is built by `build/build.sh` (about an hour the first
time). To build and use your own instead:

```bash
xcode-select --install
brew install mingw-w64 bison flex meson ninja glslang sdl2 cmake
BUILD=1 ./setup.sh
```

`build/build.sh --check` checks that every patch applies, `tests/sh/run.sh`
runs the shell tests, and `tests/run.sh` the Wine tests in the runner (also
`./launch.sh selftest`). `build/build.sh --pack <version>` packs the engine
and the test programs for a release and prints its `manifest.txt` line.

## Controller support
- DualSense (Sony pads through SDL as XInput gamepads (the games read XInput only).
- Xbox Controller (not tested since I do not have an Xbox Controller but should be supported by Wine).

## Licenses

- Scripts and test programs: GPL-3.0-or-later (`LICENSE`); up to v0.2.0 they were MIT.
- Wine patches and the engine built from them: LGPL-2.1-or-later, built from
  CodeWeavers' published source `crossover-sources-26.3.0.tar.gz`
  (https://media.codeweavers.com/pub/crossover/source/).
- DXVK patches and DXVK's `d3d9.dll`: zlib, from the same source archive.
- MoltenVK 1.4.2 inside the engine: Apache-2.0, from Khronos' release.
- x87sidecar inside the engine (`bin/x87sidecar`): MIT, built from
  `x87sidecar/`, derived from https://github.com/athei/x87sidecar 1.9.1; its
  license is in `x87sidecar/LICENSE` and `share/doc/x87sidecar/LICENSE`.
- Sikarugir's wrapper template, DXMT and Plutonium are downloaded by setup
  from their own releases; they are not part of this repository or its
  release.

Patches 0001/0002 started from
[wine-rosetta-fix](https://github.com/masonmcbride/wine-rosetta-fix).
