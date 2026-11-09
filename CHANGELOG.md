# Changelog

## Unreleased

**SFX and music volumes are kept.** The game's own save never brought them back, so they returned to full at every
start. They are now kept in `local/audio.ini`, saved when changed in either sound menu and restored at startup, with
or without a saved profile.

**Sprint and jump (optional).** Hold Sprint (Left Shift) while moving to run faster, and press Jump (Space) on the
ground to jump. Both are off by default; turn them on in the launcher's Gameplay settings and rebind the keys in its
Controls. Keyboard and mouse only, for player one.

**Gameplay subtitles.** Mission dialogue, allies and radio transmissions are captioned in game, in English and
Brazilian Portuguese (`subtitles/<language>/gameplay.tsv`; see `subtitles/README-gameplay.md` to add a language).
The launcher's **SUBTITLES** setting turns all subtitles, cutscenes included, on or off; it is on by default.

**FREE camera range in Photo Mode.** After 30 M, the camera range offers FREE, which lifts the distance and height
limits around where Photo Mode started.

**Shader precompilation can be turned off.** Graphics settings have **PRECOMPILE SHADERS AT STARTUP**, on by default.
Off starts the game sooner after a rebuild or a cleared cache, at the cost of shader hitches while playing.

**Fewer audio-lock stalls.** The APU interrupt no longer waits for the audio frame
lock at interrupt level, which removes long freezes and FPS drops on low-power CPUs.

**Less CPU while waiting.** Under Wine and Proton, the main thread sleeps while it waits for pushbuffer space,
instead of spinning for about half of each frame, and so do the GPU pusher, GPU queue and acknowledgement threads when
idle. Windows keeps the spin until this is measured there (`RECOMP_IDLE_WAIT=1` turns it on).

## 0.1.0-alpha.5 — 2026-10-22

**First C++ BLACK Launcher release on portingpete/Black-2006-Recomp.** The starter
includes the Windows and native Linux launchers. Import your supported Xbox disc,
choose **Build the game**, then **Play**. A new quick start and `START HERE.txt`
explain the complete flow, distinguish starter archives from source downloads,
and correct the source-clone instructions. Release packaging now reads `VERSION`
by default so archive names follow the game and launcher version.
The Windows launcher includes its C++ runtime, so it can open before build tools
are installed. Bundled font and library notices are preserved in the starter.

**LOD and combat tuning.** Graphics settings can extend native mesh LOD and prop visibility
from 1X to 10X. Gameplay settings add player bullet damage (0.5X–10X), enemy spawn
health (0.25X–4X), and headshot instant kills. Defaults preserve original behavior;
restart a mission after changing enemy health. Armor, invulnerability and native
death/headshot handling are retained. Both launcher and in-game menus save these options.

**Add to Steam.** The launcher can add a shortcut for the built game to a selected
local Steam account, with explicit confirmation, backups, duplicate detection,
and a requirement to close Steam first. The HD texture setup now has a folder-status
row, a create/open-folder action and a clear error when the folder is missing.

**Optional secondary objectives.** A new toggle in the launcher and in-game
Gameplay settings removes secondary-objective requirements for mission
completion. It defaults to Off; primary objectives and earned secondary
objective progress are preserved.

**Ray-traced bounce light on walls and floors.** With ray tracing on, the level takes on the colour of what is
around it, followed over 4 bounces, and gets contact shading where surfaces meet, finer than the baked lightmaps;
the brightness stays the bake's. Characters and props get their bounce colour over 4 bounces too, twice as strong as
before. About 2.7 ms more a frame on an RTX 3060 Ti at 1440p (DLAA).

## 0.1.0-alpha.4 — 2026-10-17

