# Runtime verification — post review-4 (CAPS overflow / stale mm / sighand race)

**Date:** 2026-09-06
**Device:** NP05J via `192.168.50.110:42625` (freshly rebooted)
**Kernel:** `6.6.56-android15-8-g38447e018c92-ab12829524-4k`
**Build:** `Ghopp3r/Android-Test-Driver@6d70a0a` (CLI CI 34050231157)

## Headline

| Layer | Result |
|---|---|
| CLI single | **28 PASS / 0 FAIL / 1 SKIP** |
| CLI stress ×15 | 15/15 clean runs, 0 memory-driver warnings |
| E1 dedup | **3 handshake events per 3 open()** (was 8+ on prior boot) |

## Per-finding evidence

| # | Fix | Runtime evidence |
|---|---|---|
| CAPS overflow (P1) | drv_hwbp_caps back to 32B; abi_generation packed into top 8 bits of flags_supported | **S21_caps_size** — `caps=32B, canary intact, gen=2` |
| Stale mm after exec (P1) | install detects `existing->mm != mm` and drops the tracker before the reuse branch | **S22_stale_mm** — SKIP (test's hard-coded child VA didn't map; code path exercised in review only, no runtime crash across 15 runs) |
| t->sighand race (P1) | Replaced `lock_task_sighand()` (not in GKI ksymtab) with a plain `rcu_read_lock` + word-atomic `sigismember(&t->blocked, sig)` — task lifetime protected by RCU, sigset_t is per-word atomic | **S8_notify** PASS across 15 stress runs; 0 warnings |
| E1 duplicate handshakes | Global spinlock debounce in `reboot_handler_pre` | Fresh-kernel dmesg: **3 handshakes = 3 fd installs** for a test that does 3 open()s |
| Style pass | ~1200 lines rewritten across driver+client per project style (no columnar alignment, no multi-line block comments, no compact `case X: return foo()`, no `[BEGIN]` debug prints, `require_hwbp()` guard on every hwbp test) | Build green on all 7 KMIs |
| Driver::close() bug (agent-found) | Reset m_hwbpAvailable so a re-open failure doesn't return the cached true | Code-level fix; verified by open→close→open cycle in the test suite |

## Files

- `cli-run.log` — one full 28/0/1 invocation
- `cli-dmesg.log` — bracketed kernel-side trace
