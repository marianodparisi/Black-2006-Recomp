# Linux with Wine or Proton

This release runs the Windows game and launcher on **x86_64 Linux through Wine
or Proton**. It needs an AVX2 CPU. The Direct3D 11 renderer is translated by Wine
or DXVK; this is a compatibility build. The launcher uses .NET Framework 4 APIs.

## Build entirely on Linux

The starter contains both compiled host launchers and all host source. Supply your
supported original Xbox disc files in `game/`. No Windows installation, VM,
PowerShell, Visual Studio, or files copied from Windows are required to build.

On Ubuntu 24.04, install native Linux development packages:

```bash
sudo apt-get update
sudo apt-get install -y build-essential clang-18 clang-tools-18 llvm-18 lld-18 \
  mono-devel python3-venv cmake ninja-build git ca-certificates
```

Extract the complete release onto a writable, case-sensitive Linux filesystem,
then open **Build BLACK.sh**, or run:

```bash
cd '/path/to/BLACK-0.1.0-alpha.2'
bash './Build BLACK.sh'
```

The build downloads pinned XboxRecomp and DSP56300 source, native Linux Rust
1.98.1, and xwin 0.10.0. xwin obtains Microsoft SDK 10.0.26100 and CRT
14.44.17.14 headers/import libraries directly from Microsoft; their licenses
apply and the build accepts the SDK license. These are build inputs, not a
Windows installation. SDK case aliases require a case-sensitive filesystem;
for WSL, use its Linux home directory rather than `/mnt/c` or another Windows drive.

Linux Python translates your own XBE, Linux clang-cl/LLD compile/link the game
and Rust audio core, and Linux Mono compiles both .NET host launchers. Wine is
not used during generation or compilation. All tools/dependencies stay in
`.work/linux-toolchain/`, `third_party/`, and `.venv/`; the script does not alter
your shell profile or install system packages. It writes
`bin/black_xbox_recomp.exe` and both launchers into the release folder. Languages
are built locally from your original English bank. No game code or bank binaries
are distributed.

The first build may take several minutes and needs network access and several
GB of free disk space. Two compiler jobs are used by default; set
`BLACK_BUILD_JOBS=4` if you have enough memory. `bash scripts/setup-linux.sh
--skip-generate` rebuilds existing locally generated code. The private
`build-linux-wine/linux-build-provenance.json` records tool/input/output hashes.

## Move an existing Windows build (optional)

You can also use **Build BLACK.cmd** on Windows and copy that folder to Linux. Keep
`build-vs2022/Release/black_xbox_recomp.exe`, or put that executable in
`bin/black_xbox_recomp.exe`. Keep `game/`, `subtitles/`, the two `.exe` launchers,
`black-release.json`, root `.sh` files and `scripts/linux/` together. Also copy
`local/language-banks/` if you generated additional languages. You may omit the
other build intermediates and dependency checkouts after building.

## Wine

Install Wine using your distribution's instructions. Its 64-bit Windows runtime,
Wine Mono or Microsoft .NET Framework 4.8, and graphics drivers must work. Both
launch scripts use the same private prefix at `<release folder>/.wine`; they do
not use the default `~/.wine` prefix. If .NET support is missing, install it into
this exact prefix. For example, with Winetricks installed:

```bash
cd '/path/to/BLACK-0.1.0-alpha.2'
WINEPREFIX="$PWD/.wine" winetricks dotnet48
```

For Vulkan-capable GPUs, DXVK provides Direct3D 11 acceleration. Install it into
the same prefix using your distribution's tooling or `WINEPREFIX="$PWD/.wine"
winetricks dxvk`. Wine's built-in renderer can also be used, with performance and
feature differences. Setup may require network access and additional 32-bit Wine
packages. The launch scripts never install packages or download dependencies.

Install the native **d3dcompiler_47** into the same prefix for Kelvin GPU shaders:

```bash
WINEPREFIX="$PWD/.wine" winetricks d3dcompiler_47
```

Wine 9.0's built-in shader compiler rejected a Kelvin shader during validation
and the game fell back to its CPU renderer. The native compiler enabled the
Kelvin GPU backend. Winetricks normally sets its native override; if needed,
run with `WINEDLLOVERRIDES=d3dcompiler_47=n,b`. This DLL is not bundled.

Open **Play BLACK.sh** to play. Open **BLACK Settings.sh** to change settings.
Archive extraction preserves executable permissions; if your file manager needs
them restored, run:

```bash
chmod +x ./*.sh
./'Play BLACK.sh' --check
./'BLACK Settings.sh'
./'Play BLACK.sh'
```

File managers vary in how they open executable scripts. **Install Linux Shortcuts.sh**
creates **Play BLACK.desktop** and **BLACK Settings.desktop** in this folder using
Python 3. Allow launching/trust these files if your desktop asks. The shortcuts use
absolute paths, so rerun the installer after moving the folder. Scripts locate
their own folder and work when launched from another working directory.

`BLACK_WINE=/absolute/path/to/wine` selects another Wine binary and
`BLACK_WINEPREFIX=/absolute/path/to/prefix` selects an existing dedicated prefix.
Set the same values for both play and settings.

## Steam Proton

Add **BLACK PC Launcher.exe** as a non-Steam game, set **Start In** to the release
folder, and force a Proton compatibility tool. Install the launcher's .NET support
in that entry's prefix if needed. Change the same Steam entry's launch options to
`--settings` to open settings in the same prefix; remove the option to play.
Using separate Steam entries normally creates separate prefixes.

Advanced users can select an existing Proton script from the shell, using the
same environment for both launchers:

```bash
export BLACK_PROTON='/path/to/Steam/steamapps/common/Proton Experimental/proton'
export STEAM_COMPAT_CLIENT_INSTALL_PATH='/path/to/Steam'
./'BLACK Settings.sh'
./'Play BLACK.sh'
```

The script uses `<release folder>/.proton` for both entry points.
`BLACK_COMPAT_DATA_PATH=/absolute/path` overrides that location.
The Wine and Proton routes use separate prefixes; pick one for normal play.

## Saves, diagnostics and limitations

Settings and shader cache use `local/`. Native saves use `save/`, `game/UDATA/`
and `game/TDATA/`. Back up all three save locations with the game closed.
Do not remove the release folder when it contains progress you want to keep.
Session logs are in `reports/launcher-*/`; a failed `--check` creates no session.

Validation generated and built the complete game and both launchers with Linux
tools on Ubuntu 24.04. The resulting game was tested under WSLg with Wine 9.0,
Wine Mono 9.0.0 and a fresh dedicated prefix. The native shader compiler was
downloaded and extracted on Linux; it enabled the Kelvin Direct3D 11 backend.
The game played opening movies, reached the title and first mission, and responded
to movement, camera, fire and reload input. The separate settings entry point
opened its live window. Tests used external timeouts and the remaining game child
was explicitly stopped afterward. WSL exposed software graphics, so this does not
establish hardware GPU performance or full-mission stability on Linux. Audio was
not assessed. Proton launch routing and shared paths are covered by automated
tests; a real Proton playthrough has not been tested.

The output targets Windows x64 and runs through Wine/Proton. A Linux-hosted build
does not replace the game's Windows APIs with a native Linux renderer. Audio,
controllers, GPU drivers and fullscreen behavior can differ among Wine/Proton
versions. Later missions, Steam Deck and Proton versions are not automatically
validated by launcher tests.

Upstream documentation: [Wine](https://www.winehq.org/documentation),
[Proton](https://github.com/ValveSoftware/Proton),
[DXVK](https://github.com/doitsujin/dxvk),
[Winetricks](https://github.com/Winetricks/winetricks).
