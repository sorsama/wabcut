<#
.SYNOPSIS
    RC.1 installer checks (docs/internal/release-checklist.md): the packages from cpack, CTest install.package, and a first launch
    of the installed app with no visible window.

.DESCRIPTION
    1. ctest -R install.package: cpack builds the portable ZIP and the MSI, runs the unpacked ZIP, installs the MSI per
       user into a folder under %TEMP%, runs it, uninstalls it and checks what is left (cmake/packaging/PackageSmokeTest.ps1).
    2. A first launch: the MSI installed per user into %TEMP%\wabcut-rc-first-launch, wabcut.exe started on the offscreen
       platform with the startup probe (src/app/src/StartupProbe.h: WABCUT_PERF_STARTUP writes the milestones, quits at the
       first frame), with Windows' own folders as PATH and settings in a scratch folder; then uninstalled, and the
       folder must be empty of program files.
    3. With -Zip: the portable ZIP unpacked into %TEMP%\wabcut-rc-portable, wabcut-cli --version and the same offscreen
       first launch from it, then the folder removed.
    Writes test-output/release/install.json (or <Out>\install.json) and prints PASS / FAIL lines.

    Another build's packages, read only (RC.2: the release packages): -SkipPackageTest (install.package runs cpack in
    the build folder), -PackageDir <that build>\package -Out <scratch folder> -Zip.

.PARAMETER BuildDir
    The configured build folder (build\win-msvc).
#>
[CmdletBinding()]
param([string]$BuildDir = 'build\win-msvc', [switch]$SkipPackageTest,
      [string]$QtDir = 'C:\Qt\6.11.3\msvc2022_64', [string]$PackageDir = '', [string]$Out = '', [switch]$Zip)

$ErrorActionPreference = 'Stop'
$BuildDir = [System.IO.Path]::GetFullPath($BuildDir)
$out = if ($Out) { [System.IO.Path]::GetFullPath($Out) } else { Join-Path $BuildDir 'test-output\release' }
$packages = if ($PackageDir) { [System.IO.Path]::GetFullPath($PackageDir) } else { Join-Path $BuildDir 'package' }
New-Item -ItemType Directory -Force $out | Out-Null
$results = [System.Collections.Generic.List[object]]::new()
function Record([string]$Step, [bool]$Ok, [string]$Evidence) {
    $results.Add([pscustomobject]@{ step = $Step; ok = $Ok; evidence = $Evidence })
    Write-Host ("  [{0}] {1}: {2}" -f ($(if ($Ok) { 'PASS' } else { 'FAIL' })), $Step, $Evidence)
}

# Starts a program with Windows' own folders as PATH and no Qt variables; returns { finished, exitCode, seconds, output }.
function Start-Clean([string]$Exe, [string]$Arguments, [hashtable]$Environment, [int]$TimeoutSeconds = 120) {
    $psi = [System.Diagnostics.ProcessStartInfo]::new($Exe)
    $psi.Arguments = $Arguments
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.Environment['PATH'] = "$env:SystemRoot\System32;$env:SystemRoot"
    foreach ($name in 'QT_PLUGIN_PATH', 'QML_IMPORT_PATH', 'QML2_IMPORT_PATH', 'QT_QPA_PLATFORM_PLUGIN_PATH', 'QT_QPA_PLATFORM') { $psi.Environment.Remove($name) | Out-Null }
    foreach ($key in $Environment.Keys) { $psi.Environment[$key] = $Environment[$key] }
    $started = Get-Date
    $p = [System.Diagnostics.Process]::Start($psi)
    $stdout = $p.StandardOutput.ReadToEndAsync()
    $stderr = $p.StandardError.ReadToEndAsync()
    $finished = $p.WaitForExit($TimeoutSeconds * 1000)
    if (-not $finished) { $p.Kill() }
    [pscustomobject]@{ finished = $finished; exitCode = $(if ($finished) { $p.ExitCode } else { 'timeout' })
                       seconds = ((Get-Date) - $started).TotalSeconds; output = ($stdout.Result + $stderr.Result).Trim() }
}

