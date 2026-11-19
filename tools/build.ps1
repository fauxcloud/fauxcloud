# tools/build.ps1 -- compile (and optionally flash + monitor) a sketch with the pinned profile.
# Usage:
#   tools\build.ps1 discovery                       # compile only
#   tools\build.ps1 discovery -Port COM5 -Upload    # compile + flash
#   tools\build.ps1 discovery -Port COM5 -Upload -Monitor
#   tools\build.ps1 discovery -Profile esp32dev     # pick a profile from sketch.yaml (default: default_profile)
#   tools\build.ps1 fauxcloud -Define FAUXCLOUD_BAD_IMAGE   # extra -D (test image, another board): its own
#                                                          # build dir, binaries not exported over the normal ones
#   tools\build.ps1 fauxcloud -Define FWD_TLS=0,FAUXCLOUD_TEST     # several -D, comma-separated (the PSRAM profile's
#                                                          # FC_MAX_STATIONS=2 needs the PSRAM board option: CI step)
param(
    [Parameter(Mandatory = $true, Position = 0)] [string] $Sketch,
    [string] $Profile = "",
    [string] $Port = "",
    [switch] $Upload,
    [switch] $Monitor,
    [switch] $Clean,
    [string[]] $Define = @()
)
$ErrorActionPreference = "Stop"
# powershell -File passes "A=1,B=2" as one string: split it (inside a session the comma already makes an array)
$Define = @($Define | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$root = Split-Path -Parent $PSScriptRoot
$sketchDir = Join-Path $root "firmware\$Sketch"
if (-not (Test-Path (Join-Path $sketchDir "sketch.yaml"))) { throw "No sketch.yaml in $sketchDir" }
$cfg = Join-Path $root "arduino-cli.yaml"
# a git worktree in a deeper folder (recognised here under .claude\worktrees\) may share the main working tree's
# .arduino through a junction. Through the longer worktree path the compiler's multilib header path
# (...\esp32\psram\no-rtti\bits\c++config.h) passes Windows' 260-char MAX_PATH in the libs-detect step and every compile
# fails with "bits/c++config.h: No such file or directory". arduino-cli then gets the junction's target (the main
# working tree's shorter path) through its environment overrides; a value already set by the caller wins.
$ardLink = Join-Path $root ".arduino"
if ($root -match '\\\.claude\\worktrees\\' -and (Test-Path $ardLink)) {
    $ardItem = Get-Item $ardLink -Force
    $ardTarget = if ($ardItem.LinkType) { @($ardItem.Target)[0] } else { $null }
    if ($ardTarget) {
        # each variable on its own, only where the caller left it unset (this process and its children only)
        $set = @()
        foreach ($d in "data", "downloads", "user") {
            $var = "ARDUINO_DIRECTORIES_" + $d.ToUpper()
            if (-not [Environment]::GetEnvironmentVariable($var)) {
                [Environment]::SetEnvironmentVariable($var, (Join-Path $ardTarget $d), "Process")
                $set += $d
            }
        }
        if ($set) { Write-Host (">> worktree: arduino-cli " + ($set -join "/") + " dir(s) from $ardTarget (the junction path would exceed MAX_PATH)") -ForegroundColor Cyan }
    }
}
# One build dir PER PROFILE. Sharing one dir between profiles with different board options (e.g. partition
# schemes) left mixed artifacts and produced a non-bootable flash once.
$profileName = if ($Profile) { $Profile } else { "default" }
if ($Define) { $profileName = "$profileName-" + (($Define -join '-') -replace '[^A-Za-z0-9_-]', '_') }
$buildDir = Join-Path $sketchDir "build\$profileName"
if ($Clean -and (Test-Path $buildDir)) { Remove-Item -Recurse -Force $buildDir }

$cli = Get-Command arduino-cli -ErrorAction SilentlyContinue
if (-not $cli) { throw "arduino-cli not found. Install: winget install ArduinoSA.CLI" }

$profileArgs = @()
if ($Profile) { $profileArgs = @("--profile", $Profile) }

# the partition tables' static check and the frozen 1.x table's hash gate, before any compile
if ($Sketch -eq "fauxcloud") {
    # this script builds the WROOM-32 table only: a PSRAM fqbn needs partitions-psram.csv, which only CI's PSRAM step
    # swaps in (with the WROOM-32 table the PSRAM constants would run RAM-only for ever)
    $yaml = Get-Content (Join-Path $sketchDir "sketch.yaml") -Raw
    $pn = if ($Profile) { $Profile } elseif ($yaml -match '(?m)^default_profile:\s*(\S+)') { $Matches[1] } else { "" }
    if ($pn -and ($yaml -match ("(?m)^\s+" + [regex]::Escape($pn) + ":\s*\r?\n\s+fqbn:\s*(\S+)")) -and ($Matches[1] -match 'PSRAM=enabled')) {
        throw "profile $pn has a PSRAM fqbn: that board needs partitions-psram.csv (CI's PSRAM step); this script builds the WROOM-32 table only"
    }
    & python -I (Join-Path $root "tools\check_partitions.py")
    if ($LASTEXITCODE -ne 0) { throw "partition table check failed (tools/check_partitions.py): the 1.x table is frozen" }
    # the web budgets as CI checks them (initial pages, lazy files, flash total and its code / asset split) and the
    # generated header up to date. Without the generator's tools (node, the pinned zopfli in
    # tools/.pylib, terser in tools/node_modules) one warning and the committed web_assets.h; CI keeps the hard check
    $nodeOk = ($env:FAUXCLOUD_NODE -and (Test-Path $env:FAUXCLOUD_NODE)) -or [bool](Get-Command node -ErrorAction SilentlyContinue)
    $genOk = $nodeOk -and (Test-Path (Join-Path $root "tools\.pylib\zopfli")) -and (Test-Path (Join-Path $root "tools\node_modules\terser"))
    if (-not $genOk) {
        Write-Host "web assets not checked: install node + tools/.pylib (see firmware/README)" -ForegroundColor Yellow
    } else {
        & python -I (Join-Path $root "tools\gen_web_assets.py") --check
        if ($LASTEXITCODE -ne 0) { throw "web assets check failed (tools/gen_web_assets.py --check): a budget is over or web_assets.h is stale" }
    }
}
Write-Host ">> compile $Sketch" -ForegroundColor Cyan
# fauxcloud's image marker carries __DATE__/__TIME__ of app_sys.cpp: drop its object so every build has its real date
# Touching the source is not enough: arduino-cli copies the sketch into the build dir only when the content
# changed, so the copy keeps its old time and the object is reused.
$markerObj = Join-Path $buildDir "sketch\app_sys.cpp.o"
if (Test-Path $markerObj) { Remove-Item $markerObj -Force }
$markerSrc = Join-Path $sketchDir "app_sys.cpp"
if (Test-Path $markerSrc) { (Get-Item $markerSrc).LastWriteTime = Get-Date }
# every warning shown (the core's and the libraries' too; ours are kept at zero), never -Werror on device
$extra = @("--export-binaries", "--warnings", "all")
if ($Define) { $extra = @("--warnings", "all", "--build-property", ("compiler.cpp.extra_flags=" + (($Define | ForEach-Object { "-D$_" }) -join ' '))) }
# every C++ unit we compile without exceptions (-26.6 KB). The core's
# cpp_flags file adds -fexceptions AFTER compiler.cpp.extra_flags, so the flag rides in build.extra_flags (later in the
# recipe); this is the core 3.3.12 platform.txt value of build.extra_flags + the flag -- mirror it on a core bump (the
# .eh_frame guard below fails the build if the flag is lost). The \" keep the two string defines quoted through
# Windows PowerShell 5.1.
$noExc = 'build.extra_flags=-DARDUINO_HOST_OS=\"{runtime.os}\" -DARDUINO_FQBN=\"{build.fqbn}\" -DESP32=ESP32 ' +
         '-DCORE_DEBUG_LEVEL={build.code_debug} {build.loop_core} {build.event_core} {build.defines} ' +
         '{build.extra_flags.{build.mcu}} {build.zigbee_mode} {build.matter_mode} -fno-exceptions'
$extra += @("--build-property", $noExc)
# Warnings arrive on stderr: in Windows PowerShell 5.1 a redirected stderr line is an error record, which "Stop" would
# turn into a failure. The exit code decides.
$ErrorActionPreference = "Continue"
$compileOut = & arduino-cli --config-file $cfg compile @profileArgs --build-path $buildDir @extra $sketchDir 2>&1 | ForEach-Object { "$_" }
$compileExit = $LASTEXITCODE
$ErrorActionPreference = "Stop"
$compileOut | ForEach-Object { Write-Host $_ }
if ($compileExit -ne 0) { throw "compile failed" }

# Real app-size gate when the sketch ships its own partitions.csv: arduino-cli compares against the board
# default (1.25 MB), which is wrong for a custom table -- too strict below it, absent above it (PartitionScheme=custom).
$partCsv = Join-Path $sketchDir "partitions.csv"
$sizeLine = $compileOut | Where-Object { $_ -match "Sketch uses (\d+) bytes" } | Select-Object -First 1
if ((Test-Path $partCsv) -and $sizeLine -and ($sizeLine -match "Sketch uses (\d+) bytes")) {
    $appBytes = [int64]$Matches[1]
    # app0 and app1 both, the same size (an OTA writes the other slot)
    $slots = @{}
    foreach ($nm in "app0", "app1") {
        $row = Get-Content $partCsv | Where-Object { $_ -match "^\s*$nm\s*," } | Select-Object -First 1
        if ($row -and ($row -match "^\s*\w+\s*,\s*app\s*,\s*\w+\s*,\s*(0x[0-9A-Fa-f]+|\d+)\s*,\s*(0x[0-9A-Fa-f]+|\d+)")) {
            $slots[$nm] = [Convert]::ToInt64($Matches[2].Replace("0x", ""), $(if ($Matches[2] -like "0x*") { 16 } else { 10 }))
        }
    }
    if ($slots.Count -ne 2) { throw "partitions.csv: app0 and app1 must both exist" }
    if ($slots["app0"] -ne $slots["app1"]) { throw "partitions.csv: app0 and app1 differ in size" }
    if ($slots["app0"]) {
        $slot = $slots["app0"]
        $pct = [math]::Round(100.0 * $appBytes / $slot, 1)
        Write-Host (">> app slot (partitions.csv app0): {0:N0} B used of {1:N0} B = {2}%" -f $appBytes, $slot, $pct) -ForegroundColor Cyan
        if ($appBytes -gt $slot) { throw "app image ($appBytes B) does not fit the app0 slot ($slot B)" }
        # the warning line is 95 % of the slot (1 805 517 B for 0x1D0000); the slot itself fails
        $warnAt = [int64][math]::Ceiling(0.95 * $slot)
        if ($appBytes -ge $warnAt) { Write-Warning ("app image at or above 95% of the slot ({0:N0} B): trim the image before any table change" -f $warnAt) }
    }
}

# guard: the image was really built without exceptions -- .eh_frame holds only the prebuilt IDF archives' frames
# (4.6 KB measured; 23.4 KB with exceptions) and no unit of ours left a .gcc_except_table
$elf = Get-ChildItem $buildDir -Filter "*.ino.elf" | Select-Object -First 1
$readelf = Get-ChildItem (Join-Path $root ".arduino\data\internal") -Recurse -Filter "xtensa-esp32-elf-readelf.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $elf -or -not $readelf) { throw "no-exceptions guard: ELF or readelf not found" }
$sec = @{}
& $readelf.FullName -S -W $elf.FullName | ForEach-Object {
    if ($_ -match "\]\s+(\.eh_frame|\.gcc_except_table)\s+\S+\s+[0-9a-f]+\s+[0-9a-f]+\s+([0-9a-f]+)") { $sec[$Matches[1]] = [Convert]::ToInt32($Matches[2], 16) }
}
$eh = [int]$sec[".eh_frame"]; $gx = [int]$sec[".gcc_except_table"]
Write-Host (">> no-exceptions guard: .eh_frame {0:N0} B (max 8 192), .gcc_except_table {1:N0} B (must be 0)" -f $eh, $gx) -ForegroundColor Cyan
if ($eh -gt 8192 -or $gx -gt 0) { throw "no-exceptions guard failed: the -fno-exceptions flag did not reach every unit (core update?)" }

# the RTC_NOINIT words never move between images (tools/rtc_layout.txt, append-only)
if ($Sketch -eq "fauxcloud") {
    $nm = Get-ChildItem (Join-Path $root ".arduino\data\internal") -Recurse -Filter "xtensa-esp32-elf-nm.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $nm) { throw "RTC layout check: nm not found" }
    & python -I (Join-Path $root "tools\check_rtc_layout.py") --nm $nm.FullName --elf $elf.FullName
    if ($LASTEXITCODE -ne 0) { throw "RTC layout check failed: an RTC_NOINIT word moved (tools/rtc_layout.txt)" }
}

if ($Upload) {
    if (-not $Port) { throw "-Upload needs -Port COMx" }
    Write-Host ">> upload $Sketch -> $Port" -ForegroundColor Cyan
    & arduino-cli --config-file $cfg upload @profileArgs --input-dir $buildDir -p $Port $sketchDir
    if ($LASTEXITCODE -ne 0) { throw "upload failed" }
}
if ($Monitor) {
    if (-not $Port) { throw "-Monitor needs -Port COMx" }
    Write-Host ">> monitor $Port @115200 (Ctrl+C to stop)" -ForegroundColor Cyan
    & arduino-cli --config-file $cfg monitor -p $Port -c baudrate=115200
}
