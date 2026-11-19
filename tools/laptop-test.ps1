# tools/laptop-test.ps1 -- the laptop plays the weather console.
# Run this AFTER joining the Wi-Fi "fauxcloud-setup" (and while tools\capture.ps1 records the board's serial
# log in another terminal, started BEFORE you switch Wi-Fi). Results go to laptop-test-<timestamp>.log in the repository's captures folder.
# Nothing here needs admin rights. Switch Wi-Fi back to your home network when it finishes.
# Pointed at fauxcloud's home-network address instead, its uploads are dropped unless admin -> Publishing ->
# "Accept uploads from my home network" is on.
param(
    [string] $Esp = "",              # default: 192.168.77.1 (product); 192.168.4.1 with -Discovery
    [string] $LanTarget = "",        # e.g. your Home Assistant LAN IP, for the AP-isolation test
    [int]    $LanPort = 8123,
    [string] $StaIp = "",            # fauxcloud's home-network address, for the AP -> STA check (step 9)
    [switch] $Discovery,             # testing firmware/discovery (listens on 443, AP 192.168.4.1) instead of the product
    [switch] $Setup                  # on "fauxcloud-setup-XXXX" (setup mode) check the setup page's data only
)
if (-not $Esp) { $Esp = if ($Discovery) { "192.168.4.1" } else { "192.168.77.1" } }
$ErrorActionPreference = "Continue"
$root = Split-Path -Parent $PSScriptRoot
$inbox = Join-Path $root "captures"
if (-not (Test-Path $inbox)) { New-Item -ItemType Directory $inbox | Out-Null }
$log = Join-Path $inbox "laptop-test-$(Get-Date -Format yyyyMMdd-HHmmss).log"
function Out($s) { Write-Host $s; $s | Out-File -FilePath $log -Append -Encoding utf8 }
function Step($n, $title) { Out ""; Out "===== $n. $title"; }

Out "fauxcloud laptop test  $(Get-Date -Format s)  target $Esp"

if ($Setup) {
    # the setup page must load its zone list from /a/ on the AP. Run on the
    # setup Wi-Fi "fauxcloud-setup-XXXX" (a new board or after a factory reset). Then finish setup on a phone.
    Step S1 "Setup page (expect 200 and a /a/<hash>/tz.json reference)"
    $tzUrl = $null
    try {
        $page = Invoke-WebRequest -Uri "http://$Esp/" -UseBasicParsing -TimeoutSec 10
        $m = [regex]::Match("$($page.Content)", "/a/[0-9a-f]{8}/tz\.json")
        Out ("  GET / -> HTTP $($page.StatusCode), $($page.Content.Length) chars")
        if ($m.Success) { $tzUrl = $m.Value; Out "  zone list: $tzUrl"; Out "RESULT setup-page: PASS" }
        else { Out "RESULT setup-page: FAIL (no /a/<hash>/tz.json in the page; is the board in setup mode?)" }
    } catch { Out "RESULT setup-page: FAIL ($($_.Exception.Message))" }

    Step S2 "Zone list (expect 200, application/json, parses, >= 300 zones)"
    if ($tzUrl) {
        try {
            $r = Invoke-WebRequest -Uri "http://$Esp$tzUrl" -UseBasicParsing -TimeoutSec 15
            $ct = "$($r.Headers['Content-Type'])"
            $n = -1
            try { $n = @(($r.Content | ConvertFrom-Json).z).Count } catch { }
            Out "  GET $tzUrl -> HTTP $($r.StatusCode), Content-Type '$ct', zones $n"
            $ok = $r.StatusCode -eq 200 -and $ct -like "application/json*" -and $n -ge 300
            Out ("RESULT setup-zones: " + $(if ($ok) { "PASS" } else { "FAIL (the setup page cannot offer a place; setup cannot finish)" }))
        } catch { Out "RESULT setup-zones: FAIL ($($_.Exception.Message))" }
    } else { Out "RESULT setup-zones: SKIPPED (no zone-list URL from step S1)" }

    Step S3 "Setup state (expect JSON)"
    try {
        $r = Invoke-WebRequest -Uri "http://$Esp/api/setup/state" -UseBasicParsing -TimeoutSec 10
        $null = $r.Content | ConvertFrom-Json
        Out "RESULT setup-state: PASS"
    } catch { Out "RESULT setup-state: FAIL ($($_.Exception.Message))" }

    Out ""
    Out "Now finish setup on a phone (firmware/README.md, 'Bench: setup end-to-end'). Log: $log"
    exit 0
}

Step 1 "Wi-Fi adapter state (expect SSID fauxcloud-setup)"
Out (netsh wlan show interfaces | Select-String -Pattern "SSID|State|Signal" | Out-String)

Step 2 "DHCP: DNS server handed out by the ESP (expect $Esp)"
$ipcfg = ipconfig /all | Out-String
Out ($ipcfg -split "`r?`n" | Select-String -Pattern "Wireless|Wi-Fi|IPv4 Address|Default Gateway|DNS Servers|DHCP Server" | Out-String)
$dnsOk = $ipcfg -match "DNS Servers[ .]*: $([regex]::Escape($Esp))"
Out ("RESULT dns-from-dhcp: " + $(if ($dnsOk) { "PASS" } else { "FAIL (check the lines above)" }))

Step 3 "Wildcard DNS (expect every name -> $Esp)"
foreach ($h in "rtupdate.wunderground.com", "time.windows.com", "weathercloud.net", "example.com") {
    $r = nslookup $h $Esp 2>&1 | Out-String
    $ok = $r -match [regex]::Escape($Esp)
    Out ("  $h -> " + $(if ($ok) { "PASS" } else { "FAIL" }))
    if (-not $ok) { Out $r }
}