# The app from an installed or unpacked tree on the offscreen platform with the startup probe, quitting at its first
# frame. The package ships only the Windows platform plugin; the offscreen one (no window) comes from the same Qt in a
# scratch folder, so everything else runs from the tree.
function Test-FirstLaunch([string]$Step, [string]$Root, [string]$Work) {
    $report = Join-Path $Work 'first-launch.json'
    $settings = Join-Path $Work 'settings'
    $platforms = Join-Path $Work 'qpa\platforms'
    New-Item -ItemType Directory -Force $settings, $platforms | Out-Null
    Copy-Item (Join-Path $QtDir 'plugins\platforms\qoffscreen.dll') $platforms
    $run = Start-Clean (Join-Path $Root 'bin\wabcut.exe') '' @{
        QT_QPA_PLATFORM_PLUGIN_PATH = $platforms; QT_QPA_PLATFORM = 'offscreen'; WABCUT_PERF_STARTUP = $report
        WABCUT_PERF_STARTUP_QUIT = '1'; WABCUT_PERF_SETTINGS_DIR = $settings; WABCUT_AUDIO_BACKEND = 'null' }
    $milestones = if (Test-Path $report) { (Get-Content $report -Raw | ConvertFrom-Json) } else { $null }
    $json = if ($milestones) { $milestones | ConvertTo-Json -Depth 5 -Compress } else { 'none' }
    Record $Step ($run.finished -and $run.exitCode -eq 0 -and $json -match 'firstFrame') `
        ("exit {0} in {1:N1} s; report: {2}" -f $run.exitCode, $run.seconds, $json.Substring(0, [Math]::Min(300, $json.Length)))
}

function Test-Docs([string]$Step, [string]$Root) {
    $docs = @(Get-ChildItem $Root -Recurse -File -Include '*.md', '*.html', '*.txt' -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -notmatch '\\(qml|plugins|translations)\\' })
    $names = $docs | ForEach-Object { $_.Name }
    $missing = @('getting-started.md', 'release-notes-0.1.md' | Where-Object { $_ -notin $names })
    Record $Step ($missing.Count -eq 0) ("{0}; missing: {1}" -f (($docs | ForEach-Object { $_.FullName.Substring($Root.Length + 1) }) -join ', '),
                                       $(if ($missing) { $missing -join ', ' } else { 'none' }))
}

function Test-Cli([string]$Step, [string]$Root) {
    $run = Start-Clean (Join-Path $Root 'bin\wabcut-cli.exe') '--version' @{} 60
    Record $Step ($run.finished -and $run.exitCode -eq 0 -and $run.output) ("exit {0}: {1}" -f $run.exitCode, ($run.output -split "`n")[0])
}

