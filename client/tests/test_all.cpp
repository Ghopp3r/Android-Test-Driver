// SPDX-License-Identifier: GPL-2.0
//
// my-driver-test: autonomous feature verification for the on-device kernel
// module. Runs as an unprivileged binary spawned under `su -c` from adb; each
// test prints [PASS]/[FAIL]/[SKIP] with a one-line reason and the process
// exits non-zero if any FAIL was recorded. No interactive prompts — safe to
// invoke from tools/deploy.sh or CI matrix jobs.
//
// Coverage map (matches Ghopper_Improvement_Plan.md):
//   S1  driver.open handshake
//   S2  stealth: /proc/modules + /sys/module + /proc/vmallocinfo
//   S3  A.3 caps ioctl returns sane HW breakpoint counts
//   S4  A.1 install matrix (bp_type × bp_len × pass_through)
//   S5  E.HWBP.5 bypass_pid one-shot
//   S6  E.HWBP.3 sample gate (every N)
//   S7  E.HWBP.4 conditional trigger
//   S8  E.HWBP.1 async SIGRTMIN+1 notify
//   S9  E.HWBP.2 FPSIMD Q/D/S capture in ring
//   S10 E.HWBP.6 translate_bait roundtrip
//   S11 timing detector: baseline vs HWBP vs HWBP+TIMING_BYPASS
//   S12 B.1 file hide via hide_task_name_add
//   S13 PID hide via hide_task_add
//   S14 A.2 fd-scoped cleanup (spawn a second fd, install, close, verify)

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <csignal>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <optional>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "Driver.h"

