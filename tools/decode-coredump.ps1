# tools/decode-coredump.ps1 -- read the crash data fauxcloud's admin page downloads (Maintenance -> Crash data ->
# Download, saved as fauxcloud-coredump.bin) with the pinned toolchain's ESP32 gdb: backtrace of the crashed task
# plus every task's current frame. No Python packages, no ESP-IDF install.
#
# The file contains your passwords (a RAM image: Wi-Fi, MQTT, admin). Never share it -- paste the decoded backtrace
# from this script instead. The admin page asks for your password before the download and clears the crash data on
# fauxcloud once it was sent.
#
# The file is the raw coredump partition image: a 24-byte header, then an ELF core, then a CRC32. The ELF is what
# gdb reads; the build's own ELF (tools\build.ps1 leaves it in firmware\fauxcloud\build\<profile>\fauxcloud.ino.elf)
# gives it the symbols. Use the ELF of the image that crashed (same version + build date as the admin page showed).
#
# Usage (from the repo root):
#   tools\decode-coredump.ps1 captures\fauxcloud-coredump.bin
#   tools\decode-coredump.ps1 captures\fauxcloud-coredump.bin -Elf firmware\fauxcloud\build\default-FAUXCLOUD_BAD_IMAGE\fauxcloud.ino.elf
param(
    [Parameter(Mandatory = $true, Position = 0)] [string] $Dump,
    [string] $Elf = "firmware\fauxcloud\build\default\fauxcloud.ino.elf",
    [switch] $AllThreads
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if (-not (Test-Path $Dump)) { throw "No dump at $Dump" }
if (-not (Test-Path $Elf)) { throw "No ELF at $Elf (build first: tools\build.ps1 fauxcloud)" }
$gdb = Get-ChildItem (Join-Path $root ".arduino\data\internal") -Recurse -Filter "xtensa-esp32-elf-gdb.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $gdb) { throw "xtensa-esp32-elf-gdb.exe not found under .arduino\data\internal (run a build once so the toolchain is installed)" }

$bytes = [IO.File]::ReadAllBytes((Resolve-Path $Dump).Path)
$off = 0
for ($i = 0; $i -lt [Math]::Min(64, $bytes.Length - 4); $i++) {
    if ($bytes[$i] -eq 0x7F -and $bytes[$i + 1] -eq 0x45 -and $bytes[$i + 2] -eq 0x4C -and $bytes[$i + 3] -eq 0x46) { $off = $i; break }
}
if ($off -eq 0 -and $bytes[0] -ne 0x7F) { throw "No ELF header in the first 64 bytes: not a coredump image" }
$core = Join-Path $env:TEMP "fauxcloud-core.elf"
[IO.File]::WriteAllBytes($core, $bytes[$off..($bytes.Length - 1)])
Write-Host ("dump {0} B, ELF core at offset {1}, symbols from {2}" -f $bytes.Length, $off, $Elf)

$cmds = @("-batch", "-nx", "-ex", "set pagination off", "-ex", "bt")
if ($AllThreads) { $cmds += @("-ex", "info threads", "-ex", "thread apply all bt 8") } else { $cmds += @("-ex", "info threads") }
$cmds += @((Resolve-Path $Elf).Path, "--core", $core)
# gdb writes its warnings to stderr; Windows PowerShell 5.1 would turn a native stderr line into an error record,
# so both streams go to files and are read back.
$out = Join-Path $env:TEMP "fauxcloud-core.out"; $err = Join-Path $env:TEMP "fauxcloud-core.err"
$argStr = ($cmds | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }) -join ' '
$p = Start-Process -FilePath $gdb.FullName -ArgumentList $argStr -NoNewWindow -Wait -PassThru -RedirectStandardOutput $out -RedirectStandardError $err
Get-Content $out, $err -ErrorAction SilentlyContinue | Where-Object { $_ -notmatch "^warning:|^\[New process|^\s*$" }
if ($p.ExitCode -ne 0) { Write-Host "gdb exit code $($p.ExitCode)" }
Remove-Item $core, $out, $err -ErrorAction SilentlyContinue
