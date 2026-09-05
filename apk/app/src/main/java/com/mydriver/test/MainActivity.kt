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
    private val rows = mutableListOf<TestRow>()
    private lateinit var adapter: TestAdapter
    private lateinit var summary: TextView
    private val io = Executors.newSingleThreadExecutor()
    private val ui = Handler(Looper.getMainLooper())

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        rows.clear()
        rows.addAll(TestCatalog.all.map { TestRow(it) })

        val list = findViewById<RecyclerView>(R.id.list)
        list.layoutManager = LinearLayoutManager(this)
        adapter = TestAdapter(rows) { idx -> runOne(idx) }
        list.adapter = adapter

        summary = findViewById(R.id.summary)
        renderSummary()

        findViewById<Button>(R.id.btnRunAll).setOnClickListener { runAll() }
        findViewById<Button>(R.id.btnClear).setOnClickListener {
            rows.forEachIndexed { i, r ->
                r.state = TestState.PENDING
                r.detail = "Not run"
                adapter.update(i)
            }
            renderSummary()
        }

        /* Auto-run on launch if invoked with the extra: makes `adb am start
         * -n com.mydriver.test/.MainActivity --ez auto_run true` fully
         * autonomous — no touch input needed. */
        if (intent?.getBooleanExtra("auto_run", false) == true) {
            list.postDelayed({ runAll() }, 300)
        }
    }

    private fun runOne(index: Int) {
        val row = rows[index]
        row.state = TestState.RUNNING
        row.detail = "running..."
        adapter.update(index)

        io.submit {
            val result = try {
                row.spec.runner()
            } catch (t: Throwable) {
                "FAIL|threw ${t.javaClass.simpleName}: ${t.message}"
            }
            val bar = result.indexOf('|')
            val tag = if (bar > 0) result.substring(0, bar) else "FAIL"
            val detail = if (bar > 0) result.substring(bar + 1) else result
            ui.post {
                row.state = when (tag) {
                    "PASS" -> TestState.PASS
                    "SKIP" -> TestState.SKIP
                    else -> TestState.FAIL
                }
                row.detail = detail
                adapter.update(index)
                renderSummary()
            }
        }
    }

    private fun runAll() {
        io.submit {
            for (i in rows.indices) {
                val row = rows[i]
                ui.post {
                    row.state = TestState.RUNNING
                    row.detail = "running..."
                    adapter.update(i)
                }
                val result = try { row.spec.runner() }
                             catch (t: Throwable) { "FAIL|threw ${t.javaClass.simpleName}: ${t.message}" }
                val bar = result.indexOf('|')
                val tag = if (bar > 0) result.substring(0, bar) else "FAIL"
                val detail = if (bar > 0) result.substring(bar + 1) else result
                ui.post {
                    row.state = when (tag) {
                        "PASS" -> TestState.PASS
                        "SKIP" -> TestState.SKIP
                        else -> TestState.FAIL
                    }
                    row.detail = detail
                    adapter.update(i)
                    renderSummary()
                }
                // If S1 (driver.open) itself fails, later ioctls will all fail; still run them but they will FAIL fast.
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
        super.onDestroy()
        io.shutdownNow()
    }
}
