# Luanti on the PlayStation Vita

Luanti runs natively on the PlayStation Vita (and PlayStation TV) as a
homebrew VPK. It is the same engine as on desktop and Android, with a
platform layer for the Vita and defaults tuned for its hardware.

## Requirements

* A Vita with HENkaku/Ensō (any firmware that runs unsigned homebrew).
* `libshacccg.suprx` in `ur0:data/`. vitaGL compiles shaders at runtime with
  the system's Cg compiler, which every user has to extract from their own
  console, for example with
  [ShaRKBR33D](https://github.com/Rinnegatamante/ShaRKBR33D). Luanti shows an
  error message and exits if it is missing.
* Install `luanti.vpk` with VitaShell.

The first start takes a while, since every shader has to be compiled. With the
default build (see below) compiled shaders are cached on the memory card, so
later starts are fast.

## Files

| What                     | Where                         |
|--------------------------|-------------------------------|
| Engine data (read-only)  | `app0:` (inside the VPK)      |
| Settings (`minetest.conf`), worlds, games, mods, texture packs | `ux0:data/luanti/` |
| Shader cache, temp files | `ux0:data/luanti/cache/`      |
| Log (`debug.txt`)        | `ux0:data/luanti/`            |

Games are not bundled: install one from the content browser in the main menu
(it needs a network connection), or copy it to `ux0:data/luanti/games/`.

## Controls

In game (default bindings, all can be changed in the settings):

| Button          | Action                    |
|-----------------|---------------------------|
| Left stick      | Move                      |
| Right stick     | Look around               |
| R               | Dig / punch               |
| L               | Place / use               |
| ✕               | Jump                      |
| ○               | Sneak                     |
| □               | Aux1 (sprint)             |
| △               | Inventory                 |
| D-pad left/right | Previous / next hotbar item |
| D-pad up        | Toggle fly mode           |
| D-pad down      | Drop item                 |
| Select          | Minimap                   |
| Start           | Pause menu                |

On a PlayStation TV, the triggers of a DualShock also dig and place.

The touch screen works as on Android (on-screen controls appear as soon as
you touch it). The rear touch pad is ignored.

In menus and the inventory a pointer is shown:

| Button       | Action                      |
|--------------|-----------------------------|
| Left stick   | Move the pointer            |
| ✕            | Click                       |
| □            | Right click (split stacks)  |
| ○            | Back / close                |
| Right stick  | Scroll                      |
| D-pad        | Arrow keys                  |

Text fields open the system keyboard.

## Performance

The Vita has three usable 444 MHz Cortex-A9 cores, about 365 MiB of memory
for applications and a tile-based PowerVR SGX543MP4+. Singleplayer runs the
server inside the same process, so map generation, mods and rendering share
those cores. The defaults aim for a stable 30 FPS:

* **30 FPS locked to the display** (`vsync`, `vita_vblank_interval = 2`),
  which feels smoother than a framerate jumping between 30 and 60.
* **Viewing range 48**, small map caches on client and server, one emerge
  and one mesh generation thread.
* **No post processing, shadows, bloom, 3D clouds or waving**.
* **Opaque leaves and liquids**: alpha tested and blended geometry defeats
  the hidden surface removal of the PowerVR tile renderer.
* **Array texture emulation** (`emulate_array_textures`): node textures of the
  same size are packed into atlases, so most nodes share one material and get
  merged into the same mesh buffers. This cuts draw calls (the most
  expensive thing on the CPU side of vitaGL) dramatically. Requires
  mipmapping and texture filtering to stay off.
* **GPU-only map meshes** (`free_static_mesh_data`): the CPU copy of a
  mapblock mesh is freed once it has been uploaded, nearly halving the memory
  used by map geometry.
* **LuaJIT with its JIT compiler** (`vita_luajit_jit`), for mods and Lua
  map generators.

Things worth trying in *Settings → Advanced → PlayStation Vita* and the
graphics settings:

* `viewing_range`: the biggest single factor. 32 is very smooth, 64+ is
  heavy.
* `smooth_lighting`: turning it off speeds up mesh generation and produces
  fewer vertices.
* `vita_vblank_interval = 1` targets 60 FPS (only realistic with a small
  viewing range).
* `vita_overclock`: disable to save battery.

## Downloads

The `vita` GitHub Actions workflow (`.github/workflows/vita.yml`) builds the
VPK on every relevant push:

* **Development builds**: open the workflow run on the *Actions* tab and
  download the `luanti-vita` artifact (a zip containing `luanti-vita.vpk`).
* **Releases**: pushing a tag named `v*` or `vita-*` (for example
  `git tag vita-0.1 && git push origin vita-0.1`), or running the workflow
  manually with a release tag, publishes a GitHub release with the VPK
  attached.

## Building

The easiest way is the VitaSDK Docker image, which has every dependency:

    docker run --rm -v "$PWD:/src" -w /src vitasdk/vitasdk util/vita/build.sh

or, with a local VitaSDK (all packages installed with `vdpm`):

    VITASDK=/usr/local/vitasdk util/vita/build.sh [build dir]

The script

1. downloads the SQLite amalgamation (there is no vdpm package),
2. builds vitaGL from source with `HAVE_SHADER_CACHE=1` (persistent shader
   cache) and `DRAW_SPEEDHACK=2` (set `STOCK_VITAGL=1` to use the vdpm one),
3. configures with the VitaSDK toolchain file and builds `luanti.vpk`.

Manual CMake usage:

    cmake <source> -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake \
        -DSQLITE3_AMALGAMATION_DIR=<dir with sqlite3.c> \
        [-DVITA_VITAGL_DIR=<dir with a custom libvitaGL.a>] \
        [-DVITA_NEWLIB_HEAP_MB=224] [-DVITA_TITLEID=LUAN00001]

### Implementation notes

* `cmake/Modules/VitaLibs.cmake` sets platform defaults and dependency
  paths, `cmake/Modules/VitaPackage.cmake` links and packs the VPK.
* `src/porting_vita.cpp`: paths, clocks, network, memory layout, thread
  core assignment (renderer on core 0, server/emerge/mesh threads on cores
  1-2, light I/O threads may use the system core), system language, vitaGL
  configuration.
* `lib/sqlite_vita/`: SQLite VFS on top of `sceIo*` with a write-behind
  buffer and in-process locking (the unix VFS needs POSIX locks and mmap).
* Rendering goes through IrrlichtMt's OpenGL ES 2 driver on top of vitaGL;
  GLSL ES shaders are translated to Cg by vitaGL.
