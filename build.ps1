$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $root

$buildNumberPath = Join-Path $root 'build_number.txt'
$buildDir = Join-Path $root 'build'

if (-not (Test-Path -LiteralPath $buildDir)) {
    New-Item -ItemType Directory -Path $buildDir | Out-Null
}

$currentBuild = 0
if (Test-Path -LiteralPath $buildNumberPath) {
    $rawValue = (Get-Content -LiteralPath $buildNumberPath -Raw).Trim()
    if ($rawValue) {
        $currentBuild = [int]$rawValue
    }
}

$nextBuild = $currentBuild + 1
$buildStamp = '{0:D4}' -f $nextBuild

$sources = Get-ChildItem -LiteralPath 'src' -Filter '*.cpp' | Sort-Object Name | ForEach-Object { $_.FullName }
$namedExe = Join-Path $root ("snes_build_{0}.exe" -f $buildStamp)
$stableExe = Join-Path $root 'snes.exe'
$buildExe = Join-Path $buildDir 'snes_emu.exe'

Write-Host ("[BUILD] Compiling build {0}..." -f $buildStamp)

$args = @(
    '-std=c++17'
    '-O2'
    '-Isrc'
)
$args += $sources
$args += @(
    '-static'
    ("-DBUILD_NUMBER={0}" -f $nextBuild)
    '-lmingw32'
    '-lSDL2main'
    '-lSDL2'
    '-mwindows'
    '-lm'
    '-ldinput8'
    '-ldxguid'
    '-ldxerr8'
    '-luser32'
    '-lgdi32'
    '-lwinmm'
    '-limm32'
    '-lole32'
    '-loleaut32'
    '-lshell32'
    '-lsetupapi'
    '-lversion'
    '-luuid'
    '-lcfgmgr32'
    '-o'
    $namedExe
)

& g++ @args
if ($LASTEXITCODE -ne 0) {
    throw ("g++ failed with exit code {0}" -f $LASTEXITCODE)
}

Copy-Item -LiteralPath $namedExe -Destination $stableExe -Force
Copy-Item -LiteralPath $namedExe -Destination $buildExe -Force
Set-Content -LiteralPath $buildNumberPath -Value $nextBuild -NoNewline

Write-Host ("[BUILD] Success -> {0}" -f (Split-Path -Leaf $namedExe))
Write-Host ("[BUILD] Updated stable binaries: {0}, {1}" -f (Split-Path -Leaf $stableExe), (Split-Path -Leaf $buildExe))
