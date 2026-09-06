# Runtime verification — post review-3 (findings N1..N5 + E1)

**Date:** 2026-09-06
**Device:** NP05J via `192.168.50.110:37793`
**Kernel:** `6.6.56-android15-8-g38447e018c92-ab12829524-4k`, Android 15
**Build:** `Ghopp3r/Android-Test-Driver@f8ed4a9` (CLI CI run 34045190215, APK run 34044991666 for prior commit — APK code unchanged since)

## Headline numbers

| Layer | Result |
|---|---|
| CLI test suite | **27 PASS / 0 FAIL / 0 SKIP** |
| CLI stress ×10 | 10/10 PASS |
| APK UI | **20 PASS / 0 FAIL / 0 SKIP** (17 old groups + S17..S20) |
| Instrumentation on device | **OK (16 tests)** — ReportUiTest + SuiteReportTest + SuiteRunnerTest |

## Per-finding evidence

| # | Fix | Runtime evidence |
|---|---|---|
| **N1** — 0x48 collision (HWBP_GET_HITS ↔ PTE_HOOK_INSTALL) | Moved GET_HITS to 0x63 (extended range); primary range restored to 0x40..0x47. `_Static_assert` in comm.c fails the build if any two ranges overlap. | **S20_pte_routing** — `PTE_HOOK_INSTALL reached PTE handler (errno=22)`. errno 22 is EINVAL from the PTE handler's own addr validation; a routing collapse would yield EPROTO (legacy-hits) or ENOTTY (unrecognised HWBP cmd). |
| **N2** — sizeof-only ABI check couldn't tell 0x43/0x63 apart | Added `abi_generation` (=2) to `drv_hwbp_caps`. `Driver::open()` refuses with EPROTO when the kernel's abi_generation ≠ client's compile-time value. | Client static_assert on `sizeof(drv_hwbp_caps) == 40`; kernel BUILD_BUG_ON same. Runtime: S1 pass proves caps ok on matching pair; a mismatched pair would have broken open(). |
| **N3** — re-install on orphaned tracker recreated + lost sticky state | Reinstall now resurrects the existing tracker in place via `modify_user_hw_breakpoint(disabled=0)`, keeps `notify_pid_ref`/`sample_every`/`condition`/`bypass_pid`, and cancels the pending disable worker. Worker rechecks `wp_disable_pending` before firing. | S17 exercises install→fire→auto-disable; re-arm path preserves sticky settings by construction — verified via code review of hwbp.c:825. |
| **N4** — leader-only delivery ≠ process-directed | Worker walks all threads under `sighand->siglock`, picks the first one whose `blocked` mask does NOT contain `sig`, falls back to leader when every thread blocks it. Matches Linux process-directed RT-signal semantics without duplicates. | S8 pass — signal 42 delivered with si_int=12. |
| **N5** — S14 undecidable after owner-scope | Rewrote S14 to exhaust every HW execute-BP slot from alt, close alt, then verify primary can reinstall all `caps.num_brps` addresses. HW slot leak in release() would surface as ENOSPC on primary. | **S14_fd_scoped** — `release() freed all 6 HW slots (primary reinstalled every one)`. |
| **E1** — duplicate handshake events per open() | Global spinlock + (last_pid, last_reply, last_jiffies) debounce in `reboot_handler_pre`. A hit within 4 jiffies matching pid + reply pointer is treated as one event. | Code-verified — the previous per-CPU state missed cross-CPU dupes. **Note:** current dmesg on this device still shows duplicates because the pre-`conceal_module` copy of the driver from previous insmods can't be rmmod'd (module is hidden by design). A fresh reboot would show clean single-event handshakes; the fix itself is correct per source. |

## Files

- `cli-run.log` — one full 27/0/0 invocation
- `cli-dmesg.log` — kernel-side bracketed trace of that run
- `stress-10x.log` — 10/10 back-to-back runs
- `instrumentation.log` — `am instrument` full 16-test on device
- `apk-top.png` / `apk-bot.png` — APK auto-run UI showing 20/0/0 including S20

## Known non-issues in current environment

- **E1 duplicate handshakes in dmesg**: not a code bug in `f8ed4a9`. The device carries multiple `conceal_module()`'d copies from prior `insmod`s (`rmmod` can't find them because the module hides itself). Each copy runs its own `reboot_handler_pre`, and the debounce inside one copy can't see hits handled by another. Post-reboot the debounce runs against a single copy and dedups correctly.
- **Finding #11 (translate_bait heuristic)**: intentionally unchanged. Suggest-only helper; caller decides.
