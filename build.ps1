param(
    [string]$ToolchainBin = "",
    [ValidateSet("Release", "Debug")]
    [string]$Configuration = "Release",
    [switch]$Clean
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path

if ($ToolchainBin) {
    $cmake = Join-Path $ToolchainBin "cmake.exe"
    $ctest = Join-Path $ToolchainBin "ctest.exe"
    $ninja = Join-Path $ToolchainBin "ninja.exe"
    $compiler = Join-Path $ToolchainBin "g++.exe"
    $env:PATH = "$ToolchainBin;$env:PATH"
} else {
    $cmake = (Get-Command cmake.exe -ErrorAction Stop).Source
    $ctest = (Get-Command ctest.exe -ErrorAction Stop).Source
    $ninja = (Get-Command ninja.exe -ErrorAction Stop).Source
    $compiler = (Get-Command g++.exe -ErrorAction Stop).Source
}

foreach ($required in @($cmake, $ctest, $ninja, $compiler)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Required build tool not found: $required"
    }
}

$buildDir = Join-Path $projectRoot "build"
if ($Clean -and (Test-Path -LiteralPath $buildDir)) {
    $resolvedRoot = [System.IO.Path]::GetFullPath($projectRoot).TrimEnd('\')
    $resolvedBuild = [System.IO.Path]::GetFullPath($buildDir).TrimEnd('\')
    if (-not $resolvedBuild.StartsWith($resolvedRoot + '\', [System.StringComparison]::OrdinalIgnoreCase) -or
        [System.IO.Path]::GetFileName($resolvedBuild) -ne 'build') {
        throw "Refusing to clean unexpected build path: $resolvedBuild"
    }
    Remove-Item -LiteralPath $resolvedBuild -Recurse -Force
}
& $cmake -S $projectRoot -B $buildDir -G Ninja "-DCMAKE_BUILD_TYPE=$Configuration" "-DCMAKE_MAKE_PROGRAM=$ninja" "-DCMAKE_CXX_COMPILER=$compiler"
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed: $LASTEXITCODE" }

& $cmake --build $buildDir --config $Configuration
if ($LASTEXITCODE -ne 0) { throw "Build failed: $LASTEXITCODE" }

& $ctest --test-dir $buildDir --output-on-failure
if ($LASTEXITCODE -ne 0) { throw "Tests failed: $LASTEXITCODE" }

$version = "0.1.2-rc1"
$distDir = Join-Path $projectRoot "dist"
$manualRoot = Join-Path $buildDir "manual-package"
$manualPluginDir = Join-Path $manualRoot "aimp_ets2_cast"
New-Item -ItemType Directory -Path $distDir -Force | Out-Null
if (Test-Path -LiteralPath $manualRoot) {
    Remove-Item -LiteralPath $manualRoot -Recurse -Force
}
New-Item -ItemType Directory -Path $manualPluginDir -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $buildDir "bin\aimp_ets2_cast.dll") -Destination $manualPluginDir -Force
Copy-Item -LiteralPath (Join-Path $projectRoot "packaging\README.txt") -Destination $manualPluginDir -Force

$packageRoot = Join-Path $buildDir "package"
$packagePluginDir = Join-Path $packageRoot "aimp_ets2_cast"
$packageX64Dir = Join-Path $packagePluginDir "x64"
if (Test-Path -LiteralPath $packageRoot) {
    Remove-Item -LiteralPath $packageRoot -Recurse -Force
}
New-Item -ItemType Directory -Path $packageX64Dir -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $buildDir "bin\aimp_ets2_cast.dll") -Destination $packageX64Dir -Force
Copy-Item -LiteralPath (Join-Path $projectRoot "packaging\README.txt") -Destination $packagePluginDir -Force
$packageZip = Join-Path $buildDir "aimp_ets2_cast-package.zip"
$packageAimp = Join-Path $distDir "aimp_ets2_cast-$version.aimppack"
if (Test-Path -LiteralPath $packageZip) {
    Remove-Item -LiteralPath $packageZip -Force
}
Compress-Archive -LiteralPath $packagePluginDir -DestinationPath $packageZip -CompressionLevel Optimal
Copy-Item -LiteralPath $packageZip -Destination $packageAimp -Force

$manualZip = Join-Path $distDir "AIMP-ETS2-Cast-$version-x64-dist.zip"
if (Test-Path -LiteralPath $manualZip) {
    Remove-Item -LiteralPath $manualZip -Force
}
Compress-Archive -LiteralPath $manualPluginDir -DestinationPath $manualZip -CompressionLevel Optimal

Write-Host "Built: $(Join-Path $buildDir 'bin\aimp_ets2_cast.dll')"
Write-Host "Package: $packageAimp"
Write-Host "Manual package: $manualZip"
