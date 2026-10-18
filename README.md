# Super Mario Sunshine — PC Port (unofficial)

A native Windows port of Super Mario Sunshine, built from the community
[decompilation](https://github.com/doldecomp/sms) and the [Aurora](https://github.com/encounter/aurora)
GameCube/Wii platform layer (WebGPU: Direct3D 12 / Vulkan).

> **No game assets, Nintendo code or data are included.** You need your own legally obtained copy of the
> game (GameCube, NTSC-U `GMSE01`). Everything the game shows or plays is loaded from your disc image at runtime.

## Features

- Runs natively on Windows at any resolution, with widescreen support
- High and unlimited frame rates with frame interpolation (the game logic still runs at its original speed)
- Graphics options: internal resolution scale, MSAA, FXAA, anisotropic and trilinear filtering
- Controller, keyboard and mouse support, plus an optional free camera
- Online multiplayer through a lobby server (`port/server/` contains the server so anyone can host one)
- Discord Rich Presence (current area, Shines, Blue Coins) with join invites
- Built-in bug reporter: press **F9** (or crash) to save the last 20 seconds as video + log
- Faithful to the original: the game code is the decompiled original, original bugs included

## Requirements

- Windows 10/11 (x64) with a Direct3D 12 or Vulkan capable GPU
- Your own Super Mario Sunshine disc image (NTSC-U `GMSE01`), dumped from your disc

## Building

The port builds from source; see [`port/AURORA.md`](port/AURORA.md) for the full toolchain setup.
In short:

1. Build Aurora (not included) and apply the patches in [`port/patches/`](port/patches/) on top of a clean checkout.
2. Run `python port/tools/build_aurora.py --stubs -j 20`.
3. Point the game at your disc image (`disc =` in `port_settings.ini`) and start `sms_port.exe`.

The exe icon can be generated from your own disc with `python port/tools/make_icon.py <disc files/data folder>`.

## Repository layout

| Path | Contents |
| --- | --- |
| `src/`, `include/`, `libs/` | The decompiled game (matching decompilation, see [docs/DECOMP_README.md](docs/DECOMP_README.md)) |
| `port/src/` | PC platform code: input, audio, rendering glue, multiplayer, Discord, settings |
| `port/patches/` | Patches for Aurora |
| `port/server/` | Lobby / relay server for online play (Python, no dependencies) |
| `docs/` | Decompilation and contributor documentation |

## Credits

- The [doldecomp/sms](https://github.com/doldecomp/sms) decompilation project and its contributors
- [Aurora](https://github.com/encounter/aurora) by Luke Street (MIT)

## Legal

This is an unofficial fan project, not affiliated with or endorsed by Nintendo. Super Mario Sunshine is a
trademark of Nintendo. Do not open issues or pull requests containing game files, disc images, extracted
assets or links to them; they will be removed.

The decompiled source is released under [CC0-1.0](LICENSE).
