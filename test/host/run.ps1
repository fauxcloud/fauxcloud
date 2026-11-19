# test/host/run.ps1 -- compile firmware/fauxcloud/src/*.cpp + the host test and run it. No network.
#
# Uses g++ when it is on PATH (the reference compiler, same as run.sh / CI on Linux), otherwise
# MSVC cl.exe from Visual Studio (Build Tools) via vcvars64.bat -- no extra install.
#
# ASCII only on purpose: Windows PowerShell 5.1 reads a BOM-less .ps1 in the ANSI codepage.
#
# Usage:  powershell -ExecutionPolicy Bypass -File test\host\run.ps1   (from repo root)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$src = Join-Path $root "firmware\fauxcloud\src"
$outDir = Join-Path $PSScriptRoot "build"
if (-not (Test-Path $outDir)) { New-Item -ItemType Directory $outDir | Out-Null }
# Self-ignoring build dir, so the repo .gitignore needs no entry for it.
Set-Content -Path (Join-Path $outDir ".gitignore") -Value "*" -Encoding ascii
$exe = Join-Path $outDir "test_fauxcloud.exe"
$sources = @(Get-ChildItem (Join-Path $src "*.cpp") | ForEach-Object { $_.FullName }) + (Join-Path $PSScriptRoot "test_fauxcloud.cpp")
# The C side -- vendored BearSSL (device + host-only lists) with its warnings not promoted, and our generated anchors
# (src/fwd_roots.c) with ours. Every OS entropy / time source off, as on the xtensa target (no BR_* flag on the board).
$bear = Join-Path $src "bearssl"
$bearC = @(Get-ChildItem (Join-Path $bear "*.c") | ForEach-Object { $_.FullName }) +
         @(Get-ChildItem (Join-Path $PSScriptRoot "bearssl\*.c") | ForEach-Object { $_.FullName })
$ourC = @(Get-ChildItem (Join-Path $src "*.c") | ForEach-Object { $_.FullName })
$brDefs = @("BR_64=0", "BR_LE_UNALIGNED=0", "BR_INT128=0", "BR_RDRAND=0", "BR_USE_WIN32_RAND=0", "BR_USE_URANDOM=0", "BR_USE_GETENTROPY=0", "BR_USE_UNIX_TIME=0", "BR_USE_WIN32_TIME=0")
$cDir = Join-Path $outDir "c"
if (-not (Test-Path $cDir)) { New-Item -ItemType Directory $cDir | Out-Null }

