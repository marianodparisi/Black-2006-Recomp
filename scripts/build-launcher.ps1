[CmdletBinding()]
param([switch]$Launch, [switch]$Force, [string]$OutputDirectory)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$source = Join-Path $PSScriptRoot 'launcher.cs'
$saveTabSource = Join-Path $PSScriptRoot 'save-profile-tab.cs'
$profileEditorSource = Join-Path $PSScriptRoot 'profile_editor.cs'
$languageSource = Join-Path $PSScriptRoot 'game-language.cs'
$uiLanguageSource = Join-Path $PSScriptRoot 'launcher-language.cs'
$localeCatalog = Join-Path $PSScriptRoot 'localization\launcher-locales.json'
$launcherKeys = Join-Path $projectRoot 'localization\launcher\en-pt-BR.jsonl'
$launcherCatalogDir = Join-Path $projectRoot 'localization\launcher'
$catalogBuilder = Join-Path $PSScriptRoot 'localization\build_launcher_catalog.py'
$bankBuilder = Join-Path $PSScriptRoot 'localization\build_mainus.py'
$ptBrBankBuilder = Join-Path $PSScriptRoot 'localization\build_pt_br_bank.py'
$bankBootstrap = Join-Path $PSScriptRoot 'localization\prepare_english_bank.ps1'
$englishBank = Join-Path $projectRoot 'local\language-banks\en-US\MainUS.bin'
$ptBrSource = Join-Path $projectRoot 'translations\pt-BR\MainUS.json'
$gameCatalogDir = Join-Path $projectRoot 'localization\game'
if (-not $OutputDirectory) { $OutputDirectory = $projectRoot }
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$output = Join-Path $OutputDirectory 'BLACK PC Launcher.exe'
$settingsOutput = Join-Path $OutputDirectory 'BLACK PC Settings.exe'
$compiler = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
if (-not (Test-Path -LiteralPath $compiler)) { throw 'The .NET Framework 4 compiler is missing.' }
if (-not (Test-Path -LiteralPath $saveTabSource)) { throw 'The Save tab source is missing.' }
if (-not (Test-Path -LiteralPath $profileEditorSource)) { throw 'The Profile editor source is missing.' }
if (-not (Test-Path -LiteralPath $languageSource)) { throw 'The launcher language-bank source is missing.' }
if (-not (Test-Path -LiteralPath $uiLanguageSource)) { throw 'The launcher UI language source is missing.' }
if (-not (Test-Path -LiteralPath $localeCatalog)) { throw 'The generated launcher locale catalog is missing.' }

