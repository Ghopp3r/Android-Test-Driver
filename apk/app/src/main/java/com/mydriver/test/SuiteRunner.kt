// SPDX-License-Identifier: GPL-2.0-only
package com.mydriver.test

import java.io.IOException
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit

// The process runner owns one child and returns an immutable suite report.
class SuiteRunner(private val startProcess: () -> Process = { ProcessBuilder("su", "-c", TEST_BINARY).redirectErrorStream(true).start() }) {
    private val processLock = Any()
    private var process: Process? = null
    private var cancelled = false

    fun cancel() = synchronized(processLock) {
        cancelled = true
        process?.destroyForcibly()
    }

    fun runSuite(): SuiteReport {
        val reader = Executors.newSingleThreadExecutor()
        var child: Process? = null
        try {
            synchronized(processLock) {
                if (cancelled) return SuiteReport(emptyList(), -1, "Run cancelled")
                child = startProcess()
                process = child
            }
            val running = checkNotNull(child)
            val output = reader.submit<List<String>> {
                val lines = mutableListOf<String>()
                var total = 0
                running.inputStream.bufferedReader().use { input ->
                    while (true) {
                        val line = input.readLine() ?: break
                        total += line.length
                        if (total > MAX_OUTPUT_CHARS) throw IOException("Suite output exceeds limit")
                        lines.add(line)
                    }
                }
                lines
            }
            if (!running.waitFor(TIMEOUT_SECONDS, TimeUnit.SECONDS)) {
                return SuiteReport(emptyList(), -1, "Suite timed out")
            }
            return SuiteReport(output.get(5, TimeUnit.SECONDS), running.exitValue())
        } catch (error: InterruptedException) {
            Thread.currentThread().interrupt()
            return SuiteReport(emptyList(), -1, "Run cancelled")
        } catch (error: Exception) {
            return SuiteReport(emptyList(), -1, "Cannot run suite: ${error.message}")
        } finally {
            child?.destroyForcibly()
            runCatching { child?.inputStream?.close() }
            reader.shutdownNow()
            synchronized(processLock) { process = null }
        }
    }

    companion object {
        private const val TEST_BINARY = "/data/local/tmp/my-driver-test"
        private const val TIMEOUT_SECONDS = 150L
        private const val MAX_OUTPUT_CHARS = 1024 * 1024
    }
}
