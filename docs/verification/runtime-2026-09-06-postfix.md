# Runtime verification — post review-fixes

**Date:** 2026-09-06
**Device:** NP05J via `192.168.50.110:37793`
**Kernel:** `6.6.56-android15-8-g38447e018c92-ab12829524-4k`
**Android:** 15
**Driver:** `Ghopp3r/Android-Test-Driver@0d27d4b`
**CI run:** [34035757407](https://github.com/Ghopp3r/Android-Test-Driver/actions/runs/34035757407) (build.yml, all 8 jobs green)
**APK CI:** [34036325532](https://github.com/Ghopp3r/Android-Test-Driver/actions/runs/34036325532) (report-tests.yml)

## Artifact provenance

| File | MD5 (device) | MD5 (CI artifact) |
| --- | --- | --- |
| `/data/local/tmp/my-driver.ko` | `8d621e2e0b05fc9a95ec7897ace40884` | `8d621e2e0b05fc9a95ec7897ace40884` ✓ |
| `/data/local/tmp/my-driver-test` | `5807f7b2e5f1f805362fc1dd18e3eb08` | `5807f7b2e5f1f805362fc1dd18e3eb08` ✓ |

## CLI test suite

**Single run:** 23 PASS / 0 FAIL / 0 SKIP (`run1.log`).

**Stress:** 20 back-to-back runs, 20/20 PASS. 600 kernel-side `[memory-driver]`
events logged, 0 warnings, 0 errors, 0 refcount/leak lines.

## APK

**Reinstall:** `app-release.apk` from run 34036325532 (headSha `4e89f94`).

**UI:** 16 PASS / 0 FAIL / 0 SKIP. Every S1..S16 group green including the
new S15 (fd owner isolation) and S16 (BAIT_GUARD tracker key stability).

Screenshots:
- `apk_top.png` — S1..S5 header + summary chip
- `apk_bot.png` — S10..S16 detail

## Stealth surface

- `/proc/modules` — no `my_driver` row
- `/sys/module/` — no `my_driver` and no `iptable_filter` decoy row (both hidden)
- `/proc/vmallocinfo` — no matching entry
- `/proc/kallsyms` — no `hwbp_*` / `hide_task_*` / `module_hide_*` symbols

## Per-finding evidence

| # | Finding | Runtime evidence |
| --- | --- | --- |
| 1 | Watchpoint LR-return | S4 `R_len4`, `W_len1`, `RW_len4` install+remove PASS, test process survives without stack corruption |
| 2 | Gate skip + passThrough | S5/S6/S7 all PASS with 20-run stability, no `hwbp: rearm rc=` or `hwbp: advance rc=` errors in dmesg |
| 3 | notify_pid_ref UAF | 20 runs of S8 (setNotify+deliver+process exit), 0 refcount warnings in dmesg |
| 4 | filldir64 bool | S12 (name hide) + S13 (pid hide) both PASS; `ls` returns full listings without truncation |
| 5 | kobject_init_and_add | Probe removed; no `WARNING: at .../kobject.c` on module load or KGSL activity |
| 6 | GET_HITS ABI | Client's Driver::open() confirmed via `S1_driver_open: handshake ok` after CAPS negotiation |
| 7 | fd owner scope | S15 explicitly: `two clients isolated; secondary clearAll left primary intact` |
| 8 | Signal number | S8: `signal 42 received si_int=30` (past Bionic 32..34/40/41 reserved range) |
| 9 | send_sig_info fanout | S8 delivers even though process has an fpsimd handler + blocked signal mask |
| 10 | Condition atomicity | S7 stable across 20 runs, no partial-update PASSes on non-matching regs |
| 11 | translate_bait heuristic | S10: kernel returns a mapping, client decides whether to use it |
| 12 | Install autoreplace | S16: `install(BAIT_GUARD)` + `remove(addr)` roundtrip succeeds |
| 15 | Deploy helper | `deploy.sh` pulls `userspace-arm64-v8a` and pushes `my-driver-test` |
| 16 | Deploy SHA binding | `deploy.sh` selects run by `headSha == $(git rev-parse HEAD)` |
| 17 | Deploy insmod status | `__RC=$?` marker parsed back to caller pipeline |

## Files

- `run1.log` — one full test suite invocation
- `dmesg1.log` — bracketed kernel-side output for the same window
- `apk_top.png` / `apk_bot.png` — APK UI after auto-run
