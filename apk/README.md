# MyDriverTest — Android APK

Runs the S1..S14 scenarios from `client/tests/test_all.cpp` on-device with a
touch UI, so you can validate a freshly-loaded `my-driver.ko` on any AArch64
phone without wiring an `adb push` + `su -c` loop for every rebuild.

## Layout

- `settings.gradle.kts`, `build.gradle.kts`, `gradle.properties`, `local.properties`
- `keystore/debug.jks` — auto-generated on first build (`alias=debug`, all
  passwords `android`, `CN=MyDriverTest`). Signs both debug and release so
  `adb install -r` never trips Play protection.
- `app/build.gradle.kts` — AGP 8.5.2, Kotlin 1.9.24, targetSdk/compileSdk 35,
  minSdk 28, `abiFilters = arm64-v8a`, NDK 29.
- `app/src/main/java/com/mydriver/test/*` — MainActivity, TestCatalog,
  TestAdapter, NativeBridge JNI decls.
- `app/src/main/cpp/CMakeLists.txt` — reuses `client/src/Driver.cpp` and
  `driver/include/driver/uapi.h` from the parent repo directly. No source is
  duplicated.
- `app/src/main/cpp/jni_bridge.cpp` — one Java_..._runXxx() entry per S* case,
  each returns `"TAG|detail"` (TAG in `{PASS, FAIL, SKIP}`).

## Build

```
set JAVA_HOME=E:\Utils\JDK
E:\Utils\gradle-8.9\bin\gradle.bat -p E:\Projects\AndroidMemoryDriver\apk assembleRelease
```

The finished, signed APK ends up at:

```
apk/build/outputs/apk/release/app-release.apk
```

and is copied to a stable name:

```
apk/my-driver-test.apk
```

## Install & run

```
adb install -r E:\Projects\AndroidMemoryDriver\apk\my-driver-test.apk
adb shell am start -n com.mydriver.test/.MainActivity
```

The launcher shows one row per scenario with a per-row `Run` button and a
`Run All` button in the top bar. Each row shows `PASS`, `FAIL`, or `SKIP` plus
a one-line reason (mirroring the `report()` output of `test_all.cpp`).

## Scenarios wired up

| Row | Source (`test_all.cpp`) | What it checks |
| --- | --- | --- |
| S1  | `test_open`                    | reboot() handshake yields a driver fd |
| S2  | `test_stealth_surface`         | module invisible in `/proc/modules`, `/sys/module`, `/proc/vmallocinfo` |
| S3  | `test_caps`                    | `DRV_CMD_HWBP_GET_CAPS` returns sane brps/wrps/hit_bytes |
| S4  | `test_install_matrix`          | install matrix (type × len × pass_through) accepts/rejects as expected |
| S5  | `test_bypass_pid`              | `setBypassPid` swallows the next hit |
| S6  | `test_sample_gate`             | `setSample(every=3)` divides hit rate |
| S7  | `test_conditional`             | `setCondition(X0==42)` filters other calls |
| S8  | `test_notify`                  | `DRV_HWBP_FLAG_NOTIFY` delivers `SIGRTMIN+1` |
| S9  | `test_fpsimd_capture`          | `DRV_HWBP_FLAG_CAPTURE_FP` captures Q0 lo/hi |
| S10 | `test_translate_bait`          | `translateBait` returns non-zero |
| S11 | `test_timing_detector`         | benchmarks base vs HWBP vs HWBP+`TIMING_BYPASS` |
| S12 | `test_file_hide`               | `HIDE_NAME_ADD` hides a marker from `ls` |
| S13 | `test_pid_hide`                | `HIDE_PID_ADD` hides a forked child from `/proc` |
| S14 | `test_fd_scoped_cleanup`       | tracker installed on a secondary fd disappears when that fd closes |

## Notes / engineering decisions

- Gradle wrapper is intentionally NOT vendored — the repo has `gradle 8.9`
  under `E:\Utils\gradle-8.9`. Invoke via `gradle.bat` (or `gradle`) directly.
- `local.properties` pins the SDK to `E:\Utils\AndroidStudioSDK`, the NDK to
  `ndk\29.0.14033849` (already in that SDK) and CMake to `cmake\4.1.1`.
- Only `arm64-v8a` is built. The driver targets AArch64; no other ABI is worth
  the size hit.
- `Driver.cpp` is compiled *directly* from `../client/src` via CMake — the APK
  never keeps its own stale copy. Change the driver ABI once, both binaries
  update.
- S12/S13 use `popen("ls ...")` mirroring the CLI tests. `/data/local/tmp/`
  may be non-writable by an untrusted app; in that case S12 reports SKIP.
- Signing key is a per-repo self-signed dev key; do NOT publish to Play with it.
