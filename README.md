# my-driver

Android ARM64 loadable kernel module: process memory R/W, HW breakpoints, PTE inline hooks, touch injection, sensor spoofing, page-fault harvest, plus compile-time concealment layers for the LKM itself and for the client PID. The userspace fd is created through a magic `reboot()` handshake; no `/dev` node.

Defaults hide the LKM (`HIDE_SELF_MODULE=1`, `HIDE_VMAP=1`) and arm the PID-hiding path (`HIDE_TASK=1`). KGSL concealment is opt-in via a strength scale (`HIDE_KGSL_STRENGTH=0`).

## Build matrix

GitHub Actions builds the module against seven KMIs plus the arm64-v8a userspace client:

- `android12-5.10`
- `android13-5.10`
- `android13-5.15`
- `android14-5.15`
- `android14-6.1`
- `android15-6.6`
- `android16-6.12`

Runtime coverage today is `android15-6.6` (NP05J / Android 15 / kernel 6.6.56). The other legs are compile-validated.

## Build

Kernel code builds inside the matching `ghcr.io/ylarod/ddk:<kmi>-<release>` image (default release `20251104`, required by `android16-6.12` KCFI):

```bash
cd driver
make                                             # defaults
make HIDE_SELF_MODULE=1 HIDE_VMAP=1 HIDE_TASK=1  # everything on except KGSL
make HIDE_KGSL_STRENGTH=3                        # KGSL retro + proactive
```

## Userspace client

```bash
cmake -S client -B out/client -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-30 -DANDROID_STL=c++_static -DCMAKE_BUILD_TYPE=Release
cmake --build out/client
```

The client opens the fd lazily. `findPidByPackage()` matches the complete `argv[0]`; after `setTarget(pid)` the standard APIs live under `driver.memory`, `driver.touch`, `driver.gyro`, `driver.hwbp`, `driver.pteHook`, and `driver.hide`.

## Concealment layers

The driver has four independent hide surfaces. Each is compile-gated so a build only pays for what it enables.

### `HIDE_SELF_MODULE` (default 1)

Unlinks the LKM from `/proc/modules` and removes `/sys/module/<name>`. Additional passes: renames `mod->name` to `KCFG_DECOY_NAME` (default `iptable_filter`) before unlink so any snapshot of `struct module*` reads as an ordinary in-tree subsystem; zeroes `taints`, `srcversion`, `notes_attrs`, `modinfo_attrs`, `build_id`; and arms a kprobe on `m_show`/`s_show` so a stray seq_file walker that reinstates the module would still see it silently skipped. Implemented in `lifecycle.c::conceal_module()` + `module_hide.c`. Applies once at `init_driver`; irreversible for the module's lifetime.

### `HIDE_VMAP` (default 1)

Unlinks the LKM's data vmap area from `vmap_area_list` (source of `/proc/vmallocinfo`) and, when the single-root form exists (kernels < 6.9), also `rb_erase()`s it from `vmap_area_root`. Probe is `&drv` (core `.bss`) so the module's `__init` section is untouched and `do_free_init` still cleans up correctly. Falls back to no-op if `vmap_area_list` is not in kallsyms. See `lifecycle.c::conceal_vmap()`.

### `HIDE_TASK` (default 1) — PID + file/dir hider

`hide_task.c` registers a single kprobe on `filldir64` that services two hide sets: numeric `/proc/<pid>` directory entries (up to 8 slots) and exact-basename file/dir entries (up to 16 slots × 64 chars). A single hook covers every `iterate_dir` path — ext4, f2fs, tmpfs, overlayfs, proc, sysfs — because they all funnel through `filldir64` for the seq output. Direct `open()`/`stat()` still works; this is a readdir-level cloak.

Client API:

- PID: `add(pid)`, `remove(pid)`, `clear()`, `list()` (`HIDE_PID_ADD=0x50`..`HIDE_PID_LIST=0x53`).
- File/dir name: `HIDE_NAME_ADD=0x54`, `HIDE_NAME_REMOVE=0x55`, `HIDE_NAME_CLEAR=0x56`. `req.buf` points at the raw name bytes, `req.size` is its length.

### `HIDE_KGSL_STRENGTH` (default 0) — KGSL/Adreno hider

Values:

