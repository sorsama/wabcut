<#
.SYNOPSIS
    Installs the WiX Toolset that builds Wabcut's MSI, for the current user only (no administrator rights).

.DESCRIPTION
    WiX is a .NET tool: this runs `dotnet tool install wix --version <Version> --tool-path <Destination>` and adds the
    WiX UI and Util extensions to the user's WiX extension cache (%USERPROFILE%\.wix\extensions). The .NET SDK 6 or
    newer must be on PATH (Visual Studio installs one; so do the GitHub Windows runners).

    CMake looks for wix.exe in $env:WABCUT_WIX_DIR, then in the default destination below, so after running this a
    fresh configure (or `cmake --preset win-msvc` again) adds the WIX generator. See docs/release.md.

.PARAMETER Version
    WiX version. Wabcut's packaging is written for and tested with 5.0.2 (see cmake/packaging/WabcutPackaging.cmake).

.PARAMETER Destination
    Tool folder. Default: %LOCALAPPDATA%\wabcut-tools\wix\<Version>.

.EXAMPLE
    ./cmake/packaging/Install-WixToolset.ps1
#>
[CmdletBinding()]
param(
    [string]$Version = '5.0.2',
    [string]$Destination = ''
)

$ErrorActionPreference = 'Stop'
if (-not $Destination) {
    $Destination = Join-Path $env:LOCALAPPDATA "wabcut-tools\wix\$Version"
}
$env:DOTNET_CLI_TELEMETRY_OPTOUT = '1'
$env:DOTNET_NOLOGO = '1'

if (-not (Get-Command dotnet -ErrorAction SilentlyContinue)) {
    throw "Install-WixToolset: the .NET SDK (dotnet) is not on PATH. Install the .NET SDK 6 or newer for your user or use the one Visual Studio installed."
}

$wix = Join-Path $Destination 'wix.exe'
$installed = ''
if (Test-Path $wix) {
    $installed = (& $wix --version) -replace '\+.*$', ''
}
if ($installed -ne $Version) {
    if ($installed) {
        & dotnet tool uninstall wix --tool-path $Destination | Out-Host
    }
    & dotnet tool install wix --version $Version --tool-path $Destination | Out-Host
    if ($LASTEXITCODE) { throw "Install-WixToolset: dotnet tool install wix $Version failed ($LASTEXITCODE)" }
}

# Extensions must match the wix version exactly.
foreach ($extension in 'WixToolset.UI.wixext', 'WixToolset.Util.wixext') {
    & $wix extension add --global "$extension/$Version" | Out-Host
    if ($LASTEXITCODE) { throw "Install-WixToolset: wix extension add $extension/$Version failed ($LASTEXITCODE)" }
}

Write-Host "WiX $(& $wix --version) in $Destination"
& $wix extension list --global
if ($env:GITHUB_PATH) {
    # CI: make it visible to the following steps as well.
    Add-Content -Path $env:GITHUB_PATH -Value $Destination
}
