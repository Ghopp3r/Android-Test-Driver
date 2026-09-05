package com.mydriver.test

import java.io.BufferedReader
import java.io.InputStreamReader

/*
 * All test scenarios run as the on-device `my-driver-test` binary under
 * root through `su -c` — the kernel driver uses reboot(2) as a covert
 * handshake, and Android's seccomp filter blocks that syscall from any
 * unprivileged app process (SIGSYS). Delegating to a root child keeps the
 * APK sandboxed and cleanly separates trust boundaries.
 *
 * The binary must be present at /data/local/tmp/my-driver-test and marked
 * executable — `tools/deploy.sh` from Android-Test-Driver pushes it as
 * part of the standard install. The APK does not carry it inside its own
 * lib/ because SELinux would deny execmem for /data/app-shipped ELFs.
 */
object NativeBridge {
    private const val TEST_BIN = "/data/local/tmp/my-driver-test"

    private var cached: List<String>? = null

    /* Runs the full suite once and caches the per-test lines. Repeated
     * per-scenario callers pick their row from the cache — one 5-second
     * child exec instead of 14. Recompute with clear() if needed. */
    fun clear() { synchronized(this) { cached = null } }

    private fun runAllOnce(): List<String> = synchronized(this) {
        cached?.let { return it }
        val pb = ProcessBuilder("su", "-c", TEST_BIN)
            .redirectErrorStream(true)
        val proc = try { pb.start() } catch (t: Throwable) {
            cached = listOf("FAIL|cannot exec su: ${t.message}")
            return cached!!
        }
        val out = mutableListOf<String>()
        BufferedReader(InputStreamReader(proc.inputStream)).useLines { seq ->
            seq.forEach { out.add(it) }
        }
        proc.waitFor()
        if (out.isEmpty()) out.add("FAIL|su -c $TEST_BIN produced no output (exit=${proc.exitValue()})")
        cached = out
        out
    }

    /* Match "[PASS] Sx_scenario_key   detail..." lines and return the
     * matching row as "TAG|detail". */
    private fun pluck(scenarioKey: String): String {
        val lines = runAllOnce()
        val re = Regex("""^\[(PASS|FAIL|SKIP)]\s+${Regex.escape(scenarioKey)}\b\s*(.*)$""")
        for (line in lines) {
            val m = re.matchEntire(line.trim()) ?: continue
            return "${m.groupValues[1]}|${m.groupValues[2].trim()}"
        }
        // Not found — surface first non-BEGIN line for context.
        val ctx = lines.firstOrNull { !it.startsWith("[BEGIN]") && it.isNotBlank() } ?: "no output"
        return "FAIL|$scenarioKey not reported (${ctx.take(80)})"
    }

    fun runOpen(): String            = pluck("S1_driver_open")
    fun runStealthSurface(): String  = pluck("S2_proc_modules_hidden")
    fun runCaps(): String            = pluck("S3_hwbp_caps")
    fun runInstallMatrix(): String   = pluck("S4_X_len4")
    fun runBypassPid(): String       = pluck("S5_bypass_pid")
    fun runSampleGate(): String      = pluck("S6_sample")
    fun runConditional(): String     = pluck("S7_conditional")
    fun runNotify(): String          = pluck("S8_notify")
    fun runFpsimdCapture(): String   = pluck("S9_fp_capture")
    fun runTranslateBait(): String   = pluck("S10_translate_bait")
    fun runTimingDetector(): String  = pluck("S11_timing")
    fun runFileHide(): String        = pluck("S12_file_hide")
    fun runPidHide(): String         = pluck("S13_pid_hide")
    fun runFdScopedCleanup(): String = pluck("S14_fd_scoped")
}
