<div align="center">

# plutonium-mac-aio

**Play [Plutonium](https://plutonium.pw) Black Ops and Black Ops II on Apple Silicon Macs, with free and open-source software only.**

[![Release](https://img.shields.io/github/v/release/LuidgiAlt/plutonium-mac-aio?label=release&color=2ea44f)](https://github.com/LuidgiAlt/plutonium-mac-aio/releases/latest)
[![Downloads](https://img.shields.io/github/downloads/LuidgiAlt/plutonium-mac-aio/total?color=2ea44f)](https://github.com/LuidgiAlt/plutonium-mac-aio/releases)
[![License](https://img.shields.io/badge/license-GPL--3.0-blue)](LICENSE)
<br>
[![macOS](https://img.shields.io/badge/macOS-Apple%20Silicon-000000?logo=apple&logoColor=white)](#requirements)
[![Games](https://img.shields.io/badge/Plutonium-BO1%20%7C%20BO2-f26b1d)](#what-works)
[![CrossOver](https://img.shields.io/badge/CrossOver-not%20needed-6f42c1)](#how-it-works)
[![Wine](https://img.shields.io/badge/Wine-CrossOver%2026.3%20source-8b0000?logo=wine&logoColor=white)](#how-it-works)

[Quick start](#quick-start) ·
[Options](#options) ·
[Troubleshooting](#troubleshooting) ·
[How it works](#how-it-works) ·
[Building](#building-from-source)

</div>

---

One script sets up everything inside this folder: a patched Wine engine
built from source by this project, [Sikarugir](https://github.com/Sikarugir-App/Sikarugir)'s
wrapper, a Wine prefix and a fresh Plutonium. **No CrossOver licence**, and
nothing else on your Mac is touched: not Steam, not your game files.

> [!WARNING]
> **This repository is the only official source of plutonium-mac-aio.**
> There is no website and there never will be. Anything else using this name
> is not from this project.

## What works

| Game | Multiplayer | Zombies | Campaign | Renderer |
|---|:---:|:---:|:---:|---|
| **Black Ops** (T5) | ✅ | ✅ | ✅ | DXVK on MoltenVK |
| **Black Ops II** (T6) | ✅ | ✅ | ✅ | [DXMT](https://github.com/3Shain/dxmt) (Metal) |

Online and offline play, keyboard and mouse (500–1000 Hz mice included) and
controllers ([see below](#controllers)). Tested on an M1 Pro.

### Performance

Both games do their physics and effects in x87 maths, which Rosetta 2
translates slowly. The engine runs them under an x87 accelerator
([details](#x87-acceleration)). Measured on an M1 Pro in back-to-back
sessions:

| | Without | With |
|---|---:|---:|
| BO2 Zombies, lowest second | 37 FPS | **100 FPS** |
| BO1 multiplayer, average | 80 FPS | **112 FPS** |
| BO1 campaign, average | 72 FPS | **104 FPS** |
| BO1 campaign, seconds under 60 FPS | 134 | **2** |

BO1's campaign sound, choppy before, is clean with it.

## Requirements

- A Mac with Apple Silicon (M1 or later) and Rosetta 2:
  `softwareupdate --install-rosetta`
- **Black Ops** and/or **Black Ops II** installed through Steam
- A [Plutonium](https://plutonium.pw) account

## Quick start

```bash
git clone https://github.com/LuidgiAlt/plutonium-mac-aio.git
cd plutonium-mac-aio
./setup.sh
```

1. Setup downloads the engine from this repository's
   [Releases](https://github.com/LuidgiAlt/plutonium-mac-aio/releases),
   Sikarugir's template, DXMT and Plutonium's installer. Each download except
   Plutonium's is checked against the SHA-256 in `manifest.txt`.
2. When Plutonium's installer window opens, **log in, then close it**.
3. Setup prints two `Z:\...` paths. The first time the Plutonium launcher
   asks where a game is, paste the matching path.

Then play:

```bash
./launch.sh
```

The Plutonium launcher opens; pick BO1 or BO2 there. Always start it from
Terminal.

> [!TIP]
> Running `./setup.sh` again resumes or repairs a setup.
> `./setup.sh uninstall` removes everything setup made.

## Options

Options go before the command, e.g. `HUD=1 MAXFPS=120 ./launch.sh`.

| Option | Effect |
|---|---|
| `HUD=1` | Apple's Metal performance HUD (frame rate and frame times) |
| `MAXFPS=<n>` | Cap the frame rate of both games, e.g. `MAXFPS=60` ([frame pacing](#smoother-motion-with-maxfps)) |
| `WINDOWED=1` / `FULLSCREEN=1` | Display mode for both games (BO2's own options win) |
| `TWEAKS=0` | Keep BO1's blur, distortion and frame cap (off by default for speed) |
| `X87=0` | Turn the x87 acceleration off |
| `MOUSE_COALESCING=1` | macOS's batching of mouse moves, one batch per display refresh (off by default since engine 0.2.1, so every report from a 500–1000 Hz mouse reaches the game) |
| `XAUDIO=builtin` | Wine's XAudio2 instead of Microsoft's (BO1 gunfire is then silent) |
| `NTDLL=legacy` | Older breakpoint emulation, if Plutonium hangs at "Loading fastfile" |
| `PLUTONIUM_START=launcher` | Skip Plutonium's updater |
| `VERBOSE=1` | More Wine errors in the log |

| Command | What it does |
|---|---|
| `./launch.sh kill` | Stop everything running in the prefix |
| `./launch.sh update` | Run Plutonium's updater only |
| `./launch.sh log` | Open the last run's log |
| `./launch.sh selftest` | Check the installation without a game |

### Smoother motion with MAXFPS

A display shows a new frame only at its refresh (every 8.3 ms at 120 Hz). A
game running at, say, 100 FPS has some frames shown for one refresh and
others for two, so motion moves in uneven steps even at a high frame rate.
`HUD=1` shows the frame times as a graph. If the frame rate swings below the
refresh rate, cap it at a whole fraction of the refresh that the game always
holds, e.g. `MAXFPS=60` on a 120 Hz display: every frame then stays on
screen for exactly two refreshes.

## Updating

```bash
git pull
./setup.sh
```

`git pull` brings script fixes, which the next `./launch.sh` uses.
`./setup.sh` then installs only what changed in `manifest.txt` (a new
engine, Sikarugir's template or DXMT). Your prefix, Plutonium login and game
settings stay; no uninstall needed.

<details>
<summary><b>Coming from v0.1.0 (CrossOver)</b></summary>

Run the same two commands. Setup makes the new runner and keeps your prefix,
Plutonium login and game settings; CrossOver is no longer used. The old
runner can then be removed:

```bash
rm -rf runner/CrossOver runner/.binaries
```

The CrossOver version is still on the
[`crossover`](https://github.com/LuidgiAlt/plutonium-mac-aio/tree/crossover)
branch.

</details>

## Troubleshooting

| Problem | What to do |
|---|---|
| "Run ./setup.sh first" | Setup has not finished: run it. |
| "Plutonium is running" | A launch is still running; *Quit and restart* stops it. |
| "Plutonium stopped right away" | Read the log shown; try `VERBOSE=1 ./launch.sh`. |
| Hangs at "Loading fastfile" | `NTDLL=legacy ./launch.sh` |
| BO2 crashes on start | Usually a BO2 profile asking for 8x MSAA ([known issues](#known-issues)). |
| Stutter in the first half-minute | Shaders are being compiled and cached; wait in the menu, it gets better as you play. |
| A crash or glitch that goes away with `X87=0` | Play with `X87=0 ./launch.sh` and please [open an issue](https://github.com/LuidgiAlt/plutonium-mac-aio/issues) with the log. |
| "build failed" (`BUILD=1`) | Read the log it names; check the tools in [Building from source](#building-from-source). |

When reporting a problem, attach the log (`./launch.sh log`) and say which
Mac and macOS version you use.

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
  setup checks that it is a signed Windows program instead.

## Controllers

- **DualSense** and other Sony pads, through SDL as XInput gamepads (the
  games read XInput only).
- **Xbox controllers** should work through Wine (not tested).

## How it works

`build/build.sh` builds the whole Wine from CodeWeavers' published CrossOver
26.3 source with the patches below, as a Sikarugir engine
(`build/out/wswine.bundle`). MoltenVK 1.4.2, DXVK's `d3d9.dll`, a second,
older breakpoint ntdll (`NTDLL=legacy`) and the x87 accelerator are inside.
Its 32-bit Windows code uses SSE maths, since the x87 code mingw makes by
default is slow under Rosetta.

The engine links against the x86_64 libraries of Sikarugir's wrapper
template (FreeType, gnutls, SDL2), pinned in `manifest.txt`. Setup copies
the template to `runner/Sikarugir.app` with the engine in its
`Contents/Frameworks`, where the engine finds those libraries, and
`runner/engine` links to it. Sikarugir's own engines lack the Rosetta
breakpoint patches, so Plutonium does not run on them.

### x87 acceleration

In heavy moments the games' own threads, not the GPU, set the frame rate,
because Rosetta translates x87 instructions slowly. The engine starts the
Plutonium bootstrapper, the process both games run in, under x87sidecar
(patch 0023). x87sidecar hooks Rosetta's translator and turns runs of x87
instructions into native ARM64 code. Each game's log says
`x87 acceleration on`.

This repository's build (`x87sidecar/`) is a reduced and hardened version of
[x87sidecar](https://github.com/athei/x87sidecar) 1.9.1:

- only the attach that Wine starts, with no debugger attach;
- if the accelerator stops, the game keeps running on plain Rosetta at the
  old speed instead of closing;
- an internal error hands that one instruction back to Rosetta;
- it hooks only when it can read Rosetta's own opcode table, so after a
  macOS update it either works or stays out of the way;
- one fixed configuration, about 29,000 lines instead of 42,000.

### What is patched

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

| Command | What it does |
|---|---|
| `build/build.sh --check` | Check that every patch applies |
| `tests/sh/run.sh` | Shell tests |
| `tests/run.sh` (or `./launch.sh selftest`) | Wine tests in the runner |
| `build/build.sh --pack <version>` | Pack the engine and test programs for a release and print its `manifest.txt` line |

## Licenses and credits

- **Scripts and test programs:** GPL-3.0-or-later ([`LICENSE`](LICENSE));
  up to v0.2.0 they were MIT.
- **Wine patches and the engine built from them:** LGPL-2.1-or-later, built
  from CodeWeavers' published source `crossover-sources-26.3.0.tar.gz`
  (https://media.codeweavers.com/pub/crossover/source/).
- **DXVK patches and DXVK's `d3d9.dll`:** zlib, from the same source archive.
- **MoltenVK 1.4.2** inside the engine: Apache-2.0, from Khronos' release.
- **x87sidecar** inside the engine (`bin/x87sidecar`): MIT, built from
  `x87sidecar/`, derived from https://github.com/athei/x87sidecar 1.9.1. Its
  license is in [`x87sidecar/LICENSE`](x87sidecar/LICENSE) and
  `share/doc/x87sidecar/LICENSE`.
- **Sikarugir's wrapper template, DXMT and Plutonium** are downloaded by
  setup from their own releases; they are not part of this repository or its
  release.

Patches 0001/0002 started from
[wine-rosetta-fix](https://github.com/masonmcbride/wine-rosetta-fix).

<div align="center">
<sub>Not affiliated with Plutonium, Activision or CodeWeavers. Call of Duty and Black Ops are trademarks of Activision.</sub>
</div>
