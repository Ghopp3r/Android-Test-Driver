// SPDX-License-Identifier: GPL-2.0-only
package com.mydriver.test

import android.widget.Button
import android.widget.TextView
import androidx.test.core.app.ActivityScenario
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class ReportUiTest {
    private class ReportProcess(fail: Boolean = false, val finished: CountDownLatch = CountDownLatch(0)) : Process() {
        private val checks = TestCatalog.all.flatMap { it.checks }
        private val code = if (fail) 1 else 0
        private val text = checks.mapIndexed { index, key ->
            val status = if (fail && index == 0) "FAIL" else "PASS"
            "[$status] $key fixture"
        }.joinToString("\n") + "\n== summary: ${checks.size - code} PASS, $code FAIL, 0 SKIP ==\n"

        override fun getInputStream() = ByteArrayInputStream(text.toByteArray())
        override fun getErrorStream() = ByteArrayInputStream(byteArrayOf())
        override fun getOutputStream() = ByteArrayOutputStream()
        override fun waitFor(): Int { finished.await(); return code }
        override fun waitFor(timeout: Long, unit: TimeUnit) = finished.await(timeout, unit)
        override fun exitValue() = code
        override fun destroy() { finished.countDown() }
        override fun destroyForcibly(): Process { destroy(); return this }
    }

    private fun awaitSummary(scenario: ActivityScenario<MainActivity>, expected: String) {
        var actual = ""
        val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5)
        while (System.nanoTime() < deadline) {
            scenario.onActivity { actual = it.findViewById<TextView>(R.id.summary).text.toString() }
            if (actual == expected) return
            Thread.sleep(20)
        }
        assertEquals(expected, actual)
    }

    @Test
    fun rerunStartsFreshAndShowsTheChangedResult() {
        ActivityScenario.launch(MainActivity::class.java).use { scenario ->
            var starts = 0
            scenario.onActivity { activity ->
                activity.runnerFactory = { SuiteRunner { starts++; ReportProcess(fail = starts == 2) } }
                activity.findViewById<Button>(R.id.btnRunAll).performClick()
            }
            awaitSummary(scenario, "14 PASS / 0 FAIL / 0 SKIP")
            scenario.onActivity { it.findViewById<Button>(R.id.btnRunAll).performClick() }
            awaitSummary(scenario, "13 PASS / 1 FAIL / 0 SKIP")
            assertEquals(2, starts)
        }
    }

    @Test
    fun clearDiscardsLateResultsAndAllowsAnotherRun() {
        ActivityScenario.launch(MainActivity::class.java).use { scenario ->
            val started = CountDownLatch(1)
            val held = ReportProcess(finished = CountDownLatch(1))
            scenario.onActivity { activity ->
                activity.runnerFactory = { SuiteRunner { started.countDown(); held } }
                activity.findViewById<Button>(R.id.btnRunAll).performClick()
            }
            assertTrue(started.await(5, TimeUnit.SECONDS))
            scenario.onActivity { it.findViewById<Button>(R.id.btnClear).performClick() }
            awaitSummary(scenario, "0 PASS / 0 FAIL / 0 SKIP")
            scenario.onActivity { activity ->
                activity.runnerFactory = { SuiteRunner { ReportProcess(fail = true) } }
                activity.findViewById<Button>(R.id.btnRunAll).performClick()
            }
            awaitSummary(scenario, "13 PASS / 1 FAIL / 0 SKIP")
        }
    }
}
