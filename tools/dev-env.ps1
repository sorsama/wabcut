# Wabcut developer environment (Windows). See docs/building.md.
#
# Dot-source it before configuring/building:
#   . .\tools\dev-env.ps1          full build (win-msvc preset): MSVC + CMake + Ninja + vcpkg + Qt
#   . .\tools\dev-env.ps1 -Core    core-only build (core preset): MSVC + CMake + Ninja, no vcpkg or Qt
#
# It enters the Visual Studio x64 developer shell when cl.exe is not already on PATH, and validates every tool it
# needs, failing with a clear message instead of letting CMake fail later. Overrides (environment variables):
#   VCPKG_ROOT                   vcpkg checkout                     (default G:\dev\vcpkg)
#   VCPKG_DEFAULT_BINARY_CACHE   vcpkg binary cache                 (default G:\dev\vcpkg-cache)
#   WABCUT_QT_DIR                Qt kit, e.g. C:\Qt\6.11.3\msvc2022_64 (default C:\Qt\6.11.3\msvc2022_64)
#   WABCUT_DEPS_READY_MARKER     shared-prebuild gate file          (default G:\dev\vcpkg-prebuild\READY)

param([switch]$Core)

$ErrorActionPreference = 'Stop'
$wabcutTestedQt = '6.11.3'
$wabcutMinCMake = [version]'3.28'

function Fail([string]$message) {
    throw "dev-env: $message (see docs/building.md)"
}

# ---- Compiler, CMake, Ninja ---------------------------------------------------------------------------------
$vsInstaller = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer"
$vswhere = Join-Path $vsInstaller 'vswhere.exe'

if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    if (-not (Test-Path $vswhere)) { Fail 'Visual Studio (2022 17.6+ or newer) with "Desktop development with C++" is not installed.' }
    $env:PATH = "$vsInstaller;$env:PATH"   # VsDevCmd.bat shells out to vswhere.exe
    # Prefer an install that bundles CMake + Ninja alongside the C++ toolset; fall back to any C++ install.
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 Microsoft.VisualStudio.Component.VC.CMake.Project -property installationPath
    if (-not $vs) { $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath }
    if (-not $vs) { Fail 'no Visual Studio install has the MSVC x64 toolset (component "MSVC ... x64/x86 build tools").' }
    $vs = @($vs)[0]
    Import-Module (Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
    Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
}
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) { Fail 'cl.exe is still not on PATH after entering the VS developer shell.' }

if (-not (Get-Command cmake.exe -ErrorAction SilentlyContinue) -or -not (Get-Command ninja.exe -ErrorAction SilentlyContinue)) {
    # The selected VS install has no bundled CMake/Ninja: borrow them from any install that does.
    if (Test-Path $vswhere) {
        foreach ($inst in (& $vswhere -products * -property installationPath)) {
            $cmakeDir = Join-Path $inst 'Common7\IDE\CommonExtensions\Microsoft\CMake'
            if ((Test-Path "$cmakeDir\CMake\bin\cmake.exe") -and (Test-Path "$cmakeDir\Ninja\ninja.exe")) {
                $env:PATH = "$cmakeDir\CMake\bin;$cmakeDir\Ninja;$env:PATH"
                break
            }
        }
    }
}
if (-not (Get-Command cmake.exe -ErrorAction SilentlyContinue)) { Fail 'cmake.exe not found: install the VS component "C++ CMake tools for Windows" or CMake 3.28+.' }
if (-not (Get-Command ninja.exe -ErrorAction SilentlyContinue)) { Fail 'ninja.exe not found: install the VS component "C++ CMake tools for Windows" or Ninja.' }
$cmakeVersionText = (& cmake.exe --version | Select-Object -First 1)
if ($cmakeVersionText -match '(\d+\.\d+(\.\d+)?)') {
    if ([version]$Matches[1] -lt $wabcutMinCMake) { Fail "CMake $($Matches[1]) is too old; Wabcut needs CMake $wabcutMinCMake or later." }
} else {
    Fail "cannot read the CMake version from '$cmakeVersionText'."
}

if ($Core) {
    Write-Host "Wabcut core dev env ready: $((Get-Command cl.exe).Source), $cmakeVersionText (no vcpkg, no Qt)"
    return
}

