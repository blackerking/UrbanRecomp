param(
    [string]$Rom,
    [switch]$BuildOnly,
    [switch]$SkipBuild,
    [string]$CMake = 'C:/Program Files/CMake/bin/cmake.exe',
    [string]$Toolchain = 'C:/msys64/mingw64'
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$build = Join-Path $repo 'build-custom'
$exe = Join-Path $build 'SimCitySNESRecomp.exe'
if ($Rom) { $Rom = (Resolve-Path -LiteralPath $Rom).Path }
if (-not $SkipBuild) {
    if (-not (Test-Path -LiteralPath $CMake)) { throw "CMake not found: $CMake" }
    foreach ($program in @('gcc.exe', 'g++.exe', 'ninja.exe')) {
        if (-not (Test-Path -LiteralPath (Join-Path $Toolchain "bin/$program"))) {
            throw "Missing $program in $Toolchain/bin. See docs/ADAPTIVE_RENDERER.md."
        }
    }
    & $CMake -S $repo -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release `
        "-DCMAKE_C_COMPILER=$Toolchain/bin/gcc.exe" `
        "-DCMAKE_CXX_COMPILER=$Toolchain/bin/g++.exe" `
        "-DCMAKE_MAKE_PROGRAM=$Toolchain/bin/ninja.exe" "-DCMAKE_PREFIX_PATH=$Toolchain"
    if ($LASTEXITCODE) { throw 'CMake configuration failed' }
    & $CMake --build $build --parallel 8
    if ($LASTEXITCODE) { throw 'Build failed' }
}
if ($BuildOnly) { return }
if (-not (Test-Path -LiteralPath $exe)) { throw "Build not found: $exe" }
$gameArgs = @('--mods')
if ($Rom) { $gameArgs += $Rom }
Push-Location -LiteralPath $repo
try {
    & $exe @gameArgs
    if ($LASTEXITCODE) { throw "SimCity exited with code $LASTEXITCODE" }
} finally { Pop-Location }
