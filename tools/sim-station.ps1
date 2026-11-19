# tools/sim-station.ps1 -- a pretend weather console for long runs: sends Weather Underground uploads to fauxcloud
# every -IntervalS seconds (default 60, like the YT60234), with slowly varying, believable numbers: a daily
# temperature curve, wind and gusts that swell and calm with a drifting direction, rain in occasional showers with a
# daily counter that resets at local midnight, pressure drifting a few hPa a day, sun and UV by day.
# Stop it with Ctrl+C. Every upload is one line in the console; nothing is written to disk.
#
# ASCII only on purpose: Windows PowerShell 5.1 reads a BOM-less .ps1 in the ANSI codepage.
#
# Usage (from the repo root):
#   powershell -ExecutionPolicy Bypass -File tools\sim-station.ps1                      (laptop on the fauxcloud Wi-Fi)
#   powershell -ExecutionPolicy Bypass -File tools\sim-station.ps1 -Esp fauxcloud.local -Id SIM1 -IntervalS 30
# From your home network (-Esp fauxcloud.local / its LAN address) fauxcloud drops the uploads unless admin ->
# Publishing -> "Accept uploads from my home network" is on; on the fauxcloud Wi-Fi they always count.
# The first upload binds the station to this script's key (PASSWORD, -Key, default sim): another key is ignored
# until accepted in admin. Test it with a second run: -Key other (same -Id).
param(
    [string]$Esp = "192.168.77.1",  # the ESP on its own Wi-Fi; -Esp <name or address> for its home-network side
    [string]$Id = "SIM1",
    [int]$IntervalS = 60,
    [string]$Key = "sim",  # sent as PASSWORD; fauxcloud binds the station to the first key it sees
    [int]$Seed = 1
)
$ErrorActionPreference = "Stop"
$rnd = New-Object System.Random $Seed
function Noise([double]$amp) { return ($rnd.NextDouble() - 0.5) * 2 * $amp }
$inv = [System.Globalization.CultureInfo]::InvariantCulture
function F([double]$v, [int]$d) { return $v.ToString("F$d", $inv) }

$dir = 240.0          # wind direction, degrees, drifts slowly
$daily = 0.0          # daily rain counter, inches
$day = (Get-Date).Date
$rainLeft = 0         # minutes left in the current shower
$n = 0
Write-Host "Simulating console '$Id' -> http://$Esp/ every $IntervalS s (Ctrl+C to stop)"
while ($true) {
    $now = Get-Date
    if ($now.Date -ne $day) { $day = $now.Date; $daily = 0.0 }   # the console resets its daily rain at midnight
    $h = $now.Hour + $now.Minute / 60.0
    # temperature: low near 05:00, high near 15:00 (deg C), dew point a few degrees below
    $tc = 9.0 + 6.0 * [Math]::Sin(($h - 9.0) / 24.0 * 2 * [Math]::PI) + (Noise 0.3)
    $hum = [Math]::Min(98, [Math]::Max(35, 75 - 15 * [Math]::Sin(($h - 9.0) / 24.0 * 2 * [Math]::PI) + (Noise 2)))
    $dew = $tc - (100 - $hum) / 5.0
    # wind: a slow swell plus noise, gusts above it; direction drifts
    $wind = [Math]::Max(0, 9 + 5 * [Math]::Sin($n / 40.0) + (Noise 3))
    $gust = $wind + 3 + [Math]::Abs((Noise 6))
    $dir = ($dir + (Noise 12) + 360) % 360
    # rain: a shower starts now and then and lasts 20-60 min
    if ($rainLeft -le 0 -and $rnd.NextDouble() -lt 0.01) { $rainLeft = 20 + $rnd.Next(40) }
    $rate = 0.0
    if ($rainLeft -gt 0) { $rainLeft -= [Math]::Max(1, [int]($IntervalS / 60)); $rate = 0.02 + $rnd.NextDouble() * 0.15 }  # in/h
    $daily += $rate * $IntervalS / 3600.0
    $pressure = 1013 + 4 * [Math]::Sin($now.ToFileTimeUtc() / 1e7 / 86400.0 * 2 * [Math]::PI / 1.3) + (Noise 0.2)  # hPa, a few hPa a day
    $sun = [Math]::Max(0, 600 * [Math]::Sin(($h - 6.0) / 13.0 * [Math]::PI))   # W/m2, 06:00-19:00
    if ($rainLeft -gt 0) { $sun = $sun * 0.2 }
    $uv = [Math]::Round($sun / 120.0, 0)

    $utc = $now.ToUniversalTime().ToString("yyyy-MM-dd+HH'%3A'mm'%3A'ss")
    $q = "ID=$Id&PASSWORD=$([uri]::EscapeDataString($Key))&dateutc=$utc" +
         "&tempf=" + (F ($tc * 1.8 + 32) 1) + "&humidity=" + (F $hum 0) + "&dewptf=" + (F ($dew * 1.8 + 32) 1) +
         "&baromin=" + (F ($pressure / 33.8639) 2) + "&windspeedmph=" + (F ($wind / 1.609344) 1) +
         "&windgustmph=" + (F ($gust / 1.609344) 1) + "&winddir=" + (F $dir 0) +
         "&rainin=" + (F $rate 2) + "&dailyrainin=" + (F $daily 2) +
         "&solarRadiation=" + (F $sun 1) + "&UV=" + (F $uv 0)
    # the real YT60234 shape (docs/station-profiles.md): solarRadiation camelCase, UV upper-case, no
    # action/realtime/rtfreq flags
    $url = "http://$Esp/weatherstation/updateweatherstation.php?$q"
    $t0 = Get-Date
    try {
        $r = Invoke-WebRequest -Uri $url -UseBasicParsing -TimeoutSec 10
        $ms = [int]((Get-Date) - $t0).TotalMilliseconds
        Write-Host ("{0:HH:mm:ss} #{1} {2} ms -> {3}  T={4} C wind={5} km/h dir={6} rain today={7} in" -f $now, $n, $ms, $r.Content.Trim(), (F $tc 1), (F $wind 1), (F $dir 0), (F $daily 2))
    } catch {
        Write-Host ("{0:HH:mm:ss} #{1} FAILED: {2}" -f $now, $n, $_.Exception.Message) -ForegroundColor Yellow
    }
    $n++
    Start-Sleep -Seconds $IntervalS
}
