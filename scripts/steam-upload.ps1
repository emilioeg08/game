<#
.SYNOPSIS
    Uploads dist/GalaxyEngine (made by scripts/package.ps1) to Steam as a new build, with SteamCMD.

.EXAMPLE
    ./scripts/package.ps1
    ./scripts/steam-upload.ps1 -AppId 1234560 -DepotId 1234561 -Username mi_cuenta -Branch beta

.NOTES
    Needs SteamCMD (https://developer.valvesoftware.com/wiki/SteamCMD) on PATH, or its path in -SteamCmd, and
    a Steamworks account allowed to upload builds for the app. SteamCMD asks for the password and the Steam
    Guard code itself: they never go through this script. A build cannot be made live on the default branch
    from here; Valve requires doing that in Steamworks (App Admin > SteamPipe > Builds). -Preview checks the
    depot without uploading anything. See docs/STEAM.md.
#>
param(
    [Parameter(Mandatory = $true)][string]$AppId,
    [Parameter(Mandatory = $true)][string]$DepotId,
    [Parameter(Mandatory = $true)][string]$Username,
    [string]$Branch = '',
    [string]$Description = '',
    [string]$SteamCmd = 'steamcmd',
    [switch]$Preview
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
foreach ($id in $AppId, $DepotId) {
    if ($id -notmatch '^\d+$') { throw "Steam ids are numbers: '$id'" }
}
if ($Branch -eq 'default') { throw 'Valve does not allow setting the default branch live from SteamCMD.' }
$content = Join-Path $root 'dist\GalaxyEngine'
if (-not (Test-Path (Join-Path $content 'gx_game.exe'))) { throw 'Nothing to upload: run ./scripts/package.ps1 first.' }
if (-not (Get-Command $SteamCmd -ErrorAction SilentlyContinue)) { throw "SteamCMD not found ('$SteamCmd')." }

if (-not $Description) {
    $version = (Select-String -Path (Join-Path $root 'CMakeLists.txt') -Pattern 'VERSION (\d+\.\d+\.\d+)').Matches[0].Groups[1].Value
    $commit = (git -C $root rev-parse --short HEAD) 2>$null
    $Description = "GalaxyEngine $version ($commit)"
}

# The build scripts, from the templates in tools/steam (paths relative to them, as SteamPipe expects).
$scripts = Join-Path $root 'build\steam'
New-Item -ItemType Directory -Force -Path (Join-Path $scripts 'output') | Out-Null
$values = @{
    '@APPID@'   = $AppId
    '@DEPOTID@' = $DepotId
    '@DESC@'    = $Description -replace '"', "'"
    '@BRANCH@'  = $Branch
    '@PREVIEW@' = $(if ($Preview) { '1' } else { '0' })
}
foreach ($name in 'app_build.vdf', 'depot_build_windows.vdf') {
    $text = Get-Content -Raw (Join-Path $root "tools\steam\$name.in")
    foreach ($key in $values.Keys) { $text = $text.Replace($key, $values[$key]) }
    Set-Content -Path (Join-Path $scripts $name) -Value $text -Encoding ascii
}

$appBuild = Join-Path $scripts 'app_build.vdf'
Write-Host "==> SteamCMD: app $AppId, depot $DepotId, branch '$Branch', $Description" -ForegroundColor Cyan
& $SteamCmd +login $Username +run_app_build $appBuild +quit
if ($LASTEXITCODE -ne 0) { throw "SteamCMD failed (exit code $LASTEXITCODE): see $scripts\output" }
