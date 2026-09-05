package com.mydriver.test

/*
 * The canonical S1..S14 list mirrored from client/tests/test_all.cpp. Each
 * entry names the scenario and points at the JNI runner that executes it on
 * the device. Order matches test_all.cpp so a "Run All" reproduces the same
 * flow the CLI test binary does.
 */
data class TestSpec(
    val id: String,
    val title: String,
    val runner: () -> String,
)

object TestCatalog {
    val all: List<TestSpec> = listOf(
        TestSpec("S1",  "driver.open handshake",                       NativeBridge::runOpen),
        TestSpec("S2",  "stealth: /proc/modules + /sys/module + vmallocinfo", NativeBridge::runStealthSurface),
        TestSpec("S3",  "hwbp caps ioctl sanity",                      NativeBridge::runCaps),
        TestSpec("S4",  "install matrix (type × len × pass_through)",  NativeBridge::runInstallMatrix),
        TestSpec("S5",  "bypass_pid one-shot",                         NativeBridge::runBypassPid),
        TestSpec("S6",  "sample gate (every N)",                       NativeBridge::runSampleGate),
        TestSpec("S7",  "conditional trigger (X0 == 42)",              NativeBridge::runConditional),
        TestSpec("S8",  "async SIGRTMIN+1 notify",                     NativeBridge::runNotify),
        TestSpec("S9",  "FPSIMD Q0 capture",                           NativeBridge::runFpsimdCapture),
        TestSpec("S10", "translate_bait roundtrip",                    NativeBridge::runTranslateBait),
        TestSpec("S11", "timing detector: base vs hwbp vs timing_bypass", NativeBridge::runTimingDetector),
        TestSpec("S12", "file hide via hide_task_name_add",            NativeBridge::runFileHide),
        TestSpec("S13", "pid hide via hide_task_add",                  NativeBridge::runPidHide),
        TestSpec("S14", "fd-scoped cleanup (secondary fd)",            NativeBridge::runFdScopedCleanup),
    )
}
