# tools/capture.ps1 -- capture the board's serial log to captures/.
# Usage:
#   tools\capture.ps1 -Port COM5                  # capture until Ctrl+C, tee to captures/capture-<timestamp>.log
#   tools\capture.ps1 -Port COM5 -Seconds 60      # capture for 60 s, then stop
#   tools\capture.ps1 -Port COM5 -Reset           # pulse EN (RTS) first so the log starts at boot
# Uses System.IO.Ports only (no arduino-cli monitor plugin needed). DTR/RTS are released so the ESP32 runs
# normally; -Reset toggles RTS (EN) once.
param(
    [Parameter(Mandatory = $true)] [string] $Port,
    [int] $Baud = 115200,
    [int] $Seconds = 0,
    [switch] $Reset,
    [string] $Tag = ""
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$inbox = Join-Path $root "captures"
if (-not (Test-Path $inbox)) { New-Item -ItemType Directory $inbox | Out-Null }
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$suffix = if ($Tag) { "-$Tag" } else { "" }
$log = Join-Path $inbox "capture-$stamp$suffix.log"

$sp = New-Object System.IO.Ports.SerialPort $Port, $Baud, 'None', 8, 'One'
$sp.ReadTimeout = 500
$sp.Encoding = [System.Text.Encoding]::UTF8  # station display names are UTF-8; default ASCII printed "?"
$sp.DtrEnable = $false
$sp.RtsEnable = $false
$sp.Open()
try {
    if ($Reset) {
        # EN is wired to RTS on most dev boards; DTR must stay high (false here) so GPIO0 is not pulled low.
        $sp.RtsEnable = $true; Start-Sleep -Milliseconds 100; $sp.RtsEnable = $false
    }
    $sp.DiscardInBuffer()
    Write-Host ">> capturing $Port @ $Baud -> $log  (Ctrl+C to stop)" -ForegroundColor Cyan
    $deadline = if ($Seconds -gt 0) { (Get-Date).AddSeconds($Seconds) } else { [DateTime]::MaxValue }
    $sb = New-Object System.Text.StringBuilder
    while ((Get-Date) -lt $deadline) {
        try {
            $chunk = $sp.ReadExisting()
            if ($chunk.Length -gt 0) {
                [void]$sb.Append($chunk)
                Write-Host -NoNewline $chunk
                Add-Content -Path $log -Value $chunk -NoNewline -Encoding utf8
            } else {
                Start-Sleep -Milliseconds 50
            }
        } catch [TimeoutException] { }
    }
} finally {
    $sp.Close()
    Write-Host "`n>> saved $log" -ForegroundColor Cyan
}
