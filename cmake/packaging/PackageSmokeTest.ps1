<#
.SYNOPSIS
    CTest `install.package` (label: install, Windows): builds Wabcut's packages with cpack and checks them the way a
    user meets them.

.DESCRIPTION
    1. cpack builds every configured generator (the portable ZIP, and the MSI when WiX is installed) into
       <build>/package and the script reports each package's size and contents.
    2. The portable ZIP is unpacked into a scratch folder and run from there with PATH reduced to Windows' own
       folders: `wabcut-cli --version`, the AI helpers, and a headless render of a tiny project.
    3. The MSI is checked statically (per-user package, UpgradeCode, the uninstaller removes no wildcard files),
       installed for the current user into a scratch folder without elevation (`msiexec /qn`, opting in to the .wabcut
       association when the user has none), run like the ZIP, checked for its Start menu entry and association,
       then uninstalled. Afterwards every installed file, the shortcut and the association must be gone, and the
       user files planted next to them (a project in the install folder, a note in bin\) must still be there.

    The MSI round trip never touches a real installation: when Wabcut is already installed for this user (same
    UpgradeCode), or WiX is not configured, the script exits 77 (CTest: skipped) after the ZIP checks.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string]$BuildDir,
    [Parameter(Mandatory)] [string]$CPack,
    [Parameter(Mandatory)] [string]$Version,
    [Parameter(Mandatory)] [string]$PackageBaseName,
    [Parameter(Mandatory)] [string]$UpgradeCode,
    [Parameter(Mandatory)] [string]$SmokeProject,
    [string]$Generators = 'ZIP'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0
$skipCode = 77
$BuildDir = [System.IO.Path]::GetFullPath($BuildDir)
$SmokeProject = [System.IO.Path]::GetFullPath($SmokeProject)

function Fail([string]$message) {
    Write-Host "FAILED: $message"
    exit 1
}

# Runs a program and returns { ExitCode, Output } (stdout and stderr), without PowerShell's native-command stderr
# handling. Arguments are quoted for the C runtime's parser; $RawArguments is passed as is (msiexec has its own
# parser). A program still running after $TimeoutSeconds is killed and reported.
function Invoke-Program([string]$File, [string[]]$Arguments = @(), [string]$RawArguments = '', [string]$InputText = '',
                        [int]$TimeoutSeconds = 300, [hashtable]$Environment = $null) {
    $info = New-Object System.Diagnostics.ProcessStartInfo
    $info.FileName = $File
    if ($RawArguments) {
        $info.Arguments = $RawArguments
    } else {
        $info.Arguments = ($Arguments | ForEach-Object {
                if ($_ -match '[\s"]' -or $_ -eq '') { '"' + ($_ -replace '(\\*)"', '$1$1\"' -replace '(\\+)$', '$1$1') + '"' } else { $_ }
            }) -join ' '
    }
    $info.UseShellExecute = $false
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.RedirectStandardInput = $true
    if ($Environment) {
        foreach ($key in $Environment.Keys) { $info.EnvironmentVariables[$key] = $Environment[$key] }
    }
    $process = [System.Diagnostics.Process]::Start($info)
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    if ($InputText) { $process.StandardInput.Write($InputText) }
    $process.StandardInput.Close()
    if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
        $process.Kill()
        Fail "$File $($info.Arguments) did not finish within $TimeoutSeconds s"
    }
    $process.WaitForExit()
    return [pscustomobject]@{ ExitCode = $process.ExitCode; Output = $stdout.Result + $stderr.Result }
}

# Windows Installer's automation objects have no type information PowerShell can use: call them by name. The
# arguments go unwrapped (a string from Join-Path is a PSObject, which IDispatch rejects as a type mismatch), and the
# result comes back as one object (PowerShell would unroll a StringList such as RelatedProducts).
function Invoke-ComMember($Object, [string]$Name, [System.Reflection.BindingFlags]$Kind, [object[]]$Arguments) {
    $plain = [System.Collections.Generic.List[object]]::new()
    foreach ($argument in $Arguments) { $plain.Add($(if ($null -eq $argument) { $null } else { $argument.psobject.BaseObject })) }
    return , $Object.GetType().InvokeMember($Name, $Kind, $null, $Object, $plain.ToArray())
}
function Get-ComProperty($Object, [string]$Name, [object[]]$Arguments = @()) {
    return , (Invoke-ComMember $Object $Name ([System.Reflection.BindingFlags]::GetProperty) $Arguments)
}
function Invoke-ComMethod($Object, [string]$Name, [object[]]$Arguments = @()) {
    return , (Invoke-ComMember $Object $Name ([System.Reflection.BindingFlags]::InvokeMethod) $Arguments)
}
function Get-MsiRows($Database, [string]$Query, [int]$Columns) {
    $view = Invoke-ComMethod $Database 'OpenView' @($Query)
    Invoke-ComMethod $view 'Execute' | Out-Null
    $rows = @()
    while ($true) {
        $record = Invoke-ComMethod $view 'Fetch'
        if ($null -eq $record) { break }
        $row = @()
        for ($i = 1; $i -le $Columns; $i++) { $row += Get-ComProperty $record 'StringData' @($i) }
        $rows += , $row
    }
    Invoke-ComMethod $view 'Close' | Out-Null
    return , $rows
}

