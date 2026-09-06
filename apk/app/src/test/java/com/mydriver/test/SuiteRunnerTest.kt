// SPDX-License-Identifier: GPL-2.0-only
package com.mydriver.test

import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.util.concurrent.TimeUnit
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class SuiteRunnerTest {
    private class TestProcess(text: String, private val code: Int, private val finished: Boolean = true) : Process() {
        private val input = ByteArrayInputStream(text.toByteArray())
        var destroyed = false

        override fun getInputStream() = input
        override fun getErrorStream() = ByteArrayInputStream(byteArrayOf())
        override fun getOutputStream() = ByteArrayOutputStream()
        override fun waitFor() = code
        override fun waitFor(timeout: Long, unit: TimeUnit) = finished
        override fun exitValue() = code
        override fun destroy() { destroyed = true }
        override fun destroyForcibly(): Process { destroy(); return this }
    }

    @Test
    fun eachInvocationStartsANewProcess() {
        var starts = 0
        val runner = SuiteRunner {
            starts++
            if (starts == 1) TestProcess("[PASS] check ok\n== summary: 1 PASS, 0 FAIL, 0 SKIP ==\n", 0)
            else TestProcess("[FAIL] check changed\n== summary: 0 PASS, 1 FAIL, 0 SKIP ==\n", 1)
        }
        assertEquals(TestOutcome.PASS, runner.runSuite().result(listOf("check")).outcome)
        assertEquals(TestOutcome.FAIL, runner.runSuite().result(listOf("check")).outcome)
        assertEquals(2, starts)
    }

    @Test
    fun timeoutDestroysTheProcessAndFails() {
        val child = TestProcess("[PASS] check ok\n", 0, false)
        val result = SuiteRunner { child }.runSuite().result(listOf("check"))
        assertEquals(TestOutcome.FAIL, result.outcome)
        assertTrue(result.detail.contains("timed out"))
        assertTrue(child.destroyed)
    }

    @Test
    fun cancellationBeforeStartDoesNotLaunchAnything() {
        var started = false
        val runner = SuiteRunner { started = true; TestProcess("", 0) }
        runner.cancel()
        assertEquals(TestOutcome.FAIL, runner.runSuite().result(listOf("check")).outcome)
        assertEquals(false, started)
    }

    @Test
    fun launchFailureProducesAReport() {
        val result = SuiteRunner { throw java.io.IOException("missing helper") }.runSuite().result(listOf("check"))
        assertEquals(TestOutcome.FAIL, result.outcome)
        assertTrue(result.detail.contains("missing helper"))
    }
}
