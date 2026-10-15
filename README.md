# bfme2-map-quickload

Faster map loading for *The Battle for Middle-earth II* (1.06) and the verified
*Rise of the Witch-king* 2.01 engine. One
DLL in the game folder; it builds the map's terrain textures on all your
processor cores instead of one, and skips the five seconds the game shows its
title logo after every load.

> Built with help from [OpenBFME2](https://github.com/Open-BFME/Open-BFME-2).

<img src="media/loading-comparison.gif" width="648" alt="Three maps loading side by side, unpatched and patched, each with a stopwatch">

| Map (Medium detail) | Unpatched | Patched | |
| --- | --- | --- | --- |
| The Greywater Marches, 1,350 x 900 cells | 50.5 s | 10.2 s | 5.0x faster |
| Grey Mountains, 485 x 550 cells | 16.9 s | 6.1 s | 2.8x faster |
| Harlindon, 500 x 500 cells | 15.4 s | 5.3 s | 2.9x faster |

Seconds from starting a skirmish to its first frame of play, on a 16-thread
CPU; the animation plays at 2x speed. Bigger maps gain more. These runs leave
out the title logo the game shows after loading, so the patch saves another
five seconds on top of them.

## Install

1. Download `dinput8.dll` from the [latest release](../../releases/latest)
   (or `bin/dinput8.dll` here).
2. Copy it into your BFME2 or ROTWK folder, next to `game.dat`.

That's all: start the game as usual. To uninstall, delete `dinput8.dll` from
the game folder.

- **BFME2 1.06 and verified ROTWK 2.01.** The DLL checks image size, terrain
  call sites, import thunks and bindings, plus the view-update hook bytes,
  before installing. Unrecognized layouts only pass DirectInput through to
  Windows. A matching version number alone is insufficient; see below.
- If your game folder already has a `dinput8.dll` (another mod), don't
  replace it: only one can be loaded.
- The patch changes how fast textures are built, not what the game
  simulates, so it shouldn't affect multiplayer; that is untested.

## What it does

Most of a map's loading time is the game building its terrain textures: for
each one, it composites the terrain on the CPU, then Microsoft's D3DX library
compresses it to DXT1 and builds its mipmaps, one texture after another on the
main thread (about 85% of the load, 1,156 textures on a 480 x 480 map).

The game imports `DirectInput8Create` from `dinput8.dll` and finds this copy
before Windows' own, which it forwards to. In a supported `game.dat` the DLL also:

- redirects the game's two import slots for `D3DXLoadSurfaceFromMemory` and
  `D3DXFilterTexture`. Called from the terrain texture builder
  (`TerrainTextureClass::updateFlatDXT1`) while the loading screen shows, each
  texture's pair of calls runs on a worker thread, one fewer than your
  processor count, up to 15: the same two D3DX calls with the same input, into
  a CPU-only staging copy made on a Direct3D device of the DLL's own
  (multithreaded, on a window that is never shown). The finished textures are
  copied into the game's on the main thread, the last ones in the tactical
  view's first update after the loading screen, before the map is drawn.
  Same functions, same input: the terrain comes out the same, and screenshots
  with and without the patch differ only where things move.
- sets the 5000 ms of the title logo's wait after loading
  (`beginTime + 5000 > timeGetTime()`) to 0. The fade-in after it still runs.

Any other caller, texture outside Direct3D's managed pool, or the game after
loading gets the game's own synchronous calls.

## For tools that start the game

`bin/loading.dll` is the launcher build: the same texture work, without the
DirectInput forwarding, the loading-screen detection or the logo change. A
tool that loads it into the game (for example with `LoadLibraryA` on the main
thread at the first engine frame) calls its exported `mwFlush()` (cdecl, no
arguments, returns the number of textures handled) on the main thread when
the map has loaded, before the first frame; from then on the DLL passes
everything through. [bfme2-map-studio](https://github.com/NotNBR/bfme2-map-studio)
does this for its test games, calling it when the new-game pass ends
(`game.dat` RVA 0x248767 for BFME2), or when ROTWK destroys its completed
loading screen (RVA 0x225524, after textures were queued). Both builds export `mwLoading`, ten counters:
installed, active, workers, deferred, copied, synchronous, failed, peak queue,
last error, flushes.

## ROTWK verification

The verified engine has file version `2.1.2614.37001` and SHA-256
`5481de75b63e22483660ea8097478faef594f90b9ade7de893e092256ba68a26`.
Earlier local analysis records scheduling differences in this executable;
this is not a claim that it is untouched retail or that every 2.01 binary
has the same layout. Its hooks were located in local disassembly:

| Hook (RVA) | BFME2 1.06 | ROTWK |
| --- | --- | --- |
| D3DX load / filter import slots | `7BAA14` / `7BAA18` | `7D0A1C` / `7D0A20` |
| Terrain call returns | `EF5FC` / `EF622` | `EEFBD` / `EEFE3` |
| GameLogic pointer (LoadScreen at +120) | `9FE78C` | `9E412C` |
| View update | `85B36` | `8546A` |
| Title-logo wait instruction | `23965D` | `245CB1` |

On 15 October 2026, Water Lab at Medium detail in an isolated clean ROTWK
2.01 asset profile reached logic frame 10 in **23.2 s without the fix,
11.6 s with loading.dll, and 10.2 s with the standalone build**. All runs
used the same map and hidden DXVK renderer; the logo was already omitted in
all three. Each accelerated run handled all 1,260 terrain textures on 15
workers, with zero failed jobs or first-chance exceptions. These are single
local runs, not a general performance guarantee. The terrain algorithm and
D3DX arguments are unchanged. A separate automatic-load test placed
`dinput8.dll` beside the isolated engine and reached frame 11 in 12.3 seconds,
again with all 1,260 textures copied and zero failures, verifying the proxy's
import forwarding too. BFME2 regression tests passed with both DLL builds,
including the studio camera DLL alongside the launcher build.
Multiplayer and other ROTWK executables have
not been verified.

## Building

```
python build.py --toolchain "C:/.../Microsoft Visual Studio .NET 2003"
```

builds `bin/dinput8.dll` (`loading.cpp` with `MW_STANDALONE`) and
`bin/loading.dll` with Visual C++ .NET 2003 (MSVC 7.1), which links
`msvcr71.dll` as the game does. The game addresses come from
[Open-BFME-2](https://github.com/Open-BFME/Open-BFME-2)'s matched sources.

## Licence

GPL-3.0-only; see [LICENSE](LICENSE). This project is not endorsed by or
affiliated with EA or its licensors; it contains no game files.
