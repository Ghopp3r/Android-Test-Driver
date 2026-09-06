# Runtime verification — post review-2 (findings R1..R7)

**Date:** 2026-09-06
**Device:** NP05J via `192.168.50.110:37793`
**Kernel:** `6.6.56-android15-8-g38447e018c92-ab12829524-4k`, Android 15
**Build:** `Ghopp3r/Android-Test-Driver@08c9cea` (CLI CI run 34039318106, APK run 34039318040)

## CLI test suite

**26 PASS / 0 FAIL / 0 SKIP**, stable over 10 back-to-back runs. See `cli-run.log`.

Bracketed dmesg (`cli-dmesg.log`) — 48 memory-driver lines, **0 warnings**, **0 rc≠0 delivery failures**. Only vendor noise (`alarmtimer suspend`) touches the log during the run.

## APK

- **Application UI:** 19 PASS / 0 FAIL / 0 SKIP (16 old groups + S17/S18/S19). Screenshots `apk-top.png`, `apk-bot.png`.
- **Instrumentation suite** on the device: `OK (16 tests)` — all `SuiteReportTest`, `SuiteRunnerTest`, and both `ReportUiTest` scenarios pass. See `instrumentation.log`.

## Per-review-finding evidence

| # | Finding | Fix | Runtime evidence |
| --- | --- | --- | --- |
| R1 | Watchpoint step-over missing | `hwbp_wp_schedule_disable()` — deferred `modify_user_hw_breakpoint(disabled=1)` after first fire | **S17_wp_oneshot**: watchpoint fires (1..N hits), auto-disables; second store records 0 hits |
| R2 | RT-signal fanout duplicates | Reverted to single `send_sig_info` on group leader | **S8_notify**: `signal 42 received si_int=12` — one hit → one signal |
| R3 | Mandatory CAPS in `open()` blocks non-HWBP clients | Soft-fail on `EOPNOTSUPP`/`ENOTTY`, hard-fail only on ABI mismatch | Client code path: `Driver::open()` now returns true with `hwbpAvailable()==false` when caps returns EOPNOTSUPP |
| R4 | Old client silently misreads new hits | `DRV_CMD_HWBP_GET_HITS` → 0x48; legacy 0x43 → `-EPROTO` | **S18_hits_legacy**: `legacy 0x43 returned EPROTO as designed` |
| R5 | BAIT_GUARD flag docs vs code | Flag now silently ignored + dropped from `flags_supported` | **S16_bait_key**: `BAIT_GUARD accepted, tracker key unchanged`; **S19_bait_deprecated**: `flags_supported=0xe` |
| R6 | APK instrumentation expects 14 rows | `ReportUiTest` uses `TestCatalog.all.size` | **Instrumentation `OK (16 tests)`** — expected string derived from catalog |
| R7 | S14/S15 undecidable after ownership | Rewrote S14 (third-fd install proof); sharpened S15 (three explicit asserts) | **S14**: `release() reaped alt's tracker; third fd installs cleanly`; **S15**: `alt.clearAll dropped only alt; primary intact and removable` |

## Files

- `cli-run.log` — one full test suite invocation
- `cli-dmesg.log` — bracketed kernel-side output
- `instrumentation.log` — `am instrument` full 16-test run
- `apk-top.png` / `apk-bot.png` — APK UI after auto-run

## What still stands

- **R2 tradeoff:** single-target delivery means a blocked-signal group leader can still strand the RT signal; callers must not block it in the leader thread. This is standard Linux signal convention, not a driver bug.
- **Finding #11 (translate_bait heuristic):** the largest-cluster picker was **not** changed. It's a suggestive helper only; callers must independently validate the returned address.
