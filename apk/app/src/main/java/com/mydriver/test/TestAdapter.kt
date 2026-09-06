// SPDX-License-Identifier: GPL-2.0-only
package com.mydriver.test

import android.graphics.Color
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.TextView
import androidx.recyclerview.widget.RecyclerView

enum class TestState(val label: String, val bg: Int) {
    PENDING("—", Color.parseColor("#9E9E9E")),
    RUNNING("...", Color.parseColor("#1976D2")),
    PASS("PASS", Color.parseColor("#2E7D32")),
    FAIL("FAIL", Color.parseColor("#C62828")),
    SKIP("SKIP", Color.parseColor("#616161")),
}

data class TestRow(
    val spec: TestSpec,
    var state: TestState = TestState.PENDING,
    var detail: String = "Not run",
)

class TestAdapter(
    private val rows: MutableList<TestRow>,
    private val onRun: (Int) -> Unit,
) : RecyclerView.Adapter<TestAdapter.VH>() {

    class VH(v: View) : RecyclerView.ViewHolder(v) {
        val tag: TextView = v.findViewById(R.id.tag)
        val name: TextView = v.findViewById(R.id.name)
        val detail: TextView = v.findViewById(R.id.detail)
        val btn: Button = v.findViewById(R.id.btnRun)
    }

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): VH {
        val v = LayoutInflater.from(parent.context)
            .inflate(R.layout.item_test, parent, false)
        return VH(v)
    }

    override fun onBindViewHolder(h: VH, position: Int) {
        val row = rows[position]
        h.tag.text = row.state.label
        h.tag.setBackgroundColor(row.state.bg)
        h.name.text = "${row.spec.id}: ${row.spec.title}"
        h.detail.text = row.detail
        h.btn.isEnabled = rows.none { it.state == TestState.RUNNING }
        h.btn.setText(R.string.run_all)
        h.btn.setOnClickListener {
            val index = h.bindingAdapterPosition
            if (index != RecyclerView.NO_POSITION) onRun(index)
        }
    }

    override fun getItemCount() = rows.size

    fun update(index: Int) {
        notifyItemChanged(index)
    }
}