function Format-Size([long]$Bytes) {
    return '{0:N0} bytes ({1:N1} MiB)' -f $Bytes, ($Bytes / 1MB)
}

# A Windows-only environment: the packaged programs must not find Qt, vcpkg or the build tree on PATH.
$minimalEnvironment = @{
    PATH = "$env:SystemRoot\System32;$env:SystemRoot"
    QT_PLUGIN_PATH = ''; QML_IMPORT_PATH = ''; QML2_IMPORT_PATH = ''; QT_QPA_PLATFORM_PLUGIN_PATH = ''
}

# Runs the programs of an installed or unpacked tree: the CLI, the helpers, and a headless render of the tiny
# project (a title added with `wabcut-cli cmd`, rendered through the real renderer and libx264).
function Test-Tree([string]$Root, [string]$What, [string]$Scratch) {
    $bin = Join-Path $Root 'bin'
    $cli = Join-Path $bin 'wabcut-cli.exe'
    foreach ($required in 'wabcut.exe', 'wabcut-cli.exe', 'Qt6Core.dll', 'qt.conf') {
        if (-not (Test-Path (Join-Path $bin $required))) { Fail "$What lacks bin\$required" }
    }
    foreach ($required in 'LICENSE.txt', 'THIRD-PARTY-NOTICES.txt', 'licenses\ffmpeg.txt', 'plugins\platforms\qwindows.dll') {
        if (-not (Test-Path (Join-Path $Root $required))) { Fail "$What lacks $required" }
    }
    $run = Invoke-Program $cli @('--version') -Environment $minimalEnvironment
    if ($run.ExitCode -ne 0 -or $run.Output -notmatch "wabcut-cli $([regex]::Escape($Version))") {
        Fail "$What`: wabcut-cli --version failed ($($run.ExitCode)): $($run.Output)"
    }
    Write-Host "$What`: $($run.Output.Trim())"
    $infer = Join-Path $bin 'wabcut-infer.exe'
    if (Test-Path $infer) {
        $run = Invoke-Program $infer @('--devices') -Environment $minimalEnvironment -TimeoutSeconds 120
        if ($run.ExitCode -ne 0 -or $run.Output -notmatch '(?i)cpu') { Fail "$What`: wabcut-infer --devices: $($run.Output)" }
    }
    $embed = Join-Path $bin 'wabcut-embed.exe'
    if (Test-Path $embed) {
        $run = Invoke-Program $embed @() -InputText "{`"type`":`"hello`",`"protocol`":1}`n{`"type`":`"shutdown`"}`n" `
            -Environment $minimalEnvironment -TimeoutSeconds 120
        if ($run.ExitCode -ne 0 -or $run.Output -notmatch '"engine":"onnxruntime"') { Fail "$What`: wabcut-embed hello: $($run.Output)" }
    }
    New-Item -ItemType Directory -Force $Scratch | Out-Null
    $project = Join-Path $Scratch 'tiny.wabcut'
    Copy-Item $SmokeProject $project -Force
    $title = '{"command":"title.add","params":{"at":0,"duration":"00:00:01:00","text":"Wabcut"}}'
    $run = Invoke-Program $cli @('cmd', $project, '--save', '--json', $title) -Environment $minimalEnvironment
    if ($run.ExitCode -ne 0) { Fail "$What`: wabcut-cli cmd title.add failed ($($run.ExitCode)): $($run.Output)" }
    $movie = Join-Path $Scratch 'tiny.mp4'
    $run = Invoke-Program $cli @('render', $project, $movie, '--encoder', 'libx264') -Environment $minimalEnvironment
    if ($run.ExitCode -ne 0 -or -not (Test-Path $movie) -or $run.Output -notmatch 'video: libx264, 25 frames') {
        Fail "$What`: headless render failed ($($run.ExitCode)): $($run.Output)"
    }
    Write-Host "$What`: headless render of the tiny project: $(Format-Size (Get-Item $movie).Length)"
}

