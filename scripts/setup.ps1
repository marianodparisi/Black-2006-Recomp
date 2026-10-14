[CmdletBinding()]
param([switch]$SkipBuild)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$toolkit = Join-Path $projectRoot 'third_party\xboxrecomp'
$patch = Join-Path $projectRoot 'patches\xboxrecomp.patch'
$python = Join-Path $projectRoot '.venv\Scripts\python.exe'
$dsp56300 = Join-Path $projectRoot '.work\err009-dsp56300'
$upstreamCommit = '766ecefcd7fb2a9b344de8ec891f6fe9ea14261b'
$dsp56300Commit = 'aab3649fc452daf03da8b5e36fdc0c308d874101'
$xbe = Join-Path $projectRoot 'game\default.xbe'
# Supported retail releases; scripts\black_release.py describes their layouts.
$supportedXbe = @{
    'DF2739C372D254A90AEECCF5971D12097F22D89FBB90DB021FDA96DD4DEB68F6' = 'NTSC'
    '67F35121CCF622E7A293C402219596FE6456CA4C3D27E6F8139E449264DA30F0' = 'PAL'
}

function Invoke-Checked([string]$Program, [string[]]$Arguments) {
    $previousPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $Program @Arguments
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousPreference
    }
    if ($exitCode -ne 0) { throw "$Program failed with exit code $exitCode" }
}

if (-not (Test-Path -LiteralPath $xbe -PathType Leaf)) {
    throw 'Copy your extracted original Xbox BLACK files into game\ first (including default.xbe).'
}
$xbeHash = (Get-FileHash -LiteralPath $xbe -Algorithm SHA256).Hash
if (-not $supportedXbe.ContainsKey($xbeHash)) {
    throw 'default.xbe does not match a supported retail Xbox release; see README.md.'
}
Write-Output "Found the $($supportedXbe[$xbeHash]) release of BLACK."
if (-not (Get-Command git -ErrorAction SilentlyContinue)) { throw 'Git must be installed and available on PATH.' }
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) { throw 'CMake 3.20 or later must be installed and available on PATH.' }

if (-not (Test-Path -LiteralPath $toolkit)) {
    $null = New-Item -ItemType Directory -Path (Split-Path -Parent $toolkit) -Force
    Invoke-Checked git @('clone', 'https://github.com/sp00nznet/xboxrecomp.git', $toolkit)
    Invoke-Checked git @('-C', $toolkit, 'checkout', '--detach', $upstreamCommit)
}
$actualCommit = (& git -C $toolkit rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $actualCommit -ne $upstreamCommit) {
    throw 'third_party\xboxrecomp must be at the pinned upstream commit. Preserve another checkout separately before retrying.'
}
function Test-Patch([switch]$Reverse) {
    # Windows PowerShell 5 treats native stderr as an error record even when
    # redirected. A failed check is expected for an already-applied patch.
    $previousPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        if ($Reverse) { & git -C $toolkit apply --reverse --check $patch 2>$null }
        else { & git -C $toolkit apply --check $patch 2>$null }
        return $LASTEXITCODE -eq 0
    } finally {
        $ErrorActionPreference = $previousPreference
    }
}
if (Test-Patch) {
    Invoke-Checked git @('-C', $toolkit, 'apply', $patch)
} else {
    if (-not (Test-Patch -Reverse)) { throw 'Runtime patch conflicts with this dependency checkout; preserve local edits before retrying.' }
}

if (-not (Test-Path -LiteralPath $dsp56300)) {
    $null = New-Item -ItemType Directory -Path (Split-Path -Parent $dsp56300) -Force
    Invoke-Checked git @('clone', '--depth', '1', '--branch', 'v1.0.0', 'https://github.com/mborgerson/dsp56300.git', $dsp56300)
}
$actualDspCommit = (& git -C $dsp56300 rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $actualDspCommit -ne $dsp56300Commit) {
    throw 'The DSP56300 dependency must be at the pinned v1.0.0 commit. Preserve another checkout separately before retrying.'
}
if (-not (Get-Command cargo -ErrorAction SilentlyContinue) -or
    -not (Get-Command rustup -ErrorAction SilentlyContinue)) {
    throw 'Rustup and Cargo are required to build the DSP core. Install the x86_64-pc-windows-msvc toolchain and rerun setup.'
}

if (-not (Test-Path -LiteralPath $python)) {
    if (Get-Command py -ErrorAction SilentlyContinue) {
        Invoke-Checked py @('-3', '-m', 'venv', (Join-Path $projectRoot '.venv'))
    } elseif (Get-Command python -ErrorAction SilentlyContinue) {
        Invoke-Checked python @('-m', 'venv', (Join-Path $projectRoot '.venv'))
    } else {
        throw 'Python 3.10 or later must be installed and available as py or python.'
    }
}
Invoke-Checked $python @('-m', 'pip', 'install', 'capstone==5.0.7')
$env:PYTHONUTF8 = '1'
Invoke-Checked $python @((Join-Path $PSScriptRoot 'generate.py'))

if (-not $SkipBuild) {
    $build = Join-Path $projectRoot 'build-vs2022'
    Invoke-Checked cmake @('-S', $projectRoot, '-B', $build, '-G', 'Visual Studio 17 2022', '-A', 'x64')
    Invoke-Checked cmake @('--build', $build, '--config', 'Release', '--target', 'black_xbox_recomp')
    & (Join-Path $PSScriptRoot 'build-launcher.ps1')
    if ($LASTEXITCODE -ne 0) { throw 'Launcher compilation failed.' }
    Write-Output "Ready: $(Join-Path $projectRoot 'BLACK PC Launcher.exe')"
}
