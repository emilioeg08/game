<#
.SYNOPSIS
    Builds the release client and assembles what players get (the Steam depot) in dist/.

.DESCRIPTION
    dist/GalaxyEngine/        the game as installed by Steam: gx_game.exe and licenses/ (no PDB, no tests)
    dist/symbols/             gx_game.pdb, to symbolize crash dumps (never shipped)
    dist/GalaxyEngine-<version>-windows-x64.zip
    Unless -NoSmoke, the packaged executable is started twice into a throwaway data folder: the game and the
    main menu, 60 frames each, with a screenshot. Both must exit cleanly.

.EXAMPLE
    ./scripts/package.ps1                  # build, test, package, smoke test
    ./scripts/package.ps1 -SkipTests       # faster, for local iteration
#>
param(
    [switch]$SkipTests,
    [switch]$NoSmoke,
    [string]$Output = 'dist'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

$buildArgs = @{ Preset = 'release' }
if (-not $SkipTests) { $buildArgs.Test = $true }
& (Join-Path $PSScriptRoot 'build.ps1') @buildArgs

$build = Join-Path $root 'build\release'
$dist = Join-Path $root $Output
$content = Join-Path $dist 'GalaxyEngine'
$symbols = Join-Path $dist 'symbols'
foreach ($folder in $content, $symbols) {
    if (Test-Path $folder) { Remove-Item -Recurse -Force $folder }
}

Write-Host '==> Install (client, symbols)' -ForegroundColor Cyan
cmake --install $build --component client --prefix $content
if ($LASTEXITCODE -ne 0) { throw 'Installing the client failed' }
cmake --install $build --component symbols --prefix $symbols
if ($LASTEXITCODE -ne 0) { throw 'Installing the symbols failed' }

$exe = Join-Path $content 'gx_game.exe'
if (-not (Test-Path $exe)) { throw "The package has no gx_game.exe: $content" }
if (Get-ChildItem -Path $content -Recurse -Include *.pdb, *.lib, *.ilk) { throw 'Build files leaked into the package' }
if (-not (Test-Path (Join-Path $content 'data\lang\en.po'))) { throw 'The package has no translations (data/lang)' }

if (-not $NoSmoke) {
    Write-Host '==> Smoke test' -ForegroundColor Cyan
    $smoke = Join-Path $dist 'smoke'
    if (Test-Path $smoke) { Remove-Item -Recurse -Force $smoke }
    New-Item -ItemType Directory -Path $smoke | Out-Null
    $runs = @(
        @{ Shot = 'game.png'; Args = @('--no-help', '--prerun-hours', '0.5') },
        @{ Shot = 'menu.png'; Args = @('--menu', 'main') },
        @{ Shot = 'options.png'; Args = @('--menu', 'options') }
    )
    foreach ($run in $runs) {
        $shot = Join-Path $smoke $run.Shot
        $arguments = @('--frames', '60', '--data-dir', "`"$smoke`"", '--screenshot', "`"$shot`"") + $run.Args
        $process = Start-Process -FilePath $exe -Wait -PassThru -WorkingDirectory $content -ArgumentList $arguments
        $log = Join-Path $smoke 'logs\gx_game.log'
        if (Test-Path $log) { Get-Content $log | Select-Object -Last 10 | ForEach-Object { Write-Host "    $_" } }
        if ($process.ExitCode -ne 0) { throw "gx_game.exe exited with code $($process.ExitCode) ($($run.Shot))" }
        if (-not (Test-Path $shot)) { throw "gx_game.exe did not write $($run.Shot)" }
    }
}

$version = (Select-String -Path (Join-Path $root 'CMakeLists.txt') -Pattern 'VERSION (\d+\.\d+\.\d+)').Matches[0].Groups[1].Value
$zip = Join-Path $dist "GalaxyEngine-$version-windows-x64.zip"
Compress-Archive -Path (Join-Path $content '*') -DestinationPath $zip -Force
$size = (Get-ChildItem -Path $content -Recurse -File | Measure-Object -Property Length -Sum).Sum / 1MB
Write-Host ("==> {0} ({1:N1} MB installed)" -f $zip, $size) -ForegroundColor Green