- `0` — off. `DRV_CMD_HIDE_KGSL` returns `-EOPNOTSUPP`.
- `1` — retroactive `rb_erase`. `hide_kgsl_by_pid()` walks the two `kgsl_process_private` rbtrees held by `kgsl_driver` and erases the entry whose PID string matches. One-shot per client call; KGSL will re-create the entry if the same PID reopens a GPU context.
- `2` — proactive kprobes. Four hooks: `kgsl_process_init_sysfs`, `kgsl_process_init_debugfs`, `sysfs_create_group`, and `kobject_init_and_add`. All pre-check `hide_task_contains(current->tgid)` and, for hidden PIDs, spoof `-ENOMEM` before the original runs. `sysfs_create_group` and `kobject_init_and_add` additionally walk the kobject parent chain (up to 7 levels) and only fire when a `"kgsl"` name is found in the ancestry, so unrelated sysfs creations pay a single lock-read.
- `3` — both.

`STRENGTH >= 2` requires `HIDE_TASK=1` (the proactive layer reads the shared hidden-PID list).

Versioned offsets for the `kgsl_driver` and `kgsl_process_private` structs live in `stealth.c`. The runtime `holder_ptr_looks_valid()` check guards against BSP drift.

## Hook APIs

`driver.hwbp` installs AArch64 hardware breakpoints and watchpoints per target thread. Types: `X` (execute, 4B only), `R`/`W`/`RW` (watchpoint, 1/2/4/8B). Overrides mutate X0..X30, PC, SIMD V0..V31 low/high before the target instruction retires; a 32-entry per-tracker hit ring optionally carries FPSIMD state (Q0..Q31 + fpsr/fpcr) captured at hit time. Trackers are fd-scoped — closing the driver fd sweeps only that fd's trackers, leaving other clients' state intact. Tracker identity uses the kernel PID object and target `mm`, preventing PID reuse and `execve()` redirection.

Feature flags (bitmask in `install(...flags)`):