if (Get-Command g++ -ErrorAction SilentlyContinue) {
    Write-Host ">> g++" -ForegroundColor Cyan
    $cObjs = @()
    foreach ($c in $bearC + $ourC) {
        $o = Join-Path $cDir (([IO.Path]::GetFileNameWithoutExtension($c)) + ".o")
        $warn = if ($ourC -contains $c) { @("-Wall", "-Wextra", "-Werror") } else { @() }
        & gcc -std=c99 -O1 -c @warn ($brDefs | ForEach-Object { "-D$_" }) -I $src -I $bear $c -o $o
        if ($LASTEXITCODE -ne 0) { throw "C compile failed: $c" }
        $cObjs += $o
    }
    & g++ -std=c++17 -O1 -g -Wall -Wextra -Werror -Wno-missing-field-initializers -ffp-contract=off -I $src @sources @cObjs -o $exe
    if ($LASTEXITCODE -ne 0) { throw "compile failed" }
} else {
    $vcvars = Get-ChildItem @(
        'C:\Program Files*\Microsoft Visual Studio\*\BuildTools\VC\Auxiliary\Build\vcvars64.bat',
        'C:\Program Files*\Microsoft Visual Studio\*\Community\VC\Auxiliary\Build\vcvars64.bat',
        'C:\Program Files*\Microsoft Visual Studio\*\Professional\VC\Auxiliary\Build\vcvars64.bat'
    ) -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $vcvars) { throw "Neither g++ nor Visual Studio C++ tools found." }
    Write-Host ">> MSVC via $($vcvars.FullName)" -ForegroundColor Cyan
    $srcList = ($sources | ForEach-Object { '"' + $_ + '"' }) -join ' '
    $dList = ($brDefs | ForEach-Object { '/D' + $_ }) -join ' '
    # response files: 64 vendored paths would pass cmd.exe's 8 191-character line limit
    $bearRsp = Join-Path $outDir "bear_c.rsp"
    Set-Content -Path $bearRsp -Encoding ascii -Value ($bearC | ForEach-Object { '"' + $_ + '"' })
    $bearList = '@"' + $bearRsp + '"'
    $ourList = ($ourC | ForEach-Object { '"' + $_ + '"' }) -join ' '
    # vendored C: /W1, never promoted (upstream is never patched); ours: /W4 /WX
    $ccBear = 'cl /nologo /c /TC /O1 /W1 /utf-8 /D_CRT_SECURE_NO_WARNINGS {0} /I "{1}" /I "{2}" {3} /Fo"{4}\\" >nul' -f $dList, $src, $bear, $bearList, $cDir
    $ccOur = 'cl /nologo /c /TC /W4 /WX /utf-8 {0} /I "{1}" /I "{2}" {3} /Fo"{4}\\"' -f $dList, $src, $bear, $ourList, $cDir
    $cObjs = @($bearC + $ourC | ForEach-Object { Join-Path $cDir (([IO.Path]::GetFileNameWithoutExtension($_)) + ".obj") })
    $objRsp = Join-Path $outDir "c_obj.rsp"
    Set-Content -Path $objRsp -Encoding ascii -Value ($cObjs | ForEach-Object { '"' + $_ + '"' })
    $objList = '@"' + $objRsp + '"'
    # /utf-8: sources contain degree signs; /D_CRT_SECURE_NO_WARNINGS: strncpy/strcpy are fine here.
    $cl = 'cl /nologo /std:c++17 /EHsc /W4 /WX /utf-8 /D_CRT_SECURE_NO_WARNINGS /I "{0}" {1} {4} /Fo"{2}\\" /Fe"{3}"' -f $src, $srcList, $outDir, $exe, $objList
    $bat = Join-Path $outDir "build.cmd"
    # vcvars output (and a benign vswhere complaint on VS 18) is swallowed; </NUL avoids a hang on a prompt.
    Set-Content -Path $bat -Encoding ascii -Value @(
        '@echo off',
        ('call "{0}" >nul 2>nul <NUL' -f $vcvars.FullName),
        'if errorlevel 1 (echo VCVARS_FAILED & exit /b 1)',
        $ccBear,
        'if errorlevel 1 (echo BEARSSL_C_FAILED & exit /b 1)',
        $ccOur,
        'if errorlevel 1 exit /b 1',
        $cl,
        'exit /b %errorlevel%'
    )
    & cmd /c "`"$bat`""
    if ($LASTEXITCODE -ne 0) { throw "compile failed" }
}
& $exe
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# The variants, each the same suite in its own build dir (FC_MAX_STATIONS and FWD_TLS are independent profile
# constants; three runs by convention): the default run above is the release (FWD_TLS=1, one station); FWD_TLS=0 the
# HTTP-only fallback; the PSRAM placeholder, two stations + TLS + the bench diagnostics (the host compiles diag(),
# learnJson and HeapLowLine under FC_DIAG_BENCH, so st2 checks them).
$variants = @(
    @{ name = "FWD_TLS=0"; dir = "http"; defs = @("FWD_TLS=0") },
    @{ name = "FC_MAX_STATIONS=2 FWD_TLS=1 FC_DIAG_BENCH=1"; dir = "st2"; defs = @("FWD_TLS=1", "FC_MAX_STATIONS=2", "FC_DIAG_BENCH=1") }
)
foreach ($v in $variants) {
    $tdir = Join-Path $outDir $v.dir
    if (-not (Test-Path $tdir)) { New-Item -ItemType Directory $tdir | Out-Null }
    $texe = Join-Path $tdir ("test_fauxcloud_" + $v.dir + ".exe")
    if (Get-Command g++ -ErrorAction SilentlyContinue) {
        & g++ -std=c++17 -O1 -g -Wall -Wextra -Werror -Wno-missing-field-initializers -ffp-contract=off ($v.defs | ForEach-Object { "-D$_" }) -I $src @sources @cObjs -o $texe
        if ($LASTEXITCODE -ne 0) { throw ($v.name + " compile failed") }
    } else {
        $vd = ($v.defs | ForEach-Object { '/D' + $_ }) -join ' '
        $tcl = 'cl /nologo /std:c++17 /EHsc /W4 /WX /utf-8 /D_CRT_SECURE_NO_WARNINGS {5} /I "{0}" {1} {4} /Fo"{2}\\" /Fe"{3}" ' -f $src, $srcList, $tdir, $texe, $objList, $vd
        $tbat = Join-Path $outDir ("build_" + $v.dir + ".cmd")
        Set-Content -Path $tbat -Encoding ascii -Value @(
            '@echo off',
            ('call "{0}" >nul 2>nul <NUL' -f $vcvars.FullName),
            $tcl,
            'exit /b %errorlevel%'
        )
        & cmd /c "`"$tbat`""
        if ($LASTEXITCODE -ne 0) { throw ($v.name + " compile failed") }
    }
    $tout = & $texe
    if ($LASTEXITCODE -ne 0) { $tout | Write-Host; exit $LASTEXITCODE }
    Write-Host ($v.name + ": " + ($tout | Select-Object -Last 1))
}

