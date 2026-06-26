#requires -Version 5.1
param(
    [switch]$WindowsOnly,
    [switch]$LinuxOnly
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

function Copy-Artifact($from, $to) {
    try {
        Copy-Item $from $to -Force
    } catch [System.IO.IOException] {
        throw "Cannot write '$to' (file is locked). Stop the nanos server, then run the build again."
    }
}

function Build-Windows {
    Write-Host "==> Build Windows (MSVC)" -ForegroundColor Cyan
    cmake -S $root -B "$root\build"
    if ($LASTEXITCODE -ne 0) { throw "Windows CMake configuration failed" }
    cmake --build "$root\build" --config Release
    if ($LASTEXITCODE -ne 0) { throw "Windows build failed" }

    Copy-Artifact "$root\build\Release\norm_database.dll" "$root\norm_database.dll"
    Write-Host "    norm_database.dll -> root" -ForegroundColor Green
}

function Build-Linux {
    Write-Host "==> Build Linux (WSL / g++)" -ForegroundColor Cyan

    wsl bash -lc "command -v cmake >/dev/null" | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "cmake is missing in WSL. Install it once: wsl sudo apt update; wsl sudo apt install -y cmake"
    }

    $rootFwd = $root -replace '\\','/'
    $wslPath = (wsl wslpath -a "$rootFwd")
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($wslPath)) {
        throw "WSL path conversion failed"
    }
    $wslPath = $wslPath.Trim()

    wsl bash -lc "cmake -S '$wslPath' -B '$wslPath/build_linux' -DCMAKE_BUILD_TYPE=Release && cmake --build '$wslPath/build_linux' -j`$(nproc)"
    if ($LASTEXITCODE -ne 0) { throw "Linux build failed" }

    Copy-Artifact "$root\build_linux\norm_database.so" "$root\libnorm_database.so"
    Write-Host "    libnorm_database.so -> root" -ForegroundColor Green
}

if (-not $LinuxOnly)   { Build-Windows }
if (-not $WindowsOnly) { Build-Linux }

Write-Host "==> Done." -ForegroundColor Green