| Flag | Bit | Effect |
| --- | ---: | --- |
| `DRV_HWBP_FLAG_BAIT_GUARD` | 0x1 | **Deprecated.** Historically redirected `addr` via `translate_bait`, but the silent rewrite hid the real tracker key from the caller. Now accepted for source-compat and silently dropped; call `DRV_CMD_HWBP_TRANSLATE_BAIT` explicitly and pass the resolved address to `install`. |
| `DRV_HWBP_FLAG_NOTIFY` | 0x2 | Deliver `signal_no` (default 43 — safely past Bionic's 32..34/40/41 reservations) to `notify_pid` on hit; `si_int` carries the tracker id. Delivered process-directed: picks the first thread whose signal mask does not block `sig`, otherwise falls back to the group leader (signal stays pending until unblocked, same as any queued RT signal). |
| `DRV_HWBP_FLAG_CAPTURE_FP` | 0x4 | Fill `q_lo/q_hi/fpsr/fpcr` in every hit record from `current->thread.uw.fpsimd_state`. |
| `DRV_HWBP_FLAG_TIMING_BYPASS` | 0x8 | Skip ring push + signal delivery on hit — overrides still applied. Reduces observable per-hit overhead to the perf overflow path. |

Per-tracker gates (each set via its own ioctl after install):

- `SET_SAMPLE(every=N)` — only every Nth hit fires; useful for high-frequency call sites.
- `SET_CONDITION({reg, op, value})` — hit is filtered unless `regs->X[reg] op value` matches (`op ∈ {EQ, NE, LT, LE, GT, GE}`).
- `SET_BYPASS_PID(pid)` — one-shot: the next hit whose `current->pid == pid` is silently consumed. Prevents self-recursion when the client itself calls the traced function.
- `SET_NOTIFY({notify_pid, signal_no})` — mutable signal target; `signal_no=0` picks the kernel default (43).
- `TRANSLATE_BAIT(addr)` — returns the LARGEST same-basename VMA cluster address for `addr` without installing anything; useful for probing an AC mapping layout ahead of `install`.

HWBP command range: primary `0x40..0x47` (INSTALL, REMOVE, SET_OVERRIDE, GET_HITS_LEGACY [refused with `EPROTO`], CLEAR_ALL, GET_CAPS, SET_SAMPLE, SET_CONDITION); extended `0x60..0x63` (SET_BYPASS_PID, SET_NOTIFY, TRANSLATE_BAIT, GET_HITS 800-byte at 0x63). GET_HITS lives in the extended range to avoid a former collision with `PTE_HOOK_INSTALL = 0x48` (review N1). `GET_CAPS` returns `{num_brps, num_wrps, ring_slots, max_overrides, hit_bytes, install_req_bytes, flags_supported, fp_ready, abi_generation}` — clients must confirm `abi_generation == DRV_HWBP_ABI_GENERATION` (currently 2), not just sizeof matches (review N2).

`driver.pteHook` installs a 32-byte constant-return stub in a private executable mapping. `returnConst<T>()` supports integral, enum, pointer, float, and double values; `returnVoid()` emits only a return. Stub starts with a BTI-compatible landing, validates one complete same-page private VMA, and records expected patched bytes. Reinstall, remove, and `clearAll()` refuse to overwrite a changed function.

PTE-hook commands: `INSTALL=0x48`, `REMOVE=0x49`, `CLEAR_ALL=0x4A`. Kind values: `CONST_U64=0`, reserved `TRAMPOLINE=1`, `CONST_FLOAT=2`, `CONST_DOUBLE=3`, `VOID_RET=4`.

Both APIs use access to the trusted driver fd as the permission boundary; they do not stop target threads, so callers must quiesce threads sharing the target `mm` while installing or removing a 32-byte patch.

## Benchmark snapshot

NP05J / Android 15 / kernel 6.6.56. Values in µs, mean per operation. Reference data, not a portability guarantee.

### Memory read

| Size | Driver | process_vm_readv | /proc/pid/mem |
| --- | ---: | ---: | ---: |
| 4 B | 0.699 | 1.162 | 1.518 |
| 1 KiB | 0.809 | 1.298 | 1.615 |
| 64 KiB | 12.293 | 15.762 | 23.481 |
| 1 MiB | 188.905 | 225.284 | 362.884 |
| 4 MiB | 796.633 | 815.044 | 1470.967 |

### Memory write

| Size | Driver | process_vm_writev |
| --- | ---: | ---: |
| 4 B | 0.710 | 1.166 |
| 4 KiB | 1.047 | 1.521 |
| 64 KiB | 12.651 | 15.875 |
| 1 MiB | 191.251 | 227.859 |

### MULTI_READ

| Entries | N × READ | MULTI_READ | Speedup |
| ---: | ---: | ---: | ---: |
| 8 | 5.913 | 1.819 | 3.25× |
| 128 | 92.547 | 12.573 | 7.36× |
| 512 | 373.199 | 46.071 | 8.10× |

### Hook ioctl latency

| Operation | p50 | p95 | p99 |
| --- | ---: | ---: | ---: |
| PTE install/update | 4.063 | 4.219 | 5.156 |
| HWBP install | 9.688 | 13.438 | 2448.438 |
| HWBP SET_OVERRIDE | 0.834 | 0.938 | 0.938 |

### HWBP capability report (via `DRV_CMD_HWBP_GET_CAPS`)

Runtime-reported on NP05J:

- `num_brps=6`, `num_wrps=4` — matches Snapdragon debug spec
- `ring_slots=32` per tracker
- `max_overrides=10` per tracker
- `hit_bytes=800` (X0..X30 + Q0..Q31 + fpsr/fpcr)
- `install_req_bytes=192`
- `fp_ready=1` — `fpsimd_preserve_current_state` resolved
- `flags_supported=0xE` — NOTIFY | CAPTURE_FP | TIMING_BYPASS (BAIT_GUARD flag deprecated, see table above)

### Verified stealth surface (autonomous `my-driver-test` on kernel 6.6.56-android15-8)

| Layer | Check | Result |
| --- | --- | --- |
| `/proc/modules` | `grep -E "my_driver\|iptable_filter"` | not listed |
| `/sys/module/` | `ls | grep my_driver` | not listed |
| `/proc/vmallocinfo` | `grep my_driver` | vmap unlinked |
| `/proc/kallsyms` | `grep -E "hwbp_install\|conceal_module\|hide_task_add"` | 0 matches (module symbols not exported) |
| `/sys/kernel/debug/kprobes/list` | endpoint | absent on hardened Android kernel — no leak possible |

### HWBP feature matrix (autonomous `my-driver-test`)

Per-feature runtime confirmation on NP05J (Android 15 / kernel 6.6.56-android15-8). All tests self-target the running process so results are reproducible without a game target.

| Test | Feature | Status | Note |
| --- | --- | --- | --- |
| S1 | driver.open via reboot handshake | PASS | fd installed |
| S2 | stealth × 3 (/proc/modules, /sys/module, /proc/vmallocinfo) | PASS × 3 | module invisible |
| S3 | HWBP CAPS ioctl | PASS | `brps=6 wrps=4 ring=32 fp_ready=1` |
| S4 | install matrix (R/W/RW/X × valid lens + rejects) | PASS × 6 | watchpoints (A.1) work |
| S5 | bypass_pid one-shot | PASS | `baseline=2 bypassed=1 (Δ=1)` |
| S6 | sample gate (every=3) | PASS | `baseline=6 gated=2` |
| S7 | conditional trigger `X0 == 42` | PASS | `baseline=3 gated=1` |
| S8 | async notify (sig 42, si_int=bp_id) | PASS | `signal 42 received si_int=12` — every thread in tgid gets the signal, so a blocked group leader can't strand it |
| S9 | FPSIMD Q0 capture | PASS | `Q0 = cafef00ddeadbeef:0123456789abcdef` — primed pattern intact in ring |
| S10 | translate_bait roundtrip | PASS | ioctl returns valid mapping (install no longer silently mutates the key) |
| S11 | timing detector | PASS | `base=3.6ns hwbp=1013.8ns timing_bypass=601.7ns (278× → 165×)` |
| S12 | file/dir name hide via filldir64 | PASS | marker vanishes from `ls /data/local/tmp/` (version-gated bool contract) |
| S13 | PID hide via filldir64 | PASS | child pid vanishes from /proc |
| S14 | fd-scoped tracker cleanup | PASS | alt fd close reclaims trackers |
| S15 | fd owner isolation | PASS | two clients on the same (pid, addr) get independent trackers; `clearAll` on one leaves the other alone |
| S16 | BAIT_GUARD flag now silently ignored | PASS | install with the deprecated flag returns success, tracker key unchanged |
| S17 | watchpoint one-shot fire | PASS | W-BP on `g_probe_word` records a hit, then auto-disables so a second store records nothing |
| S18 | legacy GET_HITS refused | PASS | old 0x43 ioctl returns `EPROTO` — stale clients get an authoritative ABI-break signal |
| S19 | BAIT_GUARD dropped from caps | PASS | `caps.flags_supported` no longer advertises the deprecated bit |
| S20 | PTE_HOOK routing intact | PASS | regression for N1 — `DRV_CMD_PTE_HOOK_INSTALL` reaches the PTE handler, not HWBP |

**Summary: 27 PASS / 0 FAIL / 0 SKIP on `android15-6.6` (NP05J).** Covers R1..R7 + N1..N5 review items: HWBP command range no longer collides with PTE (N1), `abi_generation` is negotiated at open (N2), re-install on an orphaned tracker resurrects it in place and preserves every sticky setter's state (N3), notify picks the first thread with the signal unblocked and falls back to leader as pending (N4, standard process-directed shape), S14 exhausts every HW slot from `alt` and confirms release() reclaims all of them (N5), and `reboot_handler_pre` debounces the duplicate arm64-wrapper hit that produced two handshake events per `open()` (E1).

## Layout

```text
driver/include/driver/uapi.h  shared kernel/userspace ABI
driver/src/lifecycle.c        module init + conceal_module + conceal_vmap
driver/src/comm.c             ioctl router + reboot()-magic handshake
driver/src/memory.c           pagewalk + process-memory R/W
driver/src/hwbp.c             hardware breakpoint subsystem
driver/src/user_hook.c        constant-return user-code hooks
driver/src/hide_task.c        filldir64 kprobe → /proc PID hider
driver/src/stealth.c          KGSL retroactive + proactive concealment
driver/src/sensor.c           HIDL/AIDL sensor uprobe
driver/src/input_synth.c      touch injection
driver/src/log.h              LOGE/LOGW/LOGN/LOGI/LOGD macros
client/src/Driver.{h,cpp}     userspace API (memory/touch/gyro/hwbp/pteHook/hide)
diagnostics/capture-kmsg.sh   Toybox-compatible kmsg capture helper
.github/workflows/build.yml   seven-KMI module + arm64 client CI
```

## Configurable knobs

```bash
make DRIVER_NAME=my-driver \
     TARGET_PKG='"cent.tmgp.sgame"' \
     HIDE_SELF_MODULE=1 HIDE_VMAP=1 HIDE_TASK=1 HIDE_KGSL_STRENGTH=0
```

`DRIVER_NAME` controls the module filename. `TARGET_PKG` selects the harvest package string. `REBOOT_MAGIC` controls the handshake. Concealment knobs are documented in the section above. Kbuild enforces `STRENGTH >= 2 → HIDE_TASK=1`.

## Caveats

The module has no unload entry point — kprobes, task work, and hook callbacks can retain module pointers. Reboot before replacing a loaded artefact. The memory path does not pin pages, so migration races and COW semantics remain. `HIDE_KGSL_STRENGTH >= 1` is not universal across Qualcomm BSPs; enable only after the runtime sanity checks pass. `diagnostics/capture-kmsg.sh` is a debug helper, not a production logger.

## License

GPL-2.0-only. See [LICENSE](LICENSE), [NOTICE.md](NOTICE.md), [CONTRIBUTING.md](CONTRIBUTING.md).