$python = $null
$pythonArgs = @()
$pythonLauncher = Get-Command py -ErrorAction SilentlyContinue
if ($pythonLauncher) {
    $pythonVersion = & $pythonLauncher.Source -3 --version 2>&1
    if ($LASTEXITCODE -eq 0 -and ($pythonVersion -join ' ') -match 'Python 3\.') {
        $python = $pythonLauncher
        $pythonArgs = @('-3')
    }
}
if (-not $python) {
    $pythonCandidate = Get-Command python -ErrorAction SilentlyContinue
    if ($pythonCandidate) {
        $pythonVersion = & $pythonCandidate.Source --version 2>&1
        if ($LASTEXITCODE -eq 0 -and ($pythonVersion -join ' ') -match 'Python 3\.') { $python = $pythonCandidate }
    }
}
if ($python) {
    if ((Test-Path -LiteralPath $launcherKeys) -and (Test-Path -LiteralPath $catalogBuilder)) {
        & $python.Source @pythonArgs $catalogBuilder $launcherKeys $launcherCatalogDir $localeCatalog
        if ($LASTEXITCODE -ne 0) { throw 'Could not generate launcher locale catalogs.' }
    }
    # Setup builds the launcher before its first run. Preserve the retail source
    # now so newly offered locales can be generated on that first setup too.
    . $bankBootstrap
    Initialize-EnglishLanguageBank -InstalledBank (Join-Path $projectRoot 'game\language\strings\MainUS.bin') -EnglishBank $englishBank
    if (Test-Path -LiteralPath $englishBank) {
        if ((Test-Path -LiteralPath $ptBrSource) -and (Test-Path -LiteralPath $ptBrBankBuilder)) {
            $ptBrBank = Join-Path $projectRoot 'local\language-banks\pt-BR\MainUS.bin'
            # The bank is small; regenerate it every time to avoid stale output
            # when source timestamps are preserved or copied between builds.
            & $python.Source @pythonArgs $ptBrBankBuilder $englishBank $ptBrSource $ptBrBank
            if ($LASTEXITCODE -ne 0) { throw 'Could not generate the Brazilian Portuguese MainUS bank.' }
        } else {
            Write-Output 'Skipped the Brazilian Portuguese game bank: its translation source or builder is missing.'
        }
        foreach ($locale in @('es', 'fr', 'de', 'it')) {
            $catalog = Join-Path $gameCatalogDir ($locale + '.jsonl')
            $bank = Join-Path $projectRoot ('local\language-banks\' + $locale + '\MainUS.bin')
            if (-not (Test-Path -LiteralPath $catalog)) { throw "The $locale game catalog is missing." }
            # Regenerate these small banks every build, as for PT-BR. Builder
            # fixes must also replace cached banks with unchanged input timestamps.
            & $python.Source @pythonArgs $bankBuilder $englishBank $catalog $bank
            if ($LASTEXITCODE -ne 0) { throw "Could not generate the $locale MainUS bank." }
        }
    } else {
        Write-Output 'Skipped generated game-language banks: provide local\language-banks\en-US\MainUS.bin to build them.'
    }
} else {
    Write-Output 'Python is unavailable; using the checked-in launcher catalog and skipping generated game-language banks.'
}

foreach ($target in @(
    @{ Path = $output; EntryPoint = 'BlackXboxLauncher.Program' },
    @{ Path = $settingsOutput; EntryPoint = 'BlackXboxLauncher.SettingsProgram' }
)) {
    $targetPath = $target.Path
    if ($Force -or -not (Test-Path -LiteralPath $targetPath) -or
        (Get-Item -LiteralPath $source).LastWriteTimeUtc -gt (Get-Item -LiteralPath $targetPath).LastWriteTimeUtc -or
        (Get-Item -LiteralPath $saveTabSource).LastWriteTimeUtc -gt (Get-Item -LiteralPath $targetPath).LastWriteTimeUtc -or
        (Get-Item -LiteralPath $profileEditorSource).LastWriteTimeUtc -gt (Get-Item -LiteralPath $targetPath).LastWriteTimeUtc -or
        (Get-Item -LiteralPath $languageSource).LastWriteTimeUtc -gt (Get-Item -LiteralPath $targetPath).LastWriteTimeUtc -or
        (Get-Item -LiteralPath $uiLanguageSource).LastWriteTimeUtc -gt (Get-Item -LiteralPath $targetPath).LastWriteTimeUtc -or
        (Get-Item -LiteralPath $localeCatalog).LastWriteTimeUtc -gt (Get-Item -LiteralPath $targetPath).LastWriteTimeUtc) {
        & $compiler /nologo /target:winexe /platform:x64 /optimize+ "/main:$($target.EntryPoint)" `
            /reference:System.Windows.Forms.dll /reference:System.Drawing.dll `
            /reference:System.Web.Extensions.dll "/resource:$localeCatalog,BlackXboxLauncher.launcher-locales.json" "/out:$targetPath" $languageSource $uiLanguageSource $source $saveTabSource $profileEditorSource
        if ($LASTEXITCODE -ne 0) { throw "Launcher compilation failed (exit $LASTEXITCODE)." }
        Write-Output "Built $targetPath"
    }
}
if ($Launch) {
    Start-Process -FilePath $output -WorkingDirectory $projectRoot -WindowStyle Hidden
}
