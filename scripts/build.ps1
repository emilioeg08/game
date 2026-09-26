<#
.SYNOPSIS
    Configures and builds GalaxyEngine with MSVC + Ninja, optionally running tests and benchmarks.

.EXAMPLE
    ./scripts/build.ps1                     # release build
    ./scripts/build.ps1 -Preset debug -Test # debug build + tests
    ./scripts/build.ps1 -Test -Bench        # release build + tests + benchmarks

.NOTES
    Needs Visual Studio 2022+ with the C++ workload, plus CMake and Ninja:
        python -m pip install -r tools/requirements-build.txt
#>
param(
    [ValidateSet('debug', 'release', 'profile')]
    [string]$Preset = 'release',
    [switch]$Test,
    [switch]$Bench,
    [switch]$Quick,
    [switch]$Fresh
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

function Import-MsvcEnvironment {
    if (Get-Command cl.exe -ErrorAction SilentlyContinue) { return }

    $installer = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer'
    $vswhere = Join-Path $installer 'vswhere.exe'
    if (-not (Test-Path $vswhere)) { throw 'vswhere.exe not found: install Visual Studio with the "Desktop development with C++" workload.' }

    $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vsPath) { throw 'No Visual Studio installation with the MSVC x64 toolset was found.' }

    $vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
    $env:PATH = "$installer;$env:PATH" # vcvars looks vswhere up on PATH
    $lines = cmd /c "`"$vcvars`" >nul 2>&1 && set"
    foreach ($line in $lines) {
        if ($line -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
    }
    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) { throw "Failed to import the MSVC environment from $vcvars" }
}

function Invoke-Step([string]$Description, [scriptblock]$Command) {
    Write-Host "==> $Description" -ForegroundColor Cyan
    & $Command
    if ($LASTEXITCODE -ne 0) { throw "$Description failed (exit code $LASTEXITCODE)" }
}

Import-MsvcEnvironment
foreach ($tool in 'cmake', 'ninja') {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        throw "$tool not found. Install it with: python -m pip install -r tools/requirements-build.txt"
    }
}

Push-Location $root
try {
    $configureArgs = @('--preset', $Preset)
    if ($Fresh) { $configureArgs += '--fresh' }
    Invoke-Step "Configure ($Preset)" { cmake @configureArgs }
    Invoke-Step "Build ($Preset)" { cmake --build --preset $Preset }
    if ($Test) {
        Invoke-Step "Test ($Preset)" { ctest --preset $Preset }
    }
    if ($Bench) {
        $benchArgs = @()
        if ($Quick) { $benchArgs += '--quick' }
        Invoke-Step "Benchmark ($Preset)" { & (Join-Path $root "build\$Preset\bin\gx_bench.exe") @benchArgs }
    }
}
finally {
    Pop-Location
}
