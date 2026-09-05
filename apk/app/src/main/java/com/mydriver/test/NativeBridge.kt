package com.mydriver.test

/*
 * Thin JNI wrapper around Driver.cpp. Every runXxx() method mirrors the
 * corresponding S1..S14 scenario from client/tests/test_all.cpp and returns
 * a "TAG|one-line detail" string where TAG is PASS / FAIL / SKIP.
 */
object NativeBridge {
    init {
        System.loadLibrary("mydrivertest")
    }

    external fun openDriver(): Boolean
    external fun driverIsOpen(): Boolean
    external fun getCapsString(): String

    // Per-scenario runners — each returns "TAG|detail".
    external fun runStealthSurface(): String
    external fun runCaps(): String
    external fun runInstallMatrix(): String
    external fun runBypassPid(): String
    external fun runSampleGate(): String
    external fun runConditional(): String
    external fun runNotify(): String
    external fun runFpsimdCapture(): String
    external fun runTranslateBait(): String
    external fun runTimingDetector(): String
    external fun runFileHide(): String
    external fun runPidHide(): String
    external fun runFdScopedCleanup(): String

    // S1 handshake reported separately since a failure here blocks everything.
    fun runOpen(): String {
        val ok = openDriver()
        return if (ok) "PASS|handshake ok (fd acquired via reboot magic)"
               else "FAIL|driver open handshake failed — is the module loaded?"
    }
}
