# plutonium-mac-aio

Play [Plutonium](https://plutonium.pw) Call of Duty: Black Ops (T5) and
Black Ops II (T6) on an Apple Silicon Mac, with your own copy of CrossOver.

One setup command installs a patched copy of CrossOver's runtime, a Wine prefix
and a fresh Plutonium, all inside this folder. Nothing else on the Mac is
changed: not CrossOver.app, not Steam, not your games.

Tested on an M1 Pro with CrossOver 26.3.

## Requirements

- Apple Silicon Mac with Rosetta 2 (`softwareupdate --install-rosetta`)
- [CrossOver](https://www.codeweavers.com/crossover) **26.3.0** (build
  26.3.0.39832), activated. Setup stops on any other build.
- Black Ops and/or Black Ops II on Steam, and a Plutonium account

## Install

```bash
git clone https://github.com/LuidgiAlt/plutonium-mac-aio.git
cd plutonium-mac-aio
./setup.sh
```

1. When Plutonium's installer window opens, log in, then close it.
2. Setup prints two `Z:\...` paths. The first time the Plutonium launcher asks
   where a game is, paste the matching path.

Running `./setup.sh` again resumes or repairs a setup. If CrossOver is updated,
setup asks before rebuilding the runner. `./setup.sh uninstall` removes
everything setup made.

## Play

```bash
./launch.sh
```

The Plutonium launcher opens; pick BO1 or BO2 there. Run it from Terminal.

| Option | Effect |
|---|---|
| `HUD=1` | Apple's Metal performance HUD (frame rate) |
| `MAXFPS=<n>` | Cap BO2's frame rate, e.g. `MAXFPS=60` |
| `WINDOWED=1` / `FULLSCREEN=1` | Display mode for both games (BO2: its own options win) |
| `TWEAKS=0` | Keep BO1's blur, distortion and frame cap (turned off by default for speed) |
| `D3D9=vulkan` | BO1 on wined3d's Vulkan renderer instead of DXVK |
| `XAUDIO=builtin` | Wine's XAudio2 instead of Microsoft's (BO1 gunfire is then silent) |
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

## Troubleshooting

| Problem | What to do |
|---|---|
| "Run ./setup.sh first" | Setup has not finished: run it. |
| "CrossOver was updated" | Run `./setup.sh` again. |
| "Plutonium is running" | A launch is still running; *Quit and restart* stops it. |
| "Plutonium stopped right away" | Read the log shown; try `VERBOSE=1 ./launch.sh`. |
| Hangs at "Loading fastfile" | `NTDLL=legacy ./launch.sh` |
| BO2 crashes on start | Usually a BO2 profile asking for 8x MSAA (see Known issues). |
| Black textures for a moment on a new map | Shaders are compiled the first time; it gets better as you play. |

## Known issues

- **BO2 and 8x MSAA:** BO2 takes its anti-aliasing and fullscreen setting
  from a binary profile (`players/hardware_mp.chp` / `hardware_zm.chp`). One
  written by another renderer (e.g. Steam in a CrossOver bottle) can ask for
  8x, which Apple GPUs cannot do, and the game crashes ("Oops! The game ran
  into a problem", `E_INVALIDARG`). `./launch.sh` makes BO2 create its own
  safe profile in Plutonium's folder the first time, so a fresh install
  starts windowed without MSAA: switch to fullscreen in BO2's options.
- **BO1 shadows** are kept off in new configs (`sm_enable "0"`).
- **Plutonium's installer** cannot be pinned by hash (Plutonium updates it);
  setup checks that it is a signed Windows program.

## What is patched

| Patch | Fixes |
|---|---|
| wine 0001–0002 | Debug registers and execution breakpoints under Rosetta (Plutonium needs them) |
| wine 0004 | Pages become executable on first use instead of trapping every write (stutter) |
| wine 0015 | PulseEvent yields only when needed (BO2 multiplayer) |
| wine 0021 | Empty socket receives answered without wineserver (online stutter) |
| wine 0003, 0005–0019 | wined3d's Vulkan renderer for Direct3D 9 (`D3D9=vulkan`) |
| dxvk 0001–0003 | DXVK 1.10.3 on MoltenVK 1.4.2 for BO1 |

Each patch in `patches/` starts with a header explaining what it changes and
why. BO2 renders through [DXMT](https://github.com/3Shain/dxmt), BO1 through
DXVK on [MoltenVK](https://github.com/KhronosGroup/MoltenVK).

## Building from source

Setup downloads prebuilt binaries from this repository's Releases (SHA-256 in
`manifest.txt`). To build them yourself instead:

```bash
xcode-select --install
brew install mingw-w64 bison flex meson ninja glslang
BUILD=1 ./setup.sh
```

`build/build.sh --check` checks that every patch applies, `tests/sh/run.sh`
runs the shell tests, and `tests/run.sh` the Wine tests.

## Licenses

- Scripts and test programs: MIT (`LICENSE`).
- Wine patches and the binaries built from them: LGPL-2.1-or-later, built from
  CodeWeavers' published source `crossover-sources-26.3.0.tar.gz`
  (https://media.codeweavers.com/pub/crossover/source/).
- DXVK patches and DXVK's `d3d9.dll`: zlib, from the same source archive.
- DXMT, MoltenVK and Plutonium are downloaded from their own releases.
  CrossOver is not included: setup copies your installed, licensed CrossOver.

Patches 0001/0002 started from
[wine-rosetta-fix](https://github.com/masonmcbride/wine-rosetta-fix).
