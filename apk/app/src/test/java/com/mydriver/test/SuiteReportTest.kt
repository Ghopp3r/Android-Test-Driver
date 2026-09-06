// SPDX-License-Identifier: GPL-2.0-only
package com.mydriver.test

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class SuiteReportTest {
    @Test
    fun failureInLaterSubcheckFailsTheGroup() {
        val report = SuiteReport(listOf(
            "[PASS] group_first accepted",
            "[FAIL] group_second rejected",
            "== summary: 1 PASS, 1 FAIL, 0 SKIP ==",
        ), 1)
        val result = report.result(listOf("group_first", "group_second"))
        assertEquals(TestOutcome.FAIL, result.outcome)
        assertTrue(result.detail.contains("group_second: FAIL"))
    }

    @Test
    fun missingSubcheckCannotPass() {
        val report = SuiteReport(listOf(
            "[PASS] first accepted",
            "== summary: 1 PASS, 0 FAIL, 0 SKIP ==",
        ), 0)
        assertEquals(TestOutcome.FAIL, report.result(listOf("first", "missing")).outcome)
    }

    @Test
    fun duplicateResultIsRejected() {
        val report = SuiteReport(listOf(
            "[FAIL] check rejected",
            "[PASS] check accepted",
            "== summary: 1 PASS, 1 FAIL, 0 SKIP ==",
        ), 1)
        assertEquals(TestOutcome.FAIL, report.result(listOf("check")).outcome)
    }

    @Test
    fun prematureExitCannotPass() {
        val report = SuiteReport(listOf("[PASS] check accepted"), 137)
        assertEquals(TestOutcome.FAIL, report.result(listOf("check")).outcome)
    }

    @Test
    fun summaryMustMatchTheParsedResults() {
        val report = SuiteReport(listOf(
            "[PASS] check accepted",
            "== summary: 2 PASS, 0 FAIL, 0 SKIP ==",
        ), 0)
        assertEquals(TestOutcome.FAIL, report.result(listOf("check")).outcome)
    }

    @Test
    fun failedExitWithoutAFailedResultCannotPass() {
        val report = SuiteReport(listOf(
            "[PASS] check accepted",
            "== summary: 1 PASS, 0 FAIL, 0 SKIP ==",
        ), 1)
        assertEquals(TestOutcome.FAIL, report.result(listOf("check")).outcome)
    }

    @Test
    fun skippedSubcheckDoesNotBecomePass() {
        val report = SuiteReport(listOf(
            "[PASS] first accepted",
            "[SKIP] second unavailable",
            "== summary: 1 PASS, 0 FAIL, 1 SKIP ==",
        ), 0)
        assertEquals(TestOutcome.SKIP, report.result(listOf("first", "second")).outcome)
    }

    @Test
    fun exactKeysPreventPrefixMatches() {
        val report = SuiteReport(listOf(
            "[PASS] check_extra accepted",
            "== summary: 1 PASS, 0 FAIL, 0 SKIP ==",
        ), 0)
        assertEquals(TestOutcome.FAIL, report.result(listOf("check")).outcome)
    }

    @Test
    fun cleanGroupRemainsPassWhenAnotherGroupFails() {
        val report = SuiteReport(listOf(
            "[PASS] first accepted",
            "[FAIL] second rejected",
            "== summary: 1 PASS, 1 FAIL, 0 SKIP ==",
        ), 1)
        assertEquals(TestOutcome.PASS, report.result(listOf("first")).outcome)
    }

    @Test
    fun separateRunsCannotReuseThePreviousOutcome() {
        val first = SuiteReport(listOf("[PASS] check ok", "== summary: 1 PASS, 0 FAIL, 0 SKIP =="), 0)
        val second = SuiteReport(listOf("[FAIL] check broken", "== summary: 0 PASS, 1 FAIL, 0 SKIP =="), 1)
        assertEquals(TestOutcome.PASS, first.result(listOf("check")).outcome)
        assertEquals(TestOutcome.FAIL, second.result(listOf("check")).outcome)
    }
}