# ---- vcpkg -------------------------------------------------------------------------------------------------
if (-not $env:VCPKG_ROOT) { $env:VCPKG_ROOT = 'G:\dev\vcpkg' }
if (-not $env:VCPKG_DEFAULT_BINARY_CACHE) { $env:VCPKG_DEFAULT_BINARY_CACHE = 'G:\dev\vcpkg-cache' }
if (-not (Test-Path "$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake")) {
    Fail "VCPKG_ROOT ($env:VCPKG_ROOT) is not a vcpkg checkout: clone https://github.com/microsoft/vcpkg there or set VCPKG_ROOT."
}
if (-not (Test-Path "$env:VCPKG_ROOT\vcpkg.exe")) {
    Fail "vcpkg is not bootstrapped: run $env:VCPKG_ROOT\bootstrap-vcpkg.bat -disableMetrics."
}
New-Item -ItemType Directory -Force $env:VCPKG_DEFAULT_BINARY_CACHE | Out-Null

# Shared dependency prebuild gate: while the lead is filling the vcpkg binary cache, a parallel configure
# would compile FFmpeg/x264/x265 again from scratch. Wait for the READY marker (only if a prebuild exists here).
if (-not $env:WABCUT_DEPS_READY_MARKER) { $env:WABCUT_DEPS_READY_MARKER = 'G:\dev\vcpkg-prebuild\READY' }
$prebuildDir = Split-Path $env:WABCUT_DEPS_READY_MARKER -Parent
if ((Test-Path $prebuildDir) -and -not (Test-Path $env:WABCUT_DEPS_READY_MARKER)) {
    Write-Host "Waiting for the shared dependency prebuild ($env:WABCUT_DEPS_READY_MARKER) ..."
    $deadline = (Get-Date).AddMinutes(120)
    while (-not (Test-Path $env:WABCUT_DEPS_READY_MARKER)) {
        if ((Get-Date) -gt $deadline) { Fail "dependency prebuild not ready after 120 min ($env:WABCUT_DEPS_READY_MARKER). Ask the lead." }
        Start-Sleep -Seconds 30
    }
}

# ---- Qt --------------------------------------------------------------------------------------------------------
if (-not $env:WABCUT_QT_DIR) { $env:WABCUT_QT_DIR = "C:\Qt\$wabcutTestedQt\msvc2022_64" }
$qtpaths = Join-Path $env:WABCUT_QT_DIR 'bin\qtpaths.exe'
if (-not (Test-Path "$env:WABCUT_QT_DIR\lib\cmake\Qt6\Qt6Config.cmake") -or -not (Test-Path $qtpaths)) {
    Fail "WABCUT_QT_DIR ($env:WABCUT_QT_DIR) is not a Qt 6 MSVC x64 kit: install Qt $wabcutTestedQt (MSVC 2022 64-bit, with Qt Shader Tools) or set WABCUT_QT_DIR."
}
$qtVersion = (& $qtpaths --qt-version).Trim()
if ([version]$qtVersion -lt [version]'6.11') { Fail "Qt $qtVersion in $env:WABCUT_QT_DIR is too old; Wabcut needs Qt 6.11 (tested with $wabcutTestedQt)." }
if ($qtVersion -ne $wabcutTestedQt) { Write-Warning "Qt $qtVersion differs from the tested Qt $wabcutTestedQt." }
foreach ($module in @('Qt6Quick', 'Qt6QuickControls2', 'Qt6ShaderTools', 'Qt6Svg')) {
    if (-not (Test-Path "$env:WABCUT_QT_DIR\lib\cmake\$module")) { Fail "the Qt kit lacks $module; add it in the Qt installer (Qt Shader Tools is under Additional Libraries)." }
}

$env:PATH = "$env:WABCUT_QT_DIR\bin;$env:PATH"
# vcpkg builds Windows ports in a clean environment. The QtKeychain overlay port (WABCUT_WITH_CLOUD_AI) builds against
# this Qt, so WABCUT_QT_DIR is passed through (untracked by the ABI; cmake/ports/qtkeychain/wabcut-qt.cmake pins the
# Qt version instead, so the triplet and every other package's ABI stay unchanged).
$keepVars = @($env:VCPKG_KEEP_ENV_VARS -split ';' | Where-Object { $_ })
if ($keepVars -notcontains 'WABCUT_QT_DIR') { $env:VCPKG_KEEP_ENV_VARS = (@($keepVars) + 'WABCUT_QT_DIR') -join ';' }
Write-Host "Wabcut dev env ready: VCPKG_ROOT=$env:VCPKG_ROOT  Qt=$env:WABCUT_QT_DIR ($qtVersion)  $cmakeVersionText"
