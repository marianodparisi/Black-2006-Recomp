# BLACK Launcher

The player-facing launcher, in C++20 on [Walnut](https://github.com/StudioCherno/Walnut) (Vulkan + Dear ImGui).
It replaced the C# launcher and handles disc import, building the game, settings and play.
See [the plan](../docs/plans/new-launcher.md) for its design history.

It sets up the disc: an ISO (XISO or a full Redump-style dump) or a folder of extracted files, chosen with the
system's file dialog, dropped onto the window or given on the command line. The launcher reads Xbox disc images
itself (`src/core/Xdvdfs`), checks `default.xbe` before copying, and copies into `game/` with progress, resume and
cancel. It plays the game (`src/core/GameSession`: the `RECOMP_*` environment, the menu-language bank, and a
`reports/launcher-*` session with capped diagnostic logs), edits every setting the game reads, edits save profiles
(`src/core/Profile`: checksums and the HMAC-SHA1 signature, backups first) and prepares problem reports.

It builds the game (`src/core/Toolchain`: finds Visual Studio's C++ tools, CMake, Python and Rust, also where
their installers put them, and installs missing ones with winget; `src/core/GameBuild`: the stages, with progress
read from `generate.py`, MSBuild and Ninja, a log in `reports/build-*.log`, and stopping that ends the whole
process tree). On Linux the build runs `scripts/setup-linux.sh --game-only`, and missing tools are listed with the
command that installs them.

`BLACK Launcher --play` starts the game with the saved settings and no window, and waits for it: the shortcuts
(`Play BLACK.sh`) use it. `BLACK Launcher --build [--from-scratch]` builds the game in the terminal;
`scripts/setup.ps1` uses it.

**Add to Steam** discovers local accounts and creates a `--play` shortcut after
confirmation. Steam must be closed; existing shortcut bytes are preserved and
backed up before an atomic replacement. Malformed files and duplicate entries
are handled without rewriting the user's existing records.

Graphics settings include native LOD distance. Gameplay includes player bullet
damage, enemy spawn health and instant-kill headshots, shared with the in-game
menus through `local/video.ini`. HD texture setup can create/open the fixed
`texture_pack_hd` folder and reports a missing folder before launching.

## Command line

Use **BLACK Launcher.exe** on Windows or **BLACK Launcher** on Linux. Run
`--help` for the command list. With no arguments the launcher opens its menu.
A disc path on its own opens the graphical disc review.

From the extracted Linux starter, a frontend can run the following sequence.
On Windows, use the same arguments with `BLACK Launcher.exe`.

```sh
./'BLACK Launcher' --import '/media/discs/BLACK.iso'
./'BLACK Launcher' --build
./'BLACK Launcher' --settings
./'BLACK Launcher' --set FpsLimit=60 VSync=true MenuLanguage=en-US
./'BLACK Launcher' --play --fullscreen --no-dialogs
```

`--import` accepts an ISO/XISO or extracted disc directory. It checks the
supported XBE and the required `GlobData.bin`, `levels/`, `sound/`, and `videos/`
entries before copying. Rerunning the command resumes the copy into `game/`. Once built,
later launches only need `--play`. It waits for the game and returns its exit
code. `--no-dialogs` sends launch errors to stderr. `--fullscreen` selects
borderless fullscreen for that session and preserves the saved window mode.

`--set` saves one or more assignments using the case-sensitive names printed
by `--settings`. Pass strings as plain text, booleans as `true` or `false`, and
numbers in decimal. `Bindings` accepts a JSON object of keyboard overrides.
The launcher serializes simultaneous `--set` commands for the same project and
validates the whole batch before saving. To restore a binding, include its
default keys in the bindings object. `SettingsVersion` is
read-only.

The default project is the starter beside the executable, or the source tree
containing a development build. To select another extracted starter, put
`--project-dir DIRECTORY` before the command. This selects the build sources,
disc, build outputs, settings, saves and reports together. Relative disc paths
are resolved from the calling directory. On Linux the launcher uses the
project's private Wine or Proton prefix by default. The play shortcut also accepts
`./'Play BLACK.sh' --project-dir DIRECTORY --no-dialogs`. System compilers, graphics
drivers and Wine are host dependencies.

## Building

Starter archives include the compiled launcher and its license notices. To build
or change it, clone the [repository](https://github.com/portingpete/Black-2006-Recomp)
with `--recursive`; the starter does not include the full launcher sources.

Needs CMake 3.21+ and a C++20 compiler; nothing else. The Vulkan headers are a submodule
(`vendor/vulkan-headers`), and the launcher links against the Vulkan loader that graphics drivers install: on
Windows through an import library made at configure time. `-DBLACK_LAUNCHER_VULKAN_SDK=ON` builds against an
installed SDK instead.

```powershell
git submodule update --init --recursive
cd launcher
cmake --preset windows-release
cmake --build --preset windows-release
ctest --preset windows-release
```

On Linux, use GCC 13 or newer with C++20 `std::format` support (Ubuntu 24.04:
`sudo apt install build-essential cmake ninja-build libx11-dev libxrandr-dev libxinerama-dev
libxcursor-dev libxi-dev libwayland-dev wayland-protocols libxkbcommon-dev`, plus a graphics driver with Vulkan),
use the `linux-release` presets. The launcher is written to `build-launcher*/bin/` and finds the project folder
by searching upwards from itself, so it runs from the build folder too. `scripts/build-launcher.ps1` and
`scripts/setup-linux.sh` build it and copy it to the project folder.

## Layout

```
launcher/
  src/core/   the logic, with no UI: project layout, disc import (XDVDFS), settings and key bindings, playing
              the game, save profiles, problem reports, the game's menu fonts. Unit-tested.
  src/app/    the Walnut application: the pages (disc, settings, saves), the game-style widgets, key capture,
              translations, the theme
  tests/      doctest unit tests for src/core; no game files, GPU or window needed
  assets/     the logo, stand-in fonts (SIL Open Font License) and translations, built into the executable
  vendor/     submodules: walnut, imgui, glfw, glm, spdlog, json, vulkan-headers, doctest
```

## The look

The launcher is drawn like the game's own menu pages: the colours are sampled from them, rows are green-grey with
the selected one bright and its selector line, the rails run down the left, and prompts sit along the bottom. The
banner shows BLACK's logo (`assets/images/black-logo.png`, built in; a `local/logo.png` replaces it). Once the
player's disc files are in place, text uses the game's own `Small`/`Big` menu fonts, read from `language/fonts` at
run time and never shipped. Until then the closest free fonts stand in: Russo One for rows and help, Saira
Condensed ExtraBold for titles.

## Translations

All text a player sees goes through `tr()` / `trf()` (`src/app/Translations.h`), looked up by its English in
`assets/translations/<language>.json`. `python scripts/check-launcher-translations.py` lists text with no
translation (CI runs it); `--update` adds new text to every catalog for translators to fill in. The game's fonts
have no A- or O-tilde, so `MenuFont` draws those with the tilde of the font's own N-tilde.

## Walnut

`vendor/walnut` is [PauloVx/Walnut](https://github.com/PauloVx/Walnut), branch `black`: Walnut's `dev` branch, which
has the custom titlebar, plus small changes for embedding it. These include no submodules of its own, opt-in file
logs, application fonts, titlebar colour and icon, and keeping the system frame on Linux where TheCherno's GLFW fork
has no custom titlebar. Change Walnut there, then commit the new submodule commit here.

## Code style

- C++20, warnings at `/W4` / `-Wall -Wextra -Wpedantic -Wconversion` and more. CI treats them as errors
  (`*-ci` presets).
- `launcher/.clang-format` and `launcher/.clang-tidy` apply to `src/` and `tests/`. Run
  `python scripts/check-launcher.py --fix` before committing. `--tidy <build folder>` runs clang-tidy on a Ninja
  build (the Linux presets).
- Logic goes in `src/core` with tests. `src/app` only draws and wires actions. Failures that a player can hit
  return `core::Result<T>` with a message they can read. Resources are owned by RAII types, with no raw `new`
  except where Walnut's API needs it.