**HD textures.** The game can use [BLACK HD Reimagined](https://www.moddb.com/mods/black-hd-reimagined/addons/black-hd-reimagined-texture-pack),
the PS2 texture pack by HD REIMAGINED: extract it into a `texture_pack_hd` folder next to the launcher and choose
**Settings > Game > Textures: HD PACK** (**ORIGINAL** goes back). The pack's files are named by PS2 hashes, so every
texture on the Xbox disc was matched to the pack by look and the pairs checked by eye; `textures/black-hd-reimagined.map`
keeps the result, and the game finds each replacement by the texture's content. 1161 game textures use 1007 of the
pack's 1324 files, with their mipmaps; the PS2's half-scale alpha is doubled. The pack is not included. Without the
map, the game matches by look as it plays.

**Two players, split screen (experimental).** **TWO PLAYERS (EXPERIMENTAL)**, chosen before the difficulty, starts a
mission for two on one screen, top and bottom, player two on the second controller or the arrows and the numpad: each has their own weapons, ammunition,
pickups, health packs, HUD, crosshair and hit markers, and every effect is drawn in both views. Enemies see, hear and
shoot at both players. Unfinished: enemies sometimes don't engage the second player.

**Versions.** The game and the launcher take their version from the repository's `VERSION` file; the launcher's main
menu and the game's log show it.

**Debug overlay.** Insert shows a panel in the pause menu's style over the top-left corner: frame rate, frame times
(average and worst), the game's own frame rate, GPU time and draws, latency (from the game starting a frame to the
screen), the resolution drawn and shown, anti-aliasing, ray tracing, CPU, memory and graphics memory, and the
player's position and speed. Insert again hides it; `RECOMP_DEBUG_OVERLAY=1` shows it from the start.

## 0.1.0-alpha.3 — 2026-10-16

**DLSS and DLAA.** On an NVIDIA RTX graphics card, anti-aliasing gains DLAA (DLSS at the internal resolution) and
DLSS Quality, Balanced and Performance, which draw the world at 2/3, 0.58 or half the width and height and have DLSS
rebuild the internal resolution before the HUD, which stays sharp. With ray tracing on, the traced passes then run at
the smaller size too. `scripts\fetch-dlss.ps1` (run by `setup.ps1`) fetches NVIDIA's runtime and headers into
`third_party\dlss`; without them, or without an RTX card, these modes fall back to FXAA.

**Crosshair and hit markers.** A crosshair in place of the game's dot (a cross, a T or a circle, on the Display page)
opens with every shot and as you move, and closes again when you stop; aiming down the sights hides it and brings the
dot back. A white mark flashes on every shot that wounds an enemy or knocks the weapon out of their hands, and a red
one when it kills. Neither is ever in a photo.

**Ray tracing (experimental).** With a graphics card that has DirectX Raytracing (NVIDIA RTX, AMD RX 6000 or newer),
**Ray tracing** in the Graphics settings (in game and in the launcher) uses the GPU's ray tracing cores for shadows
cast by characters and props, ambient occlusion and bounce light around them, and reflections of the whole scene,
off screen too, on weapons and glass (with Reflections above Original). It is off by default and Windows only. It
starts with the game, so turning it on in a session that started without it applies at the next start. About 3 ms a
frame on an RTX 3060 Ti at 1440p.

**The launcher sets up your disc.** Choose an ISO (XISO or a full Redump-style dump) or a folder, or drop
it onto the window: the launcher reads the Xbox disc itself, checks it is the supported release before copying, and
copies it into `game/` with a progress bar, percentage, current file, speed and time left. Copying can be stopped
and carries on where it left off; saves in the game folder are kept.

**The launcher builds the game.** **Build the game** finds the tools the build needs (Visual Studio's C++
tools, CMake, Python and Rust, also where their installers put them), installs missing ones with winget, then
prepares Python, translates the code from your disc, sets up the build, compiles and makes the menu languages,
with each stage's progress, the elapsed time and the current output line. Stopping ends everything the build
started; building again carries on, and code that is still current is not translated again. A failure shows the
step and the last lines of output, and every build is logged in `reports/`. `scripts/setup.ps1` now just builds
the launcher and runs `BLACK Launcher.exe --build`.

**BLACK Launcher replaces BLACK PC Launcher and BLACK PC Settings.** One launcher, in C++ on Walnut
(Vulkan + Dear ImGui) and drawn like the game's own menu pages, now does everything the C# launchers did:

- **Play** starts the game with the same environment, menu-language bank and `reports/launcher-*` session
  logs (capped diagnostic traces, `launch.json`, `exit.json`, the kernel log); the same row stops it.
  `BLACK Launcher --play` plays with no window, for shortcuts.
- **Settings** pages for display, graphics, controls and game, saved at once to the files the game reads.
  Key bindings are read by key position, like the game stores them, with three keys per action.
- **Diagnostics**: the visual, DSP and GPU draw traces, the logs folder, and a problem report that opens a
  pre-filled GitHub issue without your folder names.
- **Save game**: rename a profile, open or lock missions and set their results, unlock them all, and put
  any backup back. Profiles are re-signed as the game does and backed up before every change.