namespace {

int g_fails = 0;
int g_passes = 0;
int g_skips = 0;

void report(const char* tag, const char* name, const char* fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	char buf[512];
	std::vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	std::printf("[%s] %-40s %s\n", tag, name, buf);
	if (tag[0] == 'P') ++g_passes;
	else if (tag[0] == 'F') ++g_fails;
	else ++g_skips;
}
#define PASS(name, ...) report("PASS", name, __VA_ARGS__)
#define FAIL(name, ...) report("FAIL", name, __VA_ARGS__)
#define SKIP(name, ...) report("SKIP", name, __VA_ARGS__)

// ---------------------------------------------------------------------------
// Test S1 — driver handshake
// ---------------------------------------------------------------------------
bool test_open() {
	if (!driver.open()) {
		FAIL("S1_driver_open", "errno=%d (%s)", errno, std::strerror(errno));
		return false;
	}
	PASS("S1_driver_open", "handshake ok");
	return true;
}

// ---------------------------------------------------------------------------
// S2 — stealth surface (module invisibility from /proc + /sys)
// ---------------------------------------------------------------------------
bool file_grep(const char* path, const char* needle) {
	FILE* f = std::fopen(path, "r");
	if (!f) return false;
	char line[512];
	while (std::fgets(line, sizeof(line), f)) {
		if (std::strstr(line, needle)) { std::fclose(f); return true; }
	}
	std::fclose(f);
	return false;
}
bool dir_has(const char* path, const char* needle) {
	// Simple listing via /proc/self/fd read of opendir avoided — just try open.
	std::string p = std::string(path) + "/" + needle;
	struct stat st;
	return stat(p.c_str(), &st) == 0;
}
void test_stealth_surface() {
	// Module name in our current build defaults to "my-driver" (KCFG_DRIVER_NAME).
	const char* needle = "my_driver";  // kernel translates '-' to '_' in module names
	const char* alt = "my-driver";
	if (file_grep("/proc/modules", needle) || file_grep("/proc/modules", alt))
		FAIL("S2_proc_modules_hidden", "still visible in /proc/modules");
	else
		PASS("S2_proc_modules_hidden", "not listed");

	if (dir_has("/sys/module", "my_driver") || dir_has("/sys/module", "my-driver"))
		FAIL("S2_sys_module_hidden", "still visible in /sys/module");
	else
		PASS("S2_sys_module_hidden", "not listed");

	// /proc/vmallocinfo — search for the module name in caller column.
	if (file_grep("/proc/vmallocinfo", "my_driver"))
		FAIL("S2_vmallocinfo_hidden", "vmap area still named");
	else
		PASS("S2_vmallocinfo_hidden", "no matching entry");
}

// ---------------------------------------------------------------------------
// S3 — caps ioctl
// ---------------------------------------------------------------------------
void test_caps() {
	auto c = driver.hwbp.caps();
	if (!c) { FAIL("S3_hwbp_caps", "ioctl failed errno=%d", errno); return; }
	if (c->num_brps == 0 || c->num_brps > 16 || c->num_wrps == 0 || c->num_wrps > 16) {
		FAIL("S3_hwbp_caps", "insane brps=%u wrps=%u", c->num_brps, c->num_wrps);
		return;
	}
	if (c->hit_bytes != sizeof(drv_hwbp_hit)) {
		FAIL("S3_hwbp_caps", "hit_bytes=%u expected=%zu", c->hit_bytes, sizeof(drv_hwbp_hit));
		return;
	}
	PASS("S3_hwbp_caps", "brps=%u wrps=%u ring=%u fp_ready=%u",
	     c->num_brps, c->num_wrps, c->ring_slots, c->fp_ready);
}

// ---------------------------------------------------------------------------
// S4 — install matrix (needs a live target; use own pid + a benign address)
// ---------------------------------------------------------------------------
volatile uint32_t g_probe_word __attribute__((used));
__attribute__((noinline)) void probe_fn() { asm volatile("nop"); asm volatile("nop"); }

struct InstallCase { uint32_t type; uint32_t len; bool pass_through; bool expect_ok; const char* label; };

void test_install_matrix() {
	driver.setTarget(getpid());
	uint64_t x_addr = reinterpret_cast<uint64_t>(&probe_fn);
	uint64_t w_addr = reinterpret_cast<uint64_t>(&g_probe_word);

	InstallCase cases[] = {
		{DRV_HWBP_TYPE_X,  DRV_HWBP_LEN_4, false, true,  "X_len4"},
		{DRV_HWBP_TYPE_X,  DRV_HWBP_LEN_8, false, false, "X_len8_should_reject"},
		{DRV_HWBP_TYPE_R,  DRV_HWBP_LEN_4, false, true,  "R_len4"},
		{DRV_HWBP_TYPE_W,  DRV_HWBP_LEN_1, false, true,  "W_len1"},
		{DRV_HWBP_TYPE_RW, DRV_HWBP_LEN_4, false, true,  "RW_len4"},
		{DRV_HWBP_TYPE_R,  DRV_HWBP_LEN_4, true,  false, "R_passthru_should_reject"},
	};
	for (const auto& c : cases) {
		uint64_t addr = (c.type == DRV_HWBP_TYPE_X) ? x_addr : w_addr;
		bool ok = driver.hwbp.install(addr, {}, c.pass_through, c.type, c.len);
		std::string name = std::string("S4_") + c.label;
		if (ok == c.expect_ok) PASS(name.c_str(), "ok=%d expected=%d", ok, c.expect_ok);
		else FAIL(name.c_str(), "ok=%d expected=%d errno=%d", ok, c.expect_ok, errno);
		if (ok) driver.hwbp.remove(addr);
	}
	driver.hwbp.clearAll();
}

// ---------------------------------------------------------------------------
// S5 — bypass_pid one-shot
// ---------------------------------------------------------------------------
void test_bypass_pid() {
	driver.setTarget(getpid());
	uint64_t addr = reinterpret_cast<uint64_t>(&probe_fn);
	if (!driver.hwbp.install(addr, {}, false, DRV_HWBP_TYPE_X, DRV_HWBP_LEN_4)) {
		FAIL("S5_bypass_pid", "install failed errno=%d", errno); return;
	}
	if (!driver.hwbp.setBypassPid(addr, getpid())) {
		FAIL("S5_bypass_pid", "setBypassPid failed errno=%d", errno);
		driver.hwbp.remove(addr); return;
	}
	probe_fn(); // consumes bypass
	probe_fn(); // this one should hit
	auto hits = driver.hwbp.getHits(addr);
	driver.hwbp.remove(addr);
	if (hits.size() == 1) PASS("S5_bypass_pid", "1 hit after bypass consumed (got %zu)", hits.size());
	else FAIL("S5_bypass_pid", "expected 1 hit, got %zu", hits.size());
}

// ---------------------------------------------------------------------------
// S6 — sample gate (every N)
// ---------------------------------------------------------------------------
void test_sample_gate() {
	driver.setTarget(getpid());
	uint64_t addr = reinterpret_cast<uint64_t>(&probe_fn);
	if (!driver.hwbp.install(addr, {}, false, DRV_HWBP_TYPE_X, DRV_HWBP_LEN_4)) {
		FAIL("S6_sample", "install failed"); return;
	}
	if (!driver.hwbp.setSample(addr, 3)) {
		FAIL("S6_sample", "setSample failed"); driver.hwbp.remove(addr); return;
	}
	for (int i = 0; i < 6; ++i) probe_fn();
	auto hits = driver.hwbp.getHits(addr);
	driver.hwbp.remove(addr);
	// sample_every=3 → counter %3 == 0 hits at 3rd and 6th call = 2 hits.
	if (hits.size() == 2) PASS("S6_sample", "6 calls / every=3 → 2 hits");
	else FAIL("S6_sample", "expected 2 hits, got %zu", hits.size());
}

// ---------------------------------------------------------------------------
// S7 — conditional trigger
// ---------------------------------------------------------------------------
__attribute__((noinline)) void probe_fn_cond(uint64_t x0) {
	// Placate the compiler: read x0 so it's live at the breakpoint.
	asm volatile("" : : "r"(x0));
}
void test_conditional() {
	driver.setTarget(getpid());
	uint64_t addr = reinterpret_cast<uint64_t>(&probe_fn_cond);
	if (!driver.hwbp.install(addr, {}, false, DRV_HWBP_TYPE_X, DRV_HWBP_LEN_4)) {
		FAIL("S7_conditional", "install failed"); return;
	}
	// Only trigger when X0 == 42
	if (!driver.hwbp.setCondition(addr, /*reg=*/0, DRV_HWBP_COND_EQ, 42)) {
		FAIL("S7_conditional", "setCondition failed"); driver.hwbp.remove(addr); return;
	}
	probe_fn_cond(1);   // filtered
	probe_fn_cond(99);  // filtered
	probe_fn_cond(42);  // pass
	auto hits = driver.hwbp.getHits(addr);
	driver.hwbp.remove(addr);
	if (hits.size() == 1) PASS("S7_conditional", "1 hit for X0==42 (got %zu)", hits.size());
	else FAIL("S7_conditional", "expected 1 hit, got %zu", hits.size());
}

// ---------------------------------------------------------------------------
// S8 — async SIGRTMIN+1 notify
// ---------------------------------------------------------------------------
std::atomic<int> g_sig_int{0};
void sigrt_handler(int, siginfo_t* si, void*) {
	g_sig_int.store(si->si_int, std::memory_order_release);
}
void test_notify() {
	struct sigaction sa{};
	sa.sa_flags = SA_SIGINFO | SA_RESTART;
	sa.sa_sigaction = sigrt_handler;
	sigaction(SIGRTMIN + 1, &sa, nullptr);

	driver.setTarget(getpid());
	uint64_t addr = reinterpret_cast<uint64_t>(&probe_fn);
	if (!driver.hwbp.install(addr, {}, false, DRV_HWBP_TYPE_X, DRV_HWBP_LEN_4, DRV_HWBP_FLAG_NOTIFY)) {
		FAIL("S8_notify", "install failed"); return;
	}
	if (!driver.hwbp.setNotify(addr, getpid())) {
		FAIL("S8_notify", "setNotify failed"); driver.hwbp.remove(addr); return;
	}
	g_sig_int.store(0);
	probe_fn();
	// give workqueue a moment.
	for (int i = 0; i < 100 && g_sig_int.load() == 0; ++i) usleep(1000);
	int payload = g_sig_int.load();
	driver.hwbp.remove(addr);
	if (payload != 0) PASS("S8_notify", "received signal, si_int=%d", payload);
	else FAIL("S8_notify", "no signal within 100ms");
}

// ---------------------------------------------------------------------------
// S9 — FPSIMD capture
// ---------------------------------------------------------------------------
__attribute__((noinline)) void probe_fn_fp() { asm volatile("nop"); }
void test_fpsimd_capture() {
	auto c = driver.hwbp.caps();
	if (!c || !c->fp_ready) {
		SKIP("S9_fp_capture", "fp_ready=0 (no fpsimd_preserve_current_state)"); return;
	}
	driver.setTarget(getpid());
	uint64_t addr = reinterpret_cast<uint64_t>(&probe_fn_fp);
	if (!driver.hwbp.install(addr, {}, false, DRV_HWBP_TYPE_X, DRV_HWBP_LEN_4, DRV_HWBP_FLAG_CAPTURE_FP)) {
		FAIL("S9_fp_capture", "install failed"); return;
	}
	// Prime Q0 on caller side to distinguish from noise, then call.
	const uint64_t lo = 0xCAFEF00DDEADBEEFULL;
	const uint64_t hi = 0x0123456789ABCDEFULL;
	asm volatile("fmov d0, %0\n\tfmov v0.d[1], %1\n\t" : : "r"(lo), "r"(hi) : "v0");
	probe_fn_fp();
	auto hits = driver.hwbp.getHits(addr);
	driver.hwbp.remove(addr);
	if (hits.empty()) { FAIL("S9_fp_capture", "no hits"); return; }
	if (hits[0].q_lo[0] == lo && hits[0].q_hi[0] == hi)
		PASS("S9_fp_capture", "Q0 captured: lo=%016" PRIx64 " hi=%016" PRIx64, hits[0].q_lo[0], hits[0].q_hi[0]);
	else
		FAIL("S9_fp_capture", "Q0 mismatch lo=%016" PRIx64 " hi=%016" PRIx64,
		     hits[0].q_lo[0], hits[0].q_hi[0]);
}

// ---------------------------------------------------------------------------
// S10 — translate_bait roundtrip (self mapping — should return input unchanged
// when the target address is already in the largest cluster).
// ---------------------------------------------------------------------------
void test_translate_bait() {
	driver.setTarget(getpid());
	uint64_t addr = reinterpret_cast<uint64_t>(&probe_fn);
	auto out = driver.hwbp.translateBait(addr);
	if (!out) { FAIL("S10_translate_bait", "ioctl failed errno=%d", errno); return; }
	// Any value is valid — bait_guard may return same or offset — as long as
	// it didn't fault and produced a non-zero pointer.
	if (*out != 0) PASS("S10_translate_bait", "in=%px out=%px", (void*)addr, (void*)*out);
	else FAIL("S10_translate_bait", "returned 0");
}

// ---------------------------------------------------------------------------
// S11 — timing detector: baseline vs HWBP vs HWBP+TIMING_BYPASS
// ---------------------------------------------------------------------------
uint64_t bench_ns(int reps, void (*fn)()) {
	auto t0 = std::chrono::steady_clock::now();
	for (int i = 0; i < reps; ++i) fn();
	auto t1 = std::chrono::steady_clock::now();
	return std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
}
void test_timing_detector() {
	driver.setTarget(getpid());
	uint64_t addr = reinterpret_cast<uint64_t>(&probe_fn);
	const int REPS = 20000;

	// warm caches
	for (int i = 0; i < 1000; ++i) probe_fn();

	uint64_t base_ns = bench_ns(REPS, probe_fn);
	if (!driver.hwbp.install(addr, {}, false, DRV_HWBP_TYPE_X, DRV_HWBP_LEN_4)) {
		FAIL("S11_timing", "install failed"); return;
	}
	uint64_t hwbp_ns = bench_ns(REPS, probe_fn);
	driver.hwbp.clearAll();

	if (!driver.hwbp.install(addr, {}, false, DRV_HWBP_TYPE_X, DRV_HWBP_LEN_4, DRV_HWBP_FLAG_TIMING_BYPASS)) {
		FAIL("S11_timing", "install(timing_bypass) failed"); return;
	}
	uint64_t tb_ns = bench_ns(REPS, probe_fn);
	driver.hwbp.clearAll();

	double base_per = (double)base_ns / REPS;
	double hwbp_per = (double)hwbp_ns / REPS;
	double tb_per   = (double)tb_ns   / REPS;
	PASS("S11_timing",
	     "per-call ns: base=%.1f hwbp=%.1f timing_bypass=%.1f (ratio %.2fx vs %.2fx)",
	     base_per, hwbp_per, tb_per,
	     hwbp_per / (base_per > 0 ? base_per : 1.0),
	     tb_per   / (base_per > 0 ? base_per : 1.0));
}

// ---------------------------------------------------------------------------
// S12 — file hide (hide_task_name)
// ---------------------------------------------------------------------------
bool ioctl_name(uint32_t cmd, const char* name) {
	drv_ioctl_req req{};
	req.buf = reinterpret_cast<uint64_t>(name);
	req.size = std::strlen(name);
	return driver.rawIoctl(cmd, &req);
}
void test_file_hide() {
	const char* marker = "test_hidden_marker_9f37";
	std::string path = std::string("/data/local/tmp/") + marker;
	FILE* f = std::fopen(path.c_str(), "w");
	if (!f) { SKIP("S12_file_hide", "cannot create %s errno=%d", path.c_str(), errno); return; }
	std::fclose(f);

	// Verify visible without hiding
	bool seen_before = file_grep("/data/local/tmp/", marker) ||
	                   [&]{ struct stat st; return stat(path.c_str(), &st) == 0; }();
	if (!seen_before) SKIP("S12_file_hide", "marker not created?");

	// Ask driver to hide the exact basename.
	if (!ioctl_name(DRV_CMD_HIDE_NAME_ADD, marker)) {
		FAIL("S12_file_hide", "hide_name_add failed errno=%d", errno);
		unlink(path.c_str()); return;
	}
	// Readdir now should NOT see the marker.
	bool seen_after = false;
	{
		FILE* d = popen("ls /data/local/tmp/", "r");
		if (d) {
			char line[512];
			while (std::fgets(line, sizeof(line), d))
				if (std::strstr(line, marker)) { seen_after = true; break; }
			pclose(d);
		}
	}
	ioctl_name(DRV_CMD_HIDE_NAME_REMOVE, marker);
	unlink(path.c_str());
	if (!seen_after) PASS("S12_file_hide", "marker vanished from readdir");
	else FAIL("S12_file_hide", "marker still visible");
}

// ---------------------------------------------------------------------------
// S13 — PID hide
// ---------------------------------------------------------------------------
void test_pid_hide() {
	pid_t child = fork();
	if (child == 0) { pause(); _exit(0); }
	if (child < 0) { FAIL("S13_pid_hide", "fork failed"); return; }

	drv_ioctl_req req{};
	req.pid = child;
	if (!driver.rawIoctl(DRV_CMD_HIDE_PID_ADD, &req)) {
		FAIL("S13_pid_hide", "add failed errno=%d", errno);
		kill(child, SIGKILL); waitpid(child, nullptr, 0); return;
	}
	bool seen = false;
	std::string cmd = "ls /proc/ 2>/dev/null | grep -w " + std::to_string(child);
	FILE* p = popen(cmd.c_str(), "r");
	if (p) {
		char l[64];
		if (std::fgets(l, sizeof(l), p)) seen = true;
		pclose(p);
	}
	drv_ioctl_req rem{}; rem.pid = child;
	driver.rawIoctl(DRV_CMD_HIDE_PID_REMOVE, &rem);
	kill(child, SIGKILL); waitpid(child, nullptr, 0);
	if (!seen) PASS("S13_pid_hide", "pid %d vanished from /proc", child);
	else FAIL("S13_pid_hide", "pid %d still in /proc", child);
}

// ---------------------------------------------------------------------------
// S14 — fd-scoped cleanup (installs on a secondary fd, closes, verifies gone)
// ---------------------------------------------------------------------------
void test_fd_scoped_cleanup() {
	// Open a second Driver handle → open() gets a fresh fd via reboot handshake.
	Driver alt;
	if (!alt.open()) { SKIP("S14_fd_scoped", "cannot open second fd errno=%d", errno); return; }
	alt.setTarget(getpid());
	uint64_t addr = reinterpret_cast<uint64_t>(&probe_fn);
	if (!alt.hwbp.install(addr, {}, false, DRV_HWBP_TYPE_X, DRV_HWBP_LEN_4)) {
		FAIL("S14_fd_scoped", "install on alt fd failed"); return;
	}
	// Close the alt fd — kernel .release should sweep the tracker.
	alt.close();
	usleep(20000);
	// Now try to remove that tracker via the primary fd — should be gone.
	drv_ioctl_req req{}; req.pid = getpid(); req.addr = addr;
	bool still_there = driver.rawIoctl(DRV_CMD_HWBP_REMOVE, &req);
	if (!still_there) PASS("S14_fd_scoped", "tracker gone after alt fd close");
	else FAIL("S14_fd_scoped", "tracker still present — cleanup missed");
}

} // anon

int main() {
	std::printf("== my-driver-test starting (pid=%d) ==\n", getpid());
	if (!test_open()) {
		std::printf("driver.open failed — cannot run remaining tests\n");
		return 2;
	}
	test_stealth_surface();
	test_caps();
	test_install_matrix();
	test_bypass_pid();
	test_sample_gate();
	test_conditional();
	test_notify();
	test_fpsimd_capture();
	test_translate_bait();
	test_timing_detector();
	test_file_hide();
	test_pid_hide();
	test_fd_scoped_cleanup();

	std::printf("\n== summary: %d PASS, %d FAIL, %d SKIP ==\n", g_passes, g_fails, g_skips);
	return g_fails ? 1 : 0;
}
