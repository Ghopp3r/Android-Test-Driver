// SPDX-License-Identifier: GPL-2.0-only
package com.mydriver.test

data class TestSpec(val id: String, val title: String, val checks: List<String>)

object TestCatalog {
    val all = listOf(
        TestSpec("S1", "Driver connection", listOf("S1_driver_open")),
        TestSpec("S2", "Module visibility checks", listOf(
            "S2_proc_modules_hidden",
            "S2_sys_module_hidden",
            "S2_vmallocinfo_hidden",
        )),
        TestSpec("S3", "Breakpoint capabilities", listOf("S3_hwbp_caps")),
        TestSpec("S4", "Breakpoint installation", listOf(
            "S4_X_len4",
            "S4_X_len8_should_reject",
            "S4_R_len4",
            "S4_W_len1",
            "S4_RW_len4",
            "S4_R_passthru_should_reject",
        )),
        TestSpec("S5", "One-shot event filter", listOf("S5_bypass_pid")),
        TestSpec("S6", "Event sampling", listOf("S6_sample")),
        TestSpec("S7", "Register condition", listOf("S7_conditional")),
        TestSpec("S8", "Signal notification", listOf("S8_notify")),
        TestSpec("S9", "Floating-point capture", listOf("S9_fp_capture")),
        TestSpec("S10", "Address translation result", listOf("S10_translate_bait")),
        TestSpec("S11", "Timing measurements", listOf("S11_timing")),
        TestSpec("S12", "File listing check", listOf("S12_file_hide")),
        TestSpec("S13", "Process listing check", listOf("S13_pid_hide")),
        TestSpec("S14", "File descriptor ownership", listOf("S14_fd_scoped")),
        TestSpec("S15", "Fd owner isolation", listOf("S15_fd_owner")),
        TestSpec("S16", "Bait-guard tracker key", listOf("S16_bait_key")),
        TestSpec("S17", "Watchpoint one-shot fire", listOf("S17_wp_oneshot")),
        TestSpec("S18", "Legacy GET_HITS refused", listOf("S18_hits_legacy")),
        TestSpec("S19", "Bait-guard flag deprecated", listOf("S19_bait_deprecated")),
        TestSpec("S20", "PTE_HOOK routing intact", listOf("S20_pte_routing")),
        TestSpec("S21", "Caps ABI no overflow", listOf("S21_caps_size")),
        TestSpec("S22", "Stale mm dropped on exec", listOf("S22_stale_mm")),
    )
}
