<#
.SYNOPSIS
    Signs Wabcut's Windows files (.exe, .dll, .msi) with Authenticode (docs/release.md, "Code signing").

.DESCRIPTION
    Two ways to sign, picked by -Mode or by the environment the release workflow sets up:

      CertStore       A code-signing certificate in the Windows certificate store, found by its thumbprint
                      (-Thumbprint or WABCUT_SIGN_CERT_THUMBPRINT; CurrentUser\My, else LocalMachine\My): an OV or EV
                      certificate on this machine, or one the release workflow imported from its PFX secret.
      TrustedSigning  Azure Trusted Signing: WABCUT_TRUSTED_SIGNING_ENDPOINT (e.g. https://wus2.codesigning.azure.net),
                      WABCUT_TRUSTED_SIGNING_ACCOUNT, WABCUT_TRUSTED_SIGNING_PROFILE, the signing client's dlib
                      (WABCUT_TRUSTED_SIGNING_DLIB: Azure.CodeSigning.Dlib.dll from the Microsoft.Trusted.Signing.Client
                      package) and an Azure sign-in it can use (AZURE_TENANT_ID, AZURE_CLIENT_ID, AZURE_CLIENT_SECRET, or
                      any other DefaultAzureCredential source).

    Auto (the default) uses TrustedSigning when its endpoint is set, else CertStore when a thumbprint is set, else signs
    nothing and exits 0 with a notice (with -Require it fails instead). No secret is read from or written to the
    repository: everything comes from the environment or the certificate store.

    A folder is searched for .exe, .dll and .msi files. A file that already carries a valid signature (Qt's libraries,
    Microsoft's runtime and Direct3D DLLs) keeps it unless -Force. Every signature is SHA-256 with an RFC 3161 timestamp,
    so it stays valid after the certificate expires, and is verified afterwards (signtool verify /pa).

.EXAMPLE
    pwsh tools/sign-release.ps1 -Path build\win-msvc\package\wabcut-0.5.0-windows-x64.msi
.EXAMPLE
    pwsh tools/sign-release.ps1 -Path $env:RUNNER_TEMP\staging -Require
.EXAMPLE
    pwsh tools/sign-release.ps1 -Path build\win-msvc\bin -WhatIf     # lists what would be signed
#>
param(
    [Parameter(Mandatory)] [string[]]$Path,
    [ValidateSet('Auto', 'CertStore', 'TrustedSigning')] [string]$Mode = 'Auto',
    [string]$Thumbprint = $env:WABCUT_SIGN_CERT_THUMBPRINT,
    [string]$TimestampUrl = '',
    [string]$Description = 'Wabcut',
    [string]$DescriptionUrl = 'https://github.com/sorsama/wabcut',
    [switch]$Require,
    [switch]$Force,
    [switch]$WhatIf
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Fail([string]$message) {
    Write-Host "sign-release: $message" -ForegroundColor Red
    exit 1
}

# ---- Which way ----------------------------------------------------------------------------------------------------
if ($Mode -eq 'Auto') {
    if ($env:WABCUT_TRUSTED_SIGNING_ENDPOINT) { $Mode = 'TrustedSigning' }
    elseif ($Thumbprint) { $Mode = 'CertStore' }
    else {
        if ($Require) { Fail 'no signing is configured (WABCUT_TRUSTED_SIGNING_ENDPOINT or WABCUT_SIGN_CERT_THUMBPRINT)' }
        Write-Host 'sign-release: no certificate configured; the files stay unsigned.'
        exit 0
    }
}

# ---- What -----------------------------------------------------------------------------------------------------------
$files = [System.Collections.Generic.List[string]]::new()
foreach ($p in $Path) {
    if (-not (Test-Path -LiteralPath $p)) { Fail "$p does not exist" }
    $item = Get-Item -LiteralPath $p
    if ($item.PSIsContainer) {
        Get-ChildItem -LiteralPath $item.FullName -Recurse -File |
            Where-Object { $_.Extension -in '.exe', '.dll', '.msi' } |
            ForEach-Object { $files.Add($_.FullName) }
    } else {
        $files.Add($item.FullName)
    }
}
$toSign = [System.Collections.Generic.List[string]]::new()
$kept = 0
foreach ($f in $files) {
    $existing = Get-AuthenticodeSignature -LiteralPath $f
    if (-not $Force -and $existing.Status -eq 'Valid') { $kept++; continue }
    $toSign.Add($f)
}
Write-Host ("sign-release: {0} file(s) to sign with {1}, {2} already signed by their vendor" -f $toSign.Count, $Mode, $kept)
if ($WhatIf) {
    $toSign | ForEach-Object { Write-Host "  would sign $_" }
    exit 0
}
if ($toSign.Count -eq 0) { exit 0 }

# ---- signtool (the newest Windows SDK's, or WABCUT_SIGNTOOL) --------------------------------------------------------
$signtool = $env:WABCUT_SIGNTOOL
if (-not $signtool) {
    $kits = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
    $found = Get-ChildItem -Path $kits -Filter signtool.exe -Recurse -ErrorAction SilentlyContinue |
        Where-Object { $_.Directory.Name -eq 'x64' } |
        Sort-Object { [version]($_.Directory.Parent.Name -replace '[^0-9.]', '') } -Descending -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($found) { $signtool = $found.FullName }
    elseif (Get-Command signtool.exe -ErrorAction SilentlyContinue) { $signtool = (Get-Command signtool.exe).Source }
}
if (-not $signtool -or -not (Test-Path -LiteralPath $signtool)) { Fail 'signtool.exe not found (install the Windows SDK, or set WABCUT_SIGNTOOL)' }

$common = @('sign', '/fd', 'SHA256', '/td', 'SHA256', '/d', $Description, '/du', $DescriptionUrl)
$metadata = $null
switch ($Mode) {
    'CertStore' {
        if (-not $Thumbprint) { Fail 'CertStore signing needs -Thumbprint or WABCUT_SIGN_CERT_THUMBPRINT' }
        $Thumbprint = ($Thumbprint -replace '\s', '').ToUpperInvariant()
        $store = @()
        if (-not (Test-Path "Cert:\CurrentUser\My\$Thumbprint")) {
            if (Test-Path "Cert:\LocalMachine\My\$Thumbprint") { $store = @('/sm') }
            else { Fail "no certificate with thumbprint $Thumbprint in CurrentUser\My or LocalMachine\My" }
        }
        if (-not $TimestampUrl) { $TimestampUrl = 'http://timestamp.digicert.com' }
        $arguments = $common + $store + @('/sha1', $Thumbprint, '/tr', $TimestampUrl)
    }
    'TrustedSigning' {
        foreach ($name in 'WABCUT_TRUSTED_SIGNING_ENDPOINT', 'WABCUT_TRUSTED_SIGNING_ACCOUNT', 'WABCUT_TRUSTED_SIGNING_PROFILE',
                          'WABCUT_TRUSTED_SIGNING_DLIB') {
            if (-not [Environment]::GetEnvironmentVariable($name)) { Fail "Trusted Signing needs $name" }
        }
        if (-not (Test-Path -LiteralPath $env:WABCUT_TRUSTED_SIGNING_DLIB)) { Fail "no $env:WABCUT_TRUSTED_SIGNING_DLIB" }
        $metadata = Join-Path ([IO.Path]::GetTempPath()) ("wabcut-signing-{0}.json" -f [guid]::NewGuid())
        @{
            Endpoint               = $env:WABCUT_TRUSTED_SIGNING_ENDPOINT
            CodeSigningAccountName = $env:WABCUT_TRUSTED_SIGNING_ACCOUNT
            CertificateProfileName = $env:WABCUT_TRUSTED_SIGNING_PROFILE
            CorrelationId          = "wabcut-$([guid]::NewGuid())"
        } | ConvertTo-Json | Set-Content -LiteralPath $metadata -Encoding utf8
        if (-not $TimestampUrl) { $TimestampUrl = 'http://timestamp.acs.microsoft.com' }
        $arguments = $common + @('/tr', $TimestampUrl, '/dlib', $env:WABCUT_TRUSTED_SIGNING_DLIB, '/dmdf', $metadata)
    }
}

# ---- Sign in batches (the command line has a length limit; timestamp servers are retried) ----------------------------
try {
    $batch = 40
    for ($i = 0; $i -lt $toSign.Count; $i += $batch) {
        $chunk = @($toSign[$i..([Math]::Min($i + $batch, $toSign.Count) - 1)])
        $ok = $false
        for ($attempt = 1; $attempt -le 3 -and -not $ok; $attempt++) {
            & $signtool @arguments @chunk
            $ok = $LASTEXITCODE -eq 0
            if (-not $ok -and $attempt -lt 3) {
                Write-Host "sign-release: signtool failed ($LASTEXITCODE), retrying in $(10 * $attempt) s"
                Start-Sleep -Seconds (10 * $attempt)
            }
        }
        if (-not $ok) { Fail "signtool could not sign: $($chunk -join ', ')" }
    }
} finally {
    if ($metadata) { Remove-Item -LiteralPath $metadata -ErrorAction SilentlyContinue }
}

# ---- Check every signature ------------------------------------------------------------------------------------------
foreach ($f in $toSign) {
    & $signtool verify /pa /q $f | Out-Null
    if ($LASTEXITCODE -ne 0) { Fail "the signature of $f does not verify" }
}
Write-Host "sign-release: signed and verified $($toSign.Count) file(s)"
exit 0