- All of it in the six menu languages. The game's fonts lack A- and O-tilde, so the launcher draws them
  with the tilde of the font's N-tilde.

Setup builds the launcher first, because that is where the disc is set up; it needs only CMake and Visual
Studio (no Vulkan SDK: the headers are a submodule). On Linux the launcher is native and starts the game
through Wine/Proton itself; Mono and .NET in Wine are no longer needed. The C# sources, their catalogs and
tests are gone; release archives carry `BLACK Launcher.exe` and the Linux `BLACK Launcher`.

**XboxRecomp is now a submodule.** `third_party/xboxrecomp` is PauloVx/xboxrecomp on its
`black` branch, where the runtime changes are ordinary commits; `patches/xboxrecomp.patch` and
`patches/xboxrecomp-clang.patch` are gone. The DSP56300 core moved from `.work/err009-dsp56300` to
the `third_party/dsp56300` submodule. Clone with `--recursive`; setup fetches missing submodules,
and release archives include both sources.

Vertex shader compiles no longer cause hitches when a level first appears. The
shader cache is keyed by the generated HLSL alone, so one program is compiled once
instead of again for each copy left in program memory (113 cached files were 18
programs). Each program's microcode is recorded in `vs_manifest.bin` in the cache,
and launch compiles any recorded program the cache lacks on all cores before the
game starts (`RECOMP_KGPU_PRECOMPILE=0` skips it). A program first met in play runs
a quick unoptimised build, about twice as fast, until its optimised build is cached
(`RECOMP_KGPU_VS_QUICK=0` disables it). Existing caches carry over.

**Options > CONTROLS** rebinds the keyboard, mouse and controller in game,
replacing the console's Controller Setup page. Keyboard and mouse actions take
a primary and a secondary key; the controller picks one of the four original
button layouts and moves single button actions, swapping with an action that
already had the button. Changes write the game's own action map, so prompts
show the new keys and button art at once. Bindings live in
`local/controls.ini`, shared with the launcher. Unedited `menus.ini` files
upgrade to the new pages automatically.

**Photo mode** (Pause > PHOTO MODE): a free camera over the paused mission with field of view,
roll, speed and range, controlled by keyboard and mouse or controller from a panel in the game's
menu style. Photos are saved as PNG to `Pictures\BLACK` without the HUD, menus or weapon. Game blur
effects are off while it is open; scripted cameras cannot be entered. Unedited `menus.ini` files
gain the new pause item automatically.

**Reflections**: with the GPU renderer, screen-space reflections replace the
game's low-resolution 128 x 128 environment maps on the surfaces that use them,
mostly the first-person weapons and some windows. Rays that miss use a blurred
copy of the screen instead of the baked map. Night scenes are brightened to match
the original's overall level. A new Reflections row (Original, Low, Medium, High,
Ultra; default High) is on the launcher's Graphics tab and the in-game
Video Settings > Graphics page. `RECOMP_REFLECTIONS` pins it. See
[docs/reflections.md](docs/reflections.md).

## 0.1.0-alpha.2 — 2026-10-14

**Build BLACK.sh** now generates and compiles the game entirely on x86_64 Linux
without a Windows installation. The build uses native Linux Python, LLVM,
Rust and Mono, with a pinned Microsoft SDK/CRT downloaded by xwin. It outputs the
Windows x64 game executable for Wine/Proton and builds both host launchers.

Added the CMake cross toolchain, compiler compatibility fixes, local language-bank
generation, build provenance, and an asset-free Linux runtime/Rust ABI CI check.
Retail files and generated game code remain local. Windows building stays supported.

## 0.1.0-alpha.1 — 2026-10-14

First public starter release. Includes compiled one-click **BLACK PC Launcher.exe**
and separate **BLACK PC Settings.exe**, Windows build shortcut, Linux Wine/Proton
entry points, host source, licenses, and archive/file SHA-256 checksums.

Portable folders may keep the locally generated game executable in
`bin/black_xbox_recomp.exe`; existing `build-vs2022/Release/` projects also work.
Play uses saved preferences and settings opens separately. Both share saves,
language selection and diagnostics. The latest audio completion freeze,
weapon pickups, save/profile, ambient occlusion and localized bank fixes are included.

The public starter requires your own supported Xbox files and a one-time Windows
build. Game executables, retail data, generated game code, language banks and saves
are excluded. Linux runs the resulting Windows build through Wine/Proton.
This remains experimental; full later-mission playthroughs are unverified.