# The outlook replay against a saved board block -- MATCH exits 0, one code changed exits 1
$rexe = Join-Path $outDir "forecast_replay.exe"
$rsrc = @(Get-ChildItem (Join-Path $src "*.cpp") | ForEach-Object { $_.FullName }) + (Join-Path $PSScriptRoot "forecast_replay.cpp")
if (Get-Command g++ -ErrorAction SilentlyContinue) {
    & g++ -std=c++17 -O1 -Wall -Wextra -Werror -Wno-missing-field-initializers -ffp-contract=off -I $src @rsrc @cObjs -o $rexe
    if ($LASTEXITCODE -ne 0) { throw "forecast_replay compile failed" }
} else {
    $rList = ($rsrc | ForEach-Object { '"' + $_ + '"' }) -join ' '
    $rcl = 'cl /nologo /std:c++17 /EHsc /W4 /WX /utf-8 /D_CRT_SECURE_NO_WARNINGS /I "{0}" {1} {4} /Fo"{2}\\" /Fe"{3}" >nul' -f $src, $rList, $outDir, $rexe, $objList
    $rbat = Join-Path $outDir "build_replay.cmd"
    Set-Content -Path $rbat -Encoding ascii -Value @(
        '@echo off',
        ('call "{0}" >nul 2>nul <NUL' -f $vcvars.FullName),
        $rcl,
        'exit /b %errorlevel%'
    )
    & cmd /c "`"$rbat`""
    if ($LASTEXITCODE -ne 0) { throw "forecast_replay compile failed" }
}
$fx = Join-Path $PSScriptRoot "fixtures"
& $rexe (Join-Path $fx "replay-hourly.json") (Join-Path $fx "replay-live.json") (Join-Path $fx "replay-bridge.json") | Out-Null
if ($LASTEXITCODE -ne 0) { Write-Host "FAIL forecast_replay: the fixture did not MATCH"; exit 1 }
$diffLive = Join-Path $outDir "replay-live-diff.json"
(Get-Content (Join-Path $fx "replay-live.json") -Raw).Replace('"p_falling"', '"p_steady"') | Set-Content -Path $diffLive -Encoding ascii -NoNewline
& $rexe (Join-Path $fx "replay-hourly.json") $diffLive (Join-Path $fx "replay-bridge.json") | Out-Null
if ($LASTEXITCODE -ne 1) { Write-Host "FAIL forecast_replay: a changed board code did not give DIFF"; exit 1 }
# T3 through --metar -- MATCH with the airport's drizzle + mist, DIFF without the file, nothing from 61 km
$h0 = Join-Path $fx "replay-hourly.json"; $b0 = Join-Path $fx "replay-bridge.json"; $t3 = Join-Path $fx "replay-t3-live.json"
& $rexe $h0 $t3 $b0 --metar (Join-Path $fx "replay-t3-metar.json") | Out-Null
if ($LASTEXITCODE -ne 0) { Write-Host "FAIL forecast_replay: the T3 fixture did not MATCH with --metar"; exit 1 }
& $rexe $h0 $t3 $b0 | Out-Null
if ($LASTEXITCODE -ne 1) { Write-Host "FAIL forecast_replay: the T3 fixture without --metar did not give DIFF"; exit 1 }
& $rexe $h0 (Join-Path $fx "replay-live.json") $b0 --metar (Join-Path $fx "replay-t3-metar-far.json") | Out-Null
if ($LASTEXITCODE -ne 0) { Write-Host "FAIL forecast_replay: an airport 61 km away changed the outlook"; exit 1 }
Write-Host "OK: forecast_replay MATCH / DIFF on the fixture"
# --what-if (fixed rules vs the learners in use): a calm evening at 0.5 C/h with L5 learned at 0.35 changes 6 of
# 8 hours to night_clear; the same file with every learner "default" changes none; the saved board block runs
$wiOut = & $rexe --what-if (Join-Path $fx "replay-whatif-hourly.json") (Join-Path $fx "replay-bridge.json") (Join-Path $fx "replay-learn.json")
if ($LASTEXITCODE -ne 0 -or -not (($wiOut -join "`n") -match "summary: 6 of 8 hours changed") -or -not (($wiOut -join "`n") -match "learned night_clear")) { Write-Host "FAIL forecast_replay --what-if"; $wiOut | Write-Host; exit 1 }
$defLearn = Join-Path $outDir "replay-learn-default.json"
(Get-Content (Join-Path $fx "replay-learn.json") -Raw).Replace('"in_use"', '"default"') | Set-Content -Path $defLearn -Encoding ascii -NoNewline
$wiOut = & $rexe --what-if (Join-Path $fx "replay-whatif-hourly.json") (Join-Path $fx "replay-bridge.json") $defLearn
if ($LASTEXITCODE -ne 0 -or -not (($wiOut -join "`n") -match "summary: 0 of 8 hours changed")) { Write-Host "FAIL forecast_replay --what-if (all default)"; exit 1 }
& $rexe --what-if (Join-Path $fx "replay-hourly.json") (Join-Path $fx "replay-bridge.json") (Join-Path $fx "replay-learn.json") | Out-Null
if ($LASTEXITCODE -ne 0) { Write-Host "FAIL forecast_replay --what-if on the board block"; exit 1 }
Write-Host "OK: forecast_replay --what-if"
exit 0
