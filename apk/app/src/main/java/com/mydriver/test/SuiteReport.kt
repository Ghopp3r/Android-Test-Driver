// SPDX-License-Identifier: GPL-2.0-only
package com.mydriver.test

enum class TestOutcome {
    PASS,
    FAIL,
    SKIP,
}

data class TestResult(val outcome: TestOutcome, val detail: String)

// A report belongs to one completed process and never survives into another run.
class SuiteReport(lines: List<String>, exitCode: Int, error: String? = null) {
    private val results = linkedMapOf<String, TestResult>()
    private var failure = error

    init {
        val counts = IntArray(TestOutcome.entries.size)
        var summary: List<Int>? = null
        for (line in lines) {
            val resultMatch = resultPattern.matchEntire(line.trim())
            if (resultMatch != null) {
                val outcome = TestOutcome.valueOf(resultMatch.groupValues[1])
                val key = resultMatch.groupValues[2]
                val result = TestResult(outcome, resultMatch.groupValues[3].trim())
                if (results.put(key, result) != null) failure = "Duplicate result: $key"
                counts[outcome.ordinal]++
                continue
            }
            val summaryMatch = summaryPattern.matchEntire(line.trim()) ?: continue
            if (summary != null) failure = "Duplicate suite summary"
            summary = summaryMatch.groupValues.drop(1).map { it.toIntOrNull() ?: -1 }
        }
        if (summary == null) failure = failure ?: "Suite ended without a summary (exit=$exitCode)"
        else if (summary != counts.toList()) failure = failure ?: "Suite summary does not match results"
        val expectedExit = if (counts[TestOutcome.FAIL.ordinal] > 0) 1 else 0
        if (exitCode != expectedExit) failure = failure ?: "Unexpected suite exit: $exitCode"
    }

    fun result(keys: List<String>): TestResult {
        failure?.let { return TestResult(TestOutcome.FAIL, it) }
        if (keys.isEmpty()) return TestResult(TestOutcome.FAIL, "Scenario has no checks")
        val missing = keys.filterNot(results::containsKey)
        if (missing.isNotEmpty()) return TestResult(TestOutcome.FAIL, "Missing: ${missing.joinToString()}")
        val selected = keys.map { results.getValue(it) }
        val outcome = when {
            selected.any { it.outcome == TestOutcome.FAIL } -> TestOutcome.FAIL
            selected.any { it.outcome == TestOutcome.SKIP } -> TestOutcome.SKIP
            else -> TestOutcome.PASS
        }
        val detail = keys.zip(selected).joinToString("\n") { (key, result) ->
            "$key: ${result.outcome} ${result.detail}"
        }
        return TestResult(outcome, detail)
    }

    companion object {
        private val resultPattern = Regex("""^\[(PASS|FAIL|SKIP)]\s+(\S+)\s*(.*)$""")
        private val summaryPattern = Regex("""^== summary: (\d+) PASS, (\d+) FAIL, (\d+) SKIP ==$""")
    }
}
