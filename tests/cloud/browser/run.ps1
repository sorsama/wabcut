# Checks the generated review page in a real browser (Edge or Chrome, headless). By hand: it is not part of ctest, which has no browser.
#   1. Run the cloud tests once ("The review page is written for the browser check" writes <build>/test-output/cloud/browser).
#   2. pwsh -File tests/cloud/browser/run.ps1 -Build build\win-msvc
# It prints what the page did (frame stepping, marks, comments, the file it gives the reviewer, reload, the expired page) and every
# console message, script error and Content Security Policy violation the browser reported while it ran.
# The browser is driven over its DevTools port: a GUI browser prints nothing on a pipe, so --dump-dom gives no result on Windows.
param(
    [string]$Build = 'build\win-msvc',
    [string]$Browser = '',
    [int]$TimeoutSeconds = 90
)
$ErrorActionPreference = 'Stop'
$folder = Join-Path $Build 'test-output\cloud\browser'
if (-not (Test-Path (Join-Path $folder 'review\index.html'))) { throw "no page in ${folder}: run the cloud tests first" }
Copy-Item (Join-Path $PSScriptRoot 'driver.html') (Join-Path $folder 'driver.html') -Force
if (-not $Browser) {
    $Browser = @('C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe', 'C:\Program Files\Microsoft\Edge\Application\msedge.exe',
                 'C:\Program Files\Google\Chrome\Application\chrome.exe') | Where-Object { Test-Path $_ } | Select-Object -First 1
}
if (-not $Browser) { throw 'no Edge or Chrome found: pass -Browser' }
$profile = Join-Path ([IO.Path]::GetTempPath()) ('wabcut-review-check-' + [guid]::NewGuid().ToString('N'))
$url = ([Uri](Resolve-Path (Join-Path $folder 'driver.html')).Path).AbsoluteUri
$port = Get-Random -Minimum 20000 -Maximum 40000

$proc = Start-Process -FilePath $Browser -PassThru -WindowStyle Hidden -ArgumentList @(
    '--headless=new', '--disable-gpu', '--no-first-run', "--user-data-dir=$profile", '--allow-file-access-from-files',
    '--autoplay-policy=no-user-gesture-required', "--remote-debugging-port=$port", 'about:blank')
$ws = $null
try {
    $targets = $null
    for ($i = 0; $i -lt 100 -and -not $targets; $i++) {
        try { $targets = Invoke-RestMethod "http://127.0.0.1:$port/json/list" } catch { Start-Sleep -Milliseconds 200 }
    }
    $page = $targets | Where-Object { $_.type -eq 'page' } | Select-Object -First 1
    if (-not $page) { throw 'the browser did not open a page' }
    $cancel = [Threading.CancellationTokenSource]::new([TimeSpan]::FromSeconds($TimeoutSeconds))
    $ws = [System.Net.WebSockets.ClientWebSocket]::new()
    [void]$ws.ConnectAsync([Uri]$page.webSocketDebuggerUrl, $cancel.Token).GetAwaiter().GetResult()

    $events = [System.Collections.Generic.List[object]]::new()
    $script:nextId = 0
    function Invoke-Cdp([string]$method, [hashtable]$params = @{}) {
        $script:nextId++
        $id = $script:nextId
        $bytes = [Text.Encoding]::UTF8.GetBytes((@{ id = $id; method = $method; params = $params } | ConvertTo-Json -Depth 8 -Compress))
        [void]$ws.SendAsync([ArraySegment[byte]]::new($bytes), 'Text', $true, $cancel.Token).GetAwaiter().GetResult()
        if ($env:WABCUT_CDP_TRACE) { [Console]::Error.WriteLine("-> $method") }
        $buffer = New-Object byte[] 65536
        while ($true) {
            $text = [Text.StringBuilder]::new()
            do {
                $r = $ws.ReceiveAsync([ArraySegment[byte]]::new($buffer), $cancel.Token).GetAwaiter().GetResult()
                [void]$text.Append([Text.Encoding]::UTF8.GetString($buffer, 0, $r.Count))
            } until ($r.EndOfMessage)
            $message = $text.ToString() | ConvertFrom-Json
            if ($env:WABCUT_CDP_TRACE) { [Console]::Error.WriteLine("<- id=$($message.id) $($message.method)") }
            if ($message.id -eq $id) { return $message }
            if ($message.method) {
                $events.Add($message)
                if ($message.method -eq 'Page.javascriptDialogOpening') {
                    # a dialog freezes the page: close it (it is reported below, a review page must never open one)
                    $script:nextId++
                    $close = [Text.Encoding]::UTF8.GetBytes((@{ id = $script:nextId; method = 'Page.handleJavaScriptDialog'; params = @{ accept = $true } } | ConvertTo-Json -Compress))
                    [void]$ws.SendAsync([ArraySegment[byte]]::new($close), 'Text', $true, $cancel.Token).GetAwaiter().GetResult()
                }
            }
        }
    }
    [void](Invoke-Cdp 'Runtime.enable')
    [void](Invoke-Cdp 'Log.enable')
    [void](Invoke-Cdp 'Page.enable')
    [void](Invoke-Cdp 'Page.navigate' @{ url = $url })

    $result = ''
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds - 5)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 500
        $reply = Invoke-Cdp 'Runtime.evaluate' @{ expression = "(document.getElementById('out') || {}).textContent || ''"; returnByValue = $true }
        $result = [string]$reply.result.result.value
        if ($result -match 'DONE') { break }
    }
    if ($result) { $result } else { 'no result from the page' }
    '--- what the browser reported (console, script errors, Content Security Policy):'
    $reported = 0
    foreach ($e in $events) {
        switch ($e.method) {
            'Runtime.consoleAPICalled' { "console.$($e.params.type): " + (($e.params.args | ForEach-Object { $_.value ?? $_.description }) -join ' '); $reported++ }
            'Runtime.exceptionThrown' {
                $d = $e.params.exceptionDetails
                "exception: $($d.exception.description ?? $d.text) (line $($d.lineNumber), $($d.url))"; $reported++
            }
            'Log.entryAdded' { "$($e.params.entry.source)/$($e.params.entry.level): $($e.params.entry.text) $($e.params.entry.url)"; $reported++ }
            'Page.javascriptDialogOpening' { "DIALOG OPENED ($($e.params.type)): $($e.params.message) at $($e.params.url)"; $reported++ }
        }
    }
    if ($reported -eq 0) { '(nothing: no console output, no script error, no policy violation)' }
}
finally {
    if ($ws) { $ws.Dispose() }
    if ($proc -and -not $proc.HasExited) { & taskkill.exe /T /F /PID $proc.Id *> $null }
    Start-Sleep -Milliseconds 500
    Remove-Item $profile -Recurse -Force -ErrorAction SilentlyContinue
}