Step 4 "WU upload path (expect body 'success', serial shows every arg)"
$q = "ID=ST1&PASSWORD=TEST&dateutc=2026-11-1+12%3A5%3A3&baromin=29.92&tempf=50.0&humidity=60&dewptf=37.0&rainin=0.00&dailyrainin=0.12&winddir=180&windspeedmph=3.0&windgustmph=5.0&UV=1&solarRadiation=-117.-5"
try {
    $resp = Invoke-WebRequest -Uri "http://$Esp/weatherstation/updateweatherstation.php?$q" -UseBasicParsing -TimeoutSec 5
    Out ("  HTTP $($resp.StatusCode) body='$($resp.Content.Trim())'")
    Out ("RESULT wu-success: " + $(if ($resp.Content.Trim() -eq "success") { "PASS" } else { "FAIL" }))
} catch { Out "RESULT wu-success: FAIL ($($_.Exception.Message))" }

Step 5 "Weathercloud host header (expect body '200')"
try {
    $resp = Invoke-WebRequest -Uri "http://$Esp/v01/set/wid/x/key/y/temp/205" -Headers @{ Host = "api.weathercloud.net" } -UseBasicParsing -TimeoutSec 5
    Out ("  HTTP $($resp.StatusCode) body='$($resp.Content.Trim())'")
    Out ("RESULT weathercloud-200: " + $(if ($resp.Content.Trim() -eq "200") { "PASS" } else { "FAIL" }))
} catch { Out "RESULT weathercloud-200: FAIL ($($_.Exception.Message))" }

Step 6 "NTP responder (expect an offset line; FAIL if the ESP has no time yet)"
$w = w32tm /stripchart /computer:$Esp /samples:2 /dataonly 2>&1 | Out-String
Out $w
Out ("RESULT ntp-answer: " + $(if ($w -match "[+-]\d+\.\d+s") { "PASS" } else { "FAIL" }))

if ($Discovery) { Step 7 "TLS probe on 443 (discovery firmware: expect a connection that is dropped; serial shows [TCP443])" }
else { Step 7 "TLS probe on 443 (product firmware: nothing listens, expect the connection to be refused)" }
try {
    $tcp = New-Object System.Net.Sockets.TcpClient
    $ar = $tcp.BeginConnect($Esp, 443, $null, $null)
    $connected = $ar.AsyncWaitHandle.WaitOne(3000) -and $tcp.Connected
    $tcp.Close()
    if ($Discovery) { Out ("RESULT tcp443-accepts: " + $(if ($connected) { "PASS (connected, check serial)" } else { "FAIL (no connect)" })) }
    else { Out ("RESULT tcp443-refused: " + $(if ($connected) { "FAIL (something listens on 443)" } else { "PASS (refused)" })) }
} catch {
    if ($Discovery) { Out "RESULT tcp443-accepts: FAIL ($($_.Exception.Message))" } else { Out "RESULT tcp443-refused: PASS (refused: $($_.Exception.Message))" }
}

Step 8 "AP isolation: nothing beyond the ESP may be reachable -- every line must say BLOCKED"
foreach ($t in @(@{ h = "8.8.8.8"; p = 53 }, @{ h = "1.1.1.1"; p = 443 })) {
    $ok = Test-NetConnection -ComputerName $t.h -Port $t.p -InformationLevel Quiet -WarningAction SilentlyContinue
    Out ("  $($t.h):$($t.p) -> " + $(if ($ok) { "REACHABLE  <-- FAIL, the ESP forwards traffic" } else { "BLOCKED (PASS)" }))
}
$ping = Test-Connection 8.8.8.8 -Count 2 -Quiet -ErrorAction SilentlyContinue
Out ("  ping 8.8.8.8 -> " + $(if ($ping) { "REPLY  <-- FAIL" } else { "BLOCKED (PASS)" }))
if ($LanTarget) {
    $ok = Test-NetConnection -ComputerName $LanTarget -Port $LanPort -InformationLevel Quiet -WarningAction SilentlyContinue
    Out ("  LAN ${LanTarget}:$LanPort -> " + $(if ($ok) { "REACHABLE  <-- FAIL" } else { "BLOCKED (PASS)" }))
    $ping = Test-Connection $LanTarget -Count 2 -Quiet -ErrorAction SilentlyContinue
    Out ("  ping $LanTarget -> " + $(if ($ping) { "REPLY  <-- FAIL" } else { "BLOCKED (PASS)" }))
} else { Out "  (pass -LanTarget <HA LAN IP> to also probe a LAN host)" }

Step 9 "AP client -> fauxcloud's home-network address: must answer like an unknown request, never admin"
if ($StaIp) {
    try {
        $resp = Invoke-WebRequest -Uri "http://$StaIp/api/admin/challenge" -Headers @{ "X-Fauxcloud" = "1" } -UseBasicParsing -TimeoutSec 15
        $body = "$($resp.Content)".Trim()
        Out "  GET http://$StaIp/api/admin/challenge -> $($resp.StatusCode) '$body'"
        if ($body -eq "success") { Out "RESULT ap-to-sta: PASS" }
        elseif ($body -match "nonce") { Out "RESULT ap-to-sta: FAIL (the admin login is reachable from the AP)" }
        else { Out "RESULT ap-to-sta: CHECK (unexpected answer)" }
    } catch {
        # no answer proves nothing (it looks the same as a phone that used mobile data)
        Out "RESULT ap-to-sta: INCONCLUSIVE (no answer: $($_.Exception.Message)) -- check the route to $StaIp, run again"
    }
} else { Out "  (pass -StaIp <fauxcloud's home-network IP> to run this check)" }

Out ""
Out "Done. Switch Wi-Fi back to your home network. Log: $log"