# ---- 1. install.package ----------------------------------------------------------------------------------------
if (-not $SkipPackageTest) {
$log = Join-Path $out 'install-package.log'
$started = Get-Date
& ctest --test-dir $BuildDir -R '^install\.package$' --output-on-failure *> $log
$code = $LASTEXITCODE
$text = Get-Content $log -Raw
$sizes = ([regex]::Matches($text, '(?m)^.*(package|MSI|ZIP).*(MiB|MB).*$') | Select-Object -First 6 | ForEach-Object { $_.Value.Trim() }) -join '; '
Record 'install.package (cpack ZIP + MSI, unpacked ZIP run, per-user MSI round trip in %TEMP%)' ($code -eq 0 -and $text -notmatch 'SKIPPED the MSI') `
    ("exit $code in {0:N0} s; {1}" -f ((Get-Date) - $started).TotalSeconds, $sizes)
}

# ---- 2. first launch of the installed app, offscreen ------------------------------------------------------------
$msi = Get-ChildItem $packages -Filter *.msi -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
$installed = @(Get-ChildItem 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall', 'HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall' -ErrorAction SilentlyContinue |
    ForEach-Object { Get-ItemProperty $_.PSPath } | Where-Object { $_.PSObject.Properties['DisplayName'] -and $_.DisplayName -match '^Wabcut' })
if (-not $msi) {
    Record 'first launch of the installed app' $false "no MSI in $packages (WiX missing?)"
} elseif ($installed.Count) {
    # The same UpgradeCode: installing into %TEMP% would upgrade (and the uninstall remove) the real installation.
    Record 'per-user install from %TEMP%' $false "not run: $($installed[0].DisplayName) $($installed[0].DisplayVersion) is installed on this computer"
} else {
    $work = Join-Path ([System.IO.Path]::GetTempPath()) 'wabcut-rc-first-launch'
    if (Test-Path $work) { Remove-Item $work -Recurse -Force }
    New-Item -ItemType Directory -Force $work | Out-Null
    $msiCopy = Join-Path $work $msi.Name
    Copy-Item $msi.FullName $msiCopy
    $root = Join-Path $work 'Wabcut'
    $p = Start-Process "$env:SystemRoot\System32\msiexec.exe" -ArgumentList "/i `"$msiCopy`" INSTALL_ROOT=`"$root`" /qn /norestart /l*v `"$work\install.log`"" -Wait -PassThru
    if ($p.ExitCode -ne 0) {
        Record 'per-user install from %TEMP%' $false "msiexec /i exit $($p.ExitCode) (log $work\install.log)"
    } else {
        Record 'per-user install from %TEMP%' $true ("{0} ({1:N0} MB, {2}) installed into {3}" -f $msi.Name, ($msi.Length / 1MB), $msi.LastWriteTime.ToString('HH:mm'), $root)
        Test-Cli 'the installed wabcut-cli --version (Windows-only PATH)' $root
        Test-Docs 'the installed docs (getting-started.md, release-notes-0.1.md)' $root
        Test-FirstLaunch 'first launch offscreen (no visible window), quits at its first frame' $root $work
        $p = Start-Process "$env:SystemRoot\System32\msiexec.exe" -ArgumentList "/x `"$msiCopy`" /qn /norestart /l*v `"$work\uninstall.log`"" -Wait -PassThru
        $left = @(Get-ChildItem $root -Recurse -File -ErrorAction SilentlyContinue | Where-Object { $_.Extension -in '.exe', '.dll' })
        Record 'per-user uninstall' ($p.ExitCode -eq 0 -and $left.Count -eq 0) "msiexec /x exit $($p.ExitCode); program files left: $($left.Count)"
    }
}

# ---- 3. the portable ZIP, unpacked into %TEMP% ------------------------------------------------------------------
if ($Zip) {
    $zipFile = Get-ChildItem $packages -Filter *.zip -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $zipFile) {
        Record 'portable ZIP' $false "no ZIP in $packages"
    } else {
        $work = Join-Path ([System.IO.Path]::GetTempPath()) 'wabcut-rc-portable'
        if (Test-Path $work) { Remove-Item $work -Recurse -Force }
        New-Item -ItemType Directory -Force $work | Out-Null
        Expand-Archive -LiteralPath $zipFile.FullName -DestinationPath (Join-Path $work 'unpacked')
        $exe = Get-ChildItem (Join-Path $work 'unpacked') -Recurse -Filter wabcut.exe | Where-Object { $_.Directory.Name -eq 'bin' } | Select-Object -First 1
        if (-not $exe) {
            Record 'portable ZIP unpacked into %TEMP%' $false "no bin\wabcut.exe in $($zipFile.Name)"
        } else {
            $root = $exe.Directory.Parent.FullName
            Record 'portable ZIP unpacked into %TEMP%' $true ("{0} ({1:N0} MB, {2}) unpacked into {3}" -f $zipFile.Name, ($zipFile.Length / 1MB), $zipFile.LastWriteTime.ToString('HH:mm'), $root)
            Test-Cli 'the portable wabcut-cli --version (Windows-only PATH)' $root
            Test-Docs 'the portable docs (getting-started.md, release-notes-0.1.md)' $root
            Test-FirstLaunch 'portable first launch offscreen (no visible window), quits at its first frame' $root $work
        }
        Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
        Record 'portable folder removed' (-not (Test-Path $work)) $work
    }
}

$results | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $out 'install.json') -Encoding utf8
$failed = @($results | Where-Object { -not $_.ok }).Count
Write-Host "$failed failed step(s); results in $out\install.json"
exit $(if ($failed) { 1 } else { 0 })