# Size and contents of a tree: files and bytes per top-level folder, executables, the AI helpers' run-time modules.
function Write-TreeReport([string]$Root, [string]$What) {
    $files = @(Get-ChildItem $Root -Recurse -File)
    $total = ($files | Measure-Object Length -Sum).Sum
    Write-Host "$What contents: $($files.Count) files, $(Format-Size $total)"
    $files | Group-Object { $_.FullName.Substring($Root.Length).TrimStart('\').Split('\')[0] } | Sort-Object Name |
        ForEach-Object {
            $bytes = ($_.Group | Measure-Object Length -Sum).Sum
            Write-Host ('    {0,-28} {1,5} files  {2,10:N1} MiB' -f $_.Name, $_.Count, ($bytes / 1MB))
        }
    $bin = Join-Path $Root 'bin'
    Write-Host "    executables: $((Get-ChildItem $bin -Filter *.exe | Sort-Object Name | ForEach-Object Name) -join ', ')"
    $helperModules = @(Get-ChildItem $bin -File | Where-Object { $_.Name -match '^(ggml|whisper|vulkan-1|onnxruntime)' })
    if ($helperModules.Count) {
        $bytes = ($helperModules | Measure-Object Length -Sum).Sum
        Write-Host "    AI helper runtime: $($helperModules.Count) files, $(Format-Size $bytes)"
    }
}

# ---- 1. Build the packages ---------------------------------------------------------------------------------------
$packageDir = Join-Path $BuildDir 'package'
$work = Join-Path $BuildDir 'package-smoke'
if (Test-Path $work) { Remove-Item $work -Recurse -Force }
New-Item -ItemType Directory -Force $work | Out-Null
$generatorList = @($Generators -split ';' | Where-Object { $_ })
foreach ($generator in $generatorList) {
    $started = Get-Date
    $run = Invoke-Program $CPack @('--config', (Join-Path $BuildDir 'CPackConfig.cmake'), '-G', $generator,
                                   '-B', $packageDir) -TimeoutSeconds 1500
    if ($run.ExitCode -ne 0) { Fail "cpack -G $generator failed ($($run.ExitCode)):`n$($run.Output)" }
    Write-Host ("cpack -G {0}: {1:N0} s" -f $generator, ((Get-Date) - $started).TotalSeconds)
}

# ---- 2. Portable ZIP ---------------------------------------------------------------------------------------------
$zip = Join-Path $packageDir "$PackageBaseName-portable.zip"
if (-not (Test-Path $zip)) { Fail "no portable ZIP at $zip" }
Write-Host "portable ZIP: $zip, $(Format-Size (Get-Item $zip).Length)"
$unpacked = Join-Path $work 'zip'
Expand-Archive -LiteralPath $zip -DestinationPath $unpacked
$zipRoot = Join-Path $unpacked "$PackageBaseName-portable"
if (-not (Test-Path $zipRoot)) { Fail "the ZIP has no top-level folder $PackageBaseName-portable" }
Write-TreeReport $zipRoot 'portable ZIP'
Test-Tree $zipRoot 'portable ZIP' (Join-Path $work 'zip-run')

# ---- 3. MSI ------------------------------------------------------------------------------------------------------
if ($generatorList -notcontains 'WIX') {
    Write-Host 'SKIPPED the MSI checks: WiX is not configured (cmake/packaging/Install-WixToolset.ps1, docs/release.md)'
    exit $skipCode
}
$msi = Join-Path $packageDir "$PackageBaseName.msi"
if (-not (Test-Path $msi)) { Fail "no MSI at $msi" }
Write-Host "MSI: $msi, $(Format-Size (Get-Item $msi).Length)"

$installer = New-Object -ComObject WindowsInstaller.Installer
$database = Invoke-ComMethod $installer 'OpenDatabase' @($msi, 0)
$property = @{}
foreach ($row in (Get-MsiRows $database 'SELECT `Property`, `Value` FROM `Property`' 2)) { $property[$row[0]] = $row[1] }
$productCode = $property['ProductCode']
if ($property['UpgradeCode'] -ne "{$UpgradeCode}") { Fail "the MSI's UpgradeCode is $($property['UpgradeCode']), expected {$UpgradeCode}" }
if ($property['ProductVersion'] -ne $Version) { Fail "the MSI's ProductVersion is $($property['ProductVersion']), expected $Version" }
if ($property.ContainsKey('ALLUSERS') -and $property['ALLUSERS']) { Fail "the MSI sets ALLUSERS=$($property['ALLUSERS']): not a per-user package" }
# Summary information, Word Count bit 3: "elevated privileges are not required" (a per-user package).
$summary = Get-ComProperty $database 'SummaryInformation' @(0)
$wordCount = [int](Get-ComProperty $summary 'Property' @(15))
if (($wordCount -band 8) -eq 0) { Fail "the MSI asks for elevation (Word Count $wordCount)" }
$fileRows = Get-MsiRows $database 'SELECT `File`, `FileSize`, `FileName` FROM `File`' 3
$msiBytes = ($fileRows | ForEach-Object { [long]$_[1] } | Measure-Object -Sum).Sum
# Only the install tree: none of the WiX generator's own working files (FileName is "SHORT~1|long name" or "name").
$stray = @($fileRows | Where-Object { ($_[2] -split '\|')[-1] -match '\.(wxs|wxi|wixobj|rtf)$' })
if ($stray.Count) { Fail "the MSI installs WiX working files, e.g. $(($stray[0][2] -split '\|')[-1])" }
$featureRows = Get-MsiRows $database 'SELECT `Feature`, `Level` FROM `Feature`' 2
$features = ($featureRows | ForEach-Object { "$($_[0]) (level $($_[1]))" }) -join ', '
Write-Host "MSI contents: $($fileRows.Count) files, $(Format-Size $msiBytes) installed; features: $features"
# The uninstaller only removes what the package installed: no RemoveFile row may name files by wildcard or at all
# (folder rows, with an empty FileName, only remove folders once they are empty).
foreach ($row in (Get-MsiRows $database 'SELECT `FileKey`, `FileName` FROM `RemoveFile`' 2)) {
    if ($row[1]) { Fail "the MSI's uninstaller would delete files by name: RemoveFile $($row[0]) = '$($row[1])'" }
}
[System.Runtime.InteropServices.Marshal]::ReleaseComObject($database) | Out-Null

$related = Get-ComProperty $installer 'RelatedProducts' @("{$UpgradeCode}")
if ((Get-ComProperty $related 'Count') -gt 0) {
    Write-Host "SKIPPED the MSI install round trip: Wabcut is installed for this user ($(Get-ComProperty $related 'Item' @(0))); it is left alone"
    exit $skipCode
}
$elevated = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
Write-Host "installing as $([Security.Principal.WindowsIdentity]::GetCurrent().Name), elevated: $elevated"

# Opt in to the association only when the user has none, so a real association is never replaced or removed.
$classes = 'HKCU:\Software\Classes'
$associate = -not (Test-Path "$classes\.wabcut") -and -not (Test-Path "$classes\Wabcut.Project")

# msiexec's own command-line syntax: PROPERTY="value", no C-runtime escaping.
function Invoke-Msiexec([string]$Arguments, [string]$Log) {
    for ($attempt = 1; $attempt -le 20; $attempt++) {
        $run = Invoke-Program "$env:SystemRoot\System32\msiexec.exe" -RawArguments "$Arguments /qn /norestart /l*v `"$Log`"" -TimeoutSeconds 900
        if ($run.ExitCode -ne 1618) { return $run.ExitCode } # 1618: another installation is in progress
        Start-Sleep -Seconds 15
    }
    return 1618
}

# The round trip runs in a temporary folder of its own, not in the build tree: Windows Installer refuses to write its
# log or install through a junction or mount point (error 1622), and build folders are often behind one.
$msiWork = Join-Path ([System.IO.Path]::GetTempPath()) "wabcut-package-smoke-$PID"
if (Test-Path $msiWork) { Remove-Item $msiWork -Recurse -Force }
New-Item -ItemType Directory -Force $msiWork | Out-Null
Write-Host "MSI round trip in $msiWork"
$msiCopy = Join-Path $msiWork "$PackageBaseName.msi"
Copy-Item -LiteralPath $msi -Destination $msiCopy
$installRoot = Join-Path $msiWork 'msi root'
$installArgs = "/i `"$msiCopy`" INSTALL_ROOT=`"$installRoot`""
if ($associate) { $installArgs += ' WABCUT_ASSOCIATE=1' }
$started = Get-Date
$code = Invoke-Msiexec $installArgs (Join-Path $msiWork 'install.log')
if ($code -ne 0) { Fail "msiexec /i failed with $code (log: $msiWork\install.log)" }
Write-Host ("per-user install into '{0}': {1:N0} s" -f $installRoot, ((Get-Date) - $started).TotalSeconds)
$uninstalled = $false
try {
    if ((Get-ComProperty $installer 'ProductState' @($productCode)) -ne 5) { Fail "the product $productCode is not registered as installed" }
    if (-not (Test-Path (Join-Path $installRoot 'bin\wabcut.exe'))) {
        Fail "the MSI did not install bin\wabcut.exe in its folder; it holds: $((Get-ChildItem $installRoot | ForEach-Object Name) -join ', ')"
    }
    $installedFiles = @(Get-ChildItem $installRoot -Recurse -File | ForEach-Object FullName)
    Write-TreeReport $installRoot 'installed MSI'

    $shortcut = Join-Path ([Environment]::GetFolderPath('Programs')) 'Wabcut.lnk'
    if (-not (Test-Path $shortcut)) { Fail "no Start menu entry at $shortcut" }
    $target = (New-Object -ComObject WScript.Shell).CreateShortcut($shortcut).TargetPath
    if ($target -ne (Join-Path $installRoot 'bin\wabcut.exe')) { Fail "the Start menu entry points at '$target'" }
    Write-Host "Start menu entry: $shortcut -> $target"
    if ($associate) {
        $command = (Get-ItemProperty "$classes\Wabcut.Project\shell\open\command").'(default)'
        if ((Get-ItemProperty "$classes\.wabcut").'(default)' -ne 'Wabcut.Project' -or
            $command -ne "`"$installRoot\bin\wabcut.exe`" `"%1`"") { Fail "the .wabcut association is wrong: $command" }
        Write-Host ".wabcut association: $command"
    } else {
        Write-Host '.wabcut association: not tested (this user already has one; it was not touched)'
    }

    # The folder is remembered, so the uninstall below finds the files there (not in the default folder).
    $remembered = (Get-ItemProperty 'HKCU:\Software\Wabcut\Installer' -ErrorAction SilentlyContinue).InstallRoot
    if ($remembered -ne "$installRoot\") { Fail "the install folder is not remembered (HKCU\Software\Wabcut\Installer: '$remembered')" }

    Test-Tree $installRoot 'installed MSI' (Join-Path $msiWork 'msi-run')

    # What a user may leave in the program folder must survive the uninstall.
    $userProject = Join-Path $installRoot 'My Project.wabcut'
    $userNote = Join-Path $installRoot 'bin\user note.txt'
    Set-Content -LiteralPath $userProject -Value '{"user":"data"}'
    Set-Content -LiteralPath $userNote -Value 'not the installer''s'

    $code = Invoke-Msiexec "/x $productCode" (Join-Path $msiWork 'uninstall.log')
    $uninstalled = $true
    if ($code -ne 0) { Fail "msiexec /x failed with $code (log: $msiWork\uninstall.log)" }
    if ((Get-ComProperty $installer 'ProductState' @($productCode)) -ne -1) { Fail "the product $productCode is still registered" }
    $left = @($installedFiles | Where-Object { Test-Path -LiteralPath $_ })
    if ($left.Count) { Fail "the uninstaller left $($left.Count) program files, e.g. $($left[0])" }
    foreach ($kept in $userProject, $userNote) {
        if (-not (Test-Path -LiteralPath $kept)) { Fail "the uninstaller deleted the user's file $kept" }
    }
    if (Test-Path $shortcut) { Fail "the uninstaller left the Start menu entry" }
    if (Test-Path 'HKCU:\Software\Wabcut\Installer') { Fail 'the uninstaller left HKCU\Software\Wabcut\Installer' }
    if ($associate -and ((Test-Path "$classes\.wabcut") -or (Test-Path "$classes\Wabcut.Project"))) {
        Fail 'the uninstaller left the .wabcut association'
    }
    Write-Host "uninstall: $($installedFiles.Count) program files, the Start menu entry and the association removed; the user's files kept"
} finally {
    if (-not $uninstalled) {
        Invoke-Msiexec "/x $productCode" (Join-Path $msiWork 'cleanup-uninstall.log') | Out-Null
    }
}
Remove-Item $msiWork -Recurse -Force -ErrorAction SilentlyContinue # kept (with the msiexec logs) when a check failed
Write-Host 'install.package: passed'
exit 0
