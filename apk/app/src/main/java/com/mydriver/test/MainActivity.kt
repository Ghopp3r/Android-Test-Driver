// SPDX-License-Identifier: GPL-2.0-only
package com.mydriver.test

import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.widget.Button
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import java.util.concurrent.Executors

class MainActivity : AppCompatActivity() {
    internal var runnerFactory: () -> SuiteRunner = { SuiteRunner() }
    private val rows = TestCatalog.all.map(::TestRow).toMutableList()
    private val executor = Executors.newSingleThreadExecutor()
    private val ui = Handler(Looper.getMainLooper())
    private lateinit var adapter: TestAdapter
    private lateinit var summary: TextView
    private lateinit var runButton: Button
    private var generation = 0
    private var runner: SuiteRunner? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        val list = findViewById<RecyclerView>(R.id.list)
        adapter = TestAdapter(rows) { runAll() }
        list.layoutManager = LinearLayoutManager(this)
        list.adapter = adapter
        summary = findViewById(R.id.summary)
        runButton = findViewById(R.id.btnRunAll)
        runButton.setOnClickListener { runAll() }
        findViewById<Button>(R.id.btnClear).setOnClickListener { clearResults() }
        renderSummary()
        if (intent?.getBooleanExtra("auto_run", false) == true) runAll()
    }

    private fun clearResults() {
        generation++
        runner?.cancel()
        runner = null
        rows.forEachIndexed { index, row ->
            row.state = TestState.PENDING
            row.detail = "Not run"
            adapter.update(index)
        }
        runButton.isEnabled = true
        renderSummary()
    }

    private fun runAll() {
        if (runner != null) return
        val currentGeneration = ++generation
        val currentRunner = runnerFactory()
        runner = currentRunner
        runButton.isEnabled = false
        rows.forEachIndexed { index, row ->
            row.state = TestState.RUNNING
            row.detail = "Running suite..."
            adapter.update(index)
        }
        renderSummary()
        executor.submit {
            val report = currentRunner.runSuite()
            ui.post {
                if (isDestroyed || generation != currentGeneration) return@post
                rows.forEachIndexed { index, row ->
                    val result = report.result(row.spec.checks)
                    row.state = TestState.valueOf(result.outcome.name)
                    row.detail = result.detail
                    adapter.update(index)
                }
                runner = null
                runButton.isEnabled = true
                renderSummary()
            }
        }
    }

    private fun renderSummary() {
        val pass = rows.count { it.state == TestState.PASS }
        val fail = rows.count { it.state == TestState.FAIL }
        val skip = rows.count { it.state == TestState.SKIP }
        summary.text = "$pass PASS / $fail FAIL / $skip SKIP"
    }

    override fun onDestroy() {
        generation++
        runner?.cancel()
        ui.removeCallbacksAndMessages(null)
        executor.shutdownNow()
        super.onDestroy()
    }
}
