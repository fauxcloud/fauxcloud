// diag_bench.h -- the bench-diagnostics build flag. Plain C++, host-compilable; included by src/fwd.h (next
// to FWD_TLS) and by the few headers that render or carry a bench diagnostic.
//
// FC_DIAG_BENCH=1 builds the R&D diagnostics in: diag.tasks, the
// send low-waters, the forwarding tick in parts, the METAR page-idle statistics, ota_heap, http_busy_ms_max, the learner
// gates and replay figures, the last.diag stack/curve/record extras. The release (0) neither computes nor renders them:
// their keys are absent from the JSON (not null, not {}), and CI checks the release objects for their symbols and the
// release ELF for their key strings. The bench image is published per release as fauxcloud-<ver>-diag.bin and carries
// "+diag" in its version. Build it with: tools\build.ps1 fauxcloud -Define FWD_TLS=1,FC_DIAG_BENCH=1
#pragma once

#ifndef FC_DIAG_BENCH
#define FC_DIAG_BENCH 0
#endif
