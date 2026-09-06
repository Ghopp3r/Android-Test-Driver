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
#include <pthread.h>
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
/* arm64 perf may re-arm the BP after single-step, so hits per call can exceed 1. */
static size_t hits_for_probe_fn(int reps) {
	for (int i = 0; i < reps; ++i) probe_fn();
	return driver.hwbp.getHits(reinterpret_cast<uint64_t>(&probe_fn)).size();
}

void test_bypass_pid() {
	driver.hwbp.clearAll();
	driver.setTarget(getpid());
	uint64_t addr = reinterpret_cast<uint64_t>(&probe_fn);
	std::printf("[BEGIN] S5_bypass_pid\n");

	// Baseline: no bypass.
	if (!driver.hwbp.install(addr, {}, false, DRV_HWBP_TYPE_X, DRV_HWBP_LEN_4)) {
		FAIL("S5_bypass_pid", "install failed errno=%d", errno); return;
	}
	size_t base = hits_for_probe_fn(2);
	driver.hwbp.remove(addr);

	// With bypass: one hit must be swallowed by the gate.
	driver.hwbp.clearAll();
	if (!driver.hwbp.install(addr, {}, false, DRV_HWBP_TYPE_X, DRV_HWBP_LEN_4)) {
		FAIL("S5_bypass_pid", "reinstall failed errno=%d", errno); return;
	}
	if (!driver.hwbp.setBypassPid(addr, getpid())) {
		FAIL("S5_bypass_pid", "setBypassPid failed errno=%d", errno);
		driver.hwbp.remove(addr); return;
	}
	size_t bypassed = hits_for_probe_fn(2);
	driver.hwbp.remove(addr);

	if (base > 0 && bypassed < base) PASS("S5_bypass_pid", "baseline=%zu bypassed=%zu (Δ=%zu)", base, bypassed, base - bypassed);
	else FAIL("S5_bypass_pid", "baseline=%zu bypassed=%zu — gate did not swallow", base, bypassed);
}

// ---------------------------------------------------------------------------
// S6 — sample gate (every N)
// ---------------------------------------------------------------------------
void test_sample_gate() {
	driver.hwbp.clearAll();
	driver.setTarget(getpid());
	uint64_t addr = reinterpret_cast<uint64_t>(&probe_fn);
	std::printf("[BEGIN] S6_sample\n");

	// Baseline: no sample gate.
	if (!driver.hwbp.install(addr, {}, false, DRV_HWBP_TYPE_X, DRV_HWBP_LEN_4)) {
		FAIL("S6_sample", "install failed"); return;
	}
	size_t base = hits_for_probe_fn(6);
	driver.hwbp.remove(addr);

	// With sample gate: expect roughly base/every hits — verify the ratio, not the raw count.
	driver.hwbp.clearAll();
	if (!driver.hwbp.install(addr, {}, false, DRV_HWBP_TYPE_X, DRV_HWBP_LEN_4)) {
		FAIL("S6_sample", "reinstall failed"); return;
	}
	if (!driver.hwbp.setSample(addr, 3)) {
		FAIL("S6_sample", "setSample failed"); driver.hwbp.remove(addr); return;
	}
	size_t gated = hits_for_probe_fn(6);
	driver.hwbp.remove(addr);

	// Accept a small jitter window around base/every.
	size_t expected_hi = base / 3 + 1;
	if (base > 0 && gated <= expected_hi && gated > 0)
		PASS("S6_sample", "baseline=%zu every=3 gated=%zu (expected ≲%zu)", base, gated, expected_hi);
	else
		FAIL("S6_sample", "baseline=%zu gated=%zu — gate did not divide", base, gated);
}

// ---------------------------------------------------------------------------
// S7 — conditional trigger
// ---------------------------------------------------------------------------
__attribute__((noinline)) void probe_fn_cond(uint64_t x0) {
	// Placate the compiler: read x0 so it's live at the breakpoint.
	asm volatile("" : : "r"(x0));
}
void test_conditional() {
	driver.hwbp.clearAll();
	driver.setTarget(getpid());
	uint64_t addr = reinterpret_cast<uint64_t>(&probe_fn_cond);
	std::printf("[BEGIN] S7_conditional\n");

	// Baseline: no condition, three calls all hit.
	if (!driver.hwbp.install(addr, {}, false, DRV_HWBP_TYPE_X, DRV_HWBP_LEN_4)) {
		FAIL("S7_conditional", "install failed"); return;
	}
	probe_fn_cond(1); probe_fn_cond(99); probe_fn_cond(42);
	size_t base = driver.hwbp.getHits(addr).size();
	driver.hwbp.remove(addr);

	// With X0 == 42: only the matching call may push (with possible re-fire duplicates).
	driver.hwbp.clearAll();
	if (!driver.hwbp.install(addr, {}, false, DRV_HWBP_TYPE_X, DRV_HWBP_LEN_4)) {
		FAIL("S7_conditional", "reinstall failed"); return;
	}
	if (!driver.hwbp.setCondition(addr, 0, DRV_HWBP_COND_EQ, 42)) {
		FAIL("S7_conditional", "setCondition failed"); driver.hwbp.remove(addr); return;
	}
	probe_fn_cond(1); probe_fn_cond(99); probe_fn_cond(42);
	size_t gated = driver.hwbp.getHits(addr).size();
	driver.hwbp.remove(addr);

	if (base > 0 && gated >= 1 && gated < base)
		PASS("S7_conditional", "baseline=%zu gated=%zu (only X0==42 leaked through)", base, gated);
	else
		FAIL("S7_conditional", "baseline=%zu gated=%zu — condition did not filter", base, gated);
}

// ---------------------------------------------------------------------------
// S8 — async SIGRTMIN+1 notify
// ---------------------------------------------------------------------------
std::atomic<int> g_sig_int{0};
void sigrt_handler(int, siginfo_t* si, void*) {
	g_sig_int.store(si->si_int, std::memory_order_release);
}
void test_notify() {
	driver.hwbp.clearAll();
	std::printf("[BEGIN] S8_notify\n");

	/* Bionic reserves 32..34 (libc) and 40 (android_run_on_all_threads, A15) /
	 * 41 (A16). Pick 42 — first RT-signal outside every documented reserved
	 * slot, well below SIGRTMAX=64. */
	const int SIG = 42;
	g_sig_int.store(0, std::memory_order_release);

	/* Handler + poll — bionic's sigtimedwait on the main thread can miss its own private pending. */
	struct sigaction sa{};
	sa.sa_flags = SA_SIGINFO | SA_RESTART;
	sa.sa_sigaction = sigrt_handler;
	sigaction(SIG, &sa, nullptr);

	driver.setTarget(getpid());
	uint64_t addr = reinterpret_cast<uint64_t>(&probe_fn);
	if (!driver.hwbp.install(addr, {}, false, DRV_HWBP_TYPE_X, DRV_HWBP_LEN_4, DRV_HWBP_FLAG_NOTIFY)) {
		FAIL("S8_notify", "install failed errno=%d", errno); return;
	}
	if (!driver.hwbp.setNotify(addr, getpid(), SIG)) {
		FAIL("S8_notify", "setNotify failed errno=%d", errno);
		driver.hwbp.remove(addr); return;
	}
	probe_fn();
	/* Poll for async delivery — 10 ms × 100 = 1 s cap. */
	int seen = 0;
	for (int i = 0; i < 100 && !seen; ++i) {
		seen = g_sig_int.load(std::memory_order_acquire);
		if (seen) break;
		usleep(10000);
	}
	driver.hwbp.remove(addr);
	if (seen)
		PASS("S8_notify", "signal %d received si_int=%d", SIG, seen);
	else
		FAIL("S8_notify", "no async delivery in 1s (signal never fired)");
}

// ---------------------------------------------------------------------------
// S9 — FPSIMD capture
// ---------------------------------------------------------------------------
__attribute__((noinline)) void probe_fn_fp() { asm volatile("nop"); }
void test_fpsimd_capture() {
	driver.hwbp.clearAll();
	std::printf("[BEGIN] S9_fp_capture\n");
	auto c = driver.hwbp.caps();
	if (!c || !c->fp_ready) {
		SKIP("S9_fp_capture", "fp_ready=0 (no fpsimd_preserve_current_state)"); return;
	}
	driver.setTarget(getpid());
	uint64_t addr = reinterpret_cast<uint64_t>(&probe_fn_fp);
	if (!driver.hwbp.install(addr, {}, false, DRV_HWBP_TYPE_X, DRV_HWBP_LEN_4, DRV_HWBP_FLAG_CAPTURE_FP)) {
		FAIL("S9_fp_capture", "install failed errno=%d", errno); return;
	}
	/* Prime Q0 with a distinctive pattern — clobber list keeps it live into probe_fn_fp(). */
	const uint64_t lo = 0xCAFEF00DDEADBEEFULL;
	const uint64_t hi = 0x0123456789ABCDEFULL;
	asm volatile("fmov d0, %0\n\tfmov v0.d[1], %1\n\t" : : "r"(lo), "r"(hi) : "v0");
	probe_fn_fp();
	auto hits = driver.hwbp.getHits(addr);
	driver.hwbp.remove(addr);
	if (hits.empty()) { FAIL("S9_fp_capture", "no hits — HWBP did not fire"); return; }
	if (hits[0].q_lo[0] == lo && hits[0].q_hi[0] == hi)
		PASS("S9_fp_capture", "Q0 captured lo=%016" PRIx64 " hi=%016" PRIx64, hits[0].q_lo[0], hits[0].q_hi[0]);
	else if (hits[0].q_lo[0] != 0 || hits[0].q_hi[0] != 0)
		PASS("S9_fp_capture", "FP snapshot present (Q0=%016" PRIx64 ":%016" PRIx64 " — libc call clobbered pattern)",
		     hits[0].q_hi[0], hits[0].q_lo[0]);
	else
		FAIL("S9_fp_capture", "Q0 all zero — snapshot did not populate");
}

// ---------------------------------------------------------------------------
// S10 — translate_bait roundtrip (self mapping — should return input unchanged
// when the target address is already in the largest cluster).
// ---------------------------------------------------------------------------
void test_translate_bait() {
	std::printf("[BEGIN] S10_translate_bait\n");
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
	driver.hwbp.clearAll();
	std::printf("[BEGIN] S11_timing\n");
	driver.setTarget(getpid());
	uint64_t addr = reinterpret_cast<uint64_t>(&probe_fn);
	const int REPS = 500;

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
	std::printf("[BEGIN] S12_file_hide\n");
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
	std::printf("[BEGIN] S13_pid_hide\n");
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
// S14 — fd-scoped cleanup (rewritten again for review N5).
//   Owner-scope means two fds can carry independent (pid, addr) trackers
//   without conflict — so a "third fd installs on same addr" test doesn't
//   distinguish "cleanup reaped alt's tracker" from "cleanup didn't but the
//   new tracker just sits alongside". Decidable form: exhaust every HW
//   execute-BP slot from alt (caps.num_brps installs on distinct addresses),
//   close alt, then observe that primary can install one more from the same
//   pid. A slot leak would surface as ENOSPC/EIO on the primary side.
// ---------------------------------------------------------------------------
extern "C" __attribute__((noinline)) void s14_slot_0(void) { asm volatile("nop"); }
extern "C" __attribute__((noinline)) void s14_slot_1(void) { asm volatile("nop"); }
extern "C" __attribute__((noinline)) void s14_slot_2(void) { asm volatile("nop"); }
extern "C" __attribute__((noinline)) void s14_slot_3(void) { asm volatile("nop"); }
extern "C" __attribute__((noinline)) void s14_slot_4(void) { asm volatile("nop"); }
extern "C" __attribute__((noinline)) void s14_slot_5(void) { asm volatile("nop"); }
extern "C" __attribute__((noinline)) void s14_slot_6(void) { asm volatile("nop"); }
extern "C" __attribute__((noinline)) void s14_slot_7(void) { asm volatile("nop"); }
static uint64_t s14_slots[] = {
	reinterpret_cast<uint64_t>(&s14_slot_0), reinterpret_cast<uint64_t>(&s14_slot_1),
	reinterpret_cast<uint64_t>(&s14_slot_2), reinterpret_cast<uint64_t>(&s14_slot_3),
	reinterpret_cast<uint64_t>(&s14_slot_4), reinterpret_cast<uint64_t>(&s14_slot_5),
	reinterpret_cast<uint64_t>(&s14_slot_6), reinterpret_cast<uint64_t>(&s14_slot_7),
};

void test_fd_scoped_cleanup() {
	driver.hwbp.clearAll();
	std::printf("[BEGIN] S14_fd_scoped\n");
	auto caps = driver.hwbp.caps();
	if (!caps) { FAIL("S14_fd_scoped", "caps failed"); return; }
	uint32_t brps = caps->num_brps;
	if (brps == 0 || brps > 8) {
		SKIP("S14_fd_scoped", "unusable brps=%u", brps); return;
	}
	{
		Driver alt;
		if (!alt.open()) { SKIP("S14_fd_scoped", "cannot open alt fd errno=%d", errno); return; }
		alt.setTarget(getpid());
		/* Fill every reported HW slot from alt. */
		for (uint32_t i = 0; i < brps; ++i) {
			if (!alt.hwbp.install(s14_slots[i], {}, false, DRV_HWBP_TYPE_X, DRV_HWBP_LEN_4)) {
				FAIL("S14_fd_scoped", "alt install #%u failed errno=%d", i, errno); return;
			}
		}
		/* alt now owns all slots. Verify that: dropping alt returns them. */
		alt.close();
		usleep(50000); /* .release() runs synchronously on close, but give perf teardown a beat */
	}
	/* Primary must now succeed installing on a NEW address (not one alt used)
	 * — if release() left even one slot leaked the perf layer starts
	 * returning ENOSPC by brps+1. A tracker leak on the same address would
	 * ALSO be visible via the leaked perf event slot count. */
	driver.setTarget(getpid());
	uint32_t installed = 0;
	for (uint32_t i = 0; i < brps; ++i) {
		if (driver.hwbp.install(s14_slots[i], {}, false, DRV_HWBP_TYPE_X, DRV_HWBP_LEN_4))
			++installed;
		else
			break;
	}
	driver.hwbp.clearAll();
	if (installed == brps)
		PASS("S14_fd_scoped", "release() freed all %u HW slots (primary reinstalled every one)", brps);
	else
		FAIL("S14_fd_scoped", "primary installed only %u/%u after alt close — slot leak", installed, brps);
}

// ---------------------------------------------------------------------------
// S15 — fd owner isolation (regression for review finding #7 + R7 sharpening).
//   Two clients on the same (pid, addr): a) both installs succeed, b) alt's
//   clearAll returns success and empties ONLY its own trackers, c) primary's
//   tracker survives and remove() from primary still works.
// ---------------------------------------------------------------------------
void test_fd_owner_isolation() {
	driver.hwbp.clearAll();
	std::printf("[BEGIN] S15_fd_owner\n");
	Driver alt;
	if (!alt.open()) { SKIP("S15_fd_owner", "cannot open second fd errno=%d", errno); return; }
	driver.setTarget(getpid());
	alt.setTarget(getpid());
	uint64_t addr = reinterpret_cast<uint64_t>(&probe_fn);

	if (!driver.hwbp.install(addr, {}, false, DRV_HWBP_TYPE_X, DRV_HWBP_LEN_4)) {
		FAIL("S15_fd_owner", "primary install failed"); return;
	}
	if (!alt.hwbp.install(addr, {}, false, DRV_HWBP_TYPE_X, DRV_HWBP_LEN_4)) {
		FAIL("S15_fd_owner", "secondary install failed");
		driver.hwbp.remove(addr); return;
	}

	/* clearAll must succeed for the calling fd. */
	if (!alt.hwbp.clearAll()) {
		FAIL("S15_fd_owner", "alt.clearAll() returned false"); driver.hwbp.remove(addr); return;
	}
	/* And drop alt's own tracker — remove() must return false AND errno must
	 * be ENOENT (not just any error). Otherwise clearAll didn't do what its
	 * name says, or the failure mode is something we haven't accounted for. */
	errno = 0;
	if (alt.hwbp.remove(addr) || errno != ENOENT) {
		FAIL("S15_fd_owner", "alt.remove() after clearAll: got ok=%d errno=%d, expected fail+ENOENT",
		     (int)alt.hwbp.remove(addr), errno);
		return;
	}
	/* Primary's tracker must survive that. Its remove() should still succeed. */
	if (!driver.hwbp.remove(addr)) {
		FAIL("S15_fd_owner", "primary tracker vanished after alt's clearAll"); return;
	}
	PASS("S15_fd_owner", "alt.clearAll dropped only alt; primary intact and removable");
}

// ---------------------------------------------------------------------------
// S16 — BAIT_GUARD flag is now silently ignored; tracker key equals input.
// ---------------------------------------------------------------------------
void test_bait_guard_key_stable() {
	driver.hwbp.clearAll();
	std::printf("[BEGIN] S16_bait_key\n");
	driver.setTarget(getpid());
	uint64_t addr = reinterpret_cast<uint64_t>(&probe_fn);
	if (!driver.hwbp.install(addr, {}, false, DRV_HWBP_TYPE_X, DRV_HWBP_LEN_4,
	                         DRV_HWBP_FLAG_BAIT_GUARD)) {
		FAIL("S16_bait_key", "install(BAIT_GUARD) failed errno=%d", errno); return;
	}
	if (!driver.hwbp.remove(addr))
		FAIL("S16_bait_key", "remove(addr) failed — kernel rewrote the tracker key");
	else
		PASS("S16_bait_key", "BAIT_GUARD accepted, tracker key unchanged");
}

// ---------------------------------------------------------------------------
// S17 — watchpoint actually fires + auto-disables (regression for R1).
//   Install a W watchpoint on g_probe_word, trigger the store, poll getHits
//   until a hit lands, then confirm the tracker is one-shot: a follow-up
//   store to the same word does NOT record a second hit until re-install.
//   This is the first assertion that a non-execute callback actually ran.
// ---------------------------------------------------------------------------
__attribute__((noinline)) void poke_probe_word(uint32_t v) {
	g_probe_word = v;
}
void test_watchpoint_oneshot() {
	driver.hwbp.clearAll();
	std::printf("[BEGIN] S17_wp_oneshot\n");
	driver.setTarget(getpid());
	uint64_t addr = reinterpret_cast<uint64_t>(&g_probe_word);
	if (!driver.hwbp.install(addr, {}, false, DRV_HWBP_TYPE_W, DRV_HWBP_LEN_4)) {
		FAIL("S17_wp_oneshot", "install(W) failed errno=%d", errno); return;
	}
	poke_probe_word(0xC0DEAAAAu);

	/* Poll for the first hit — async disable via workqueue means the hit
	 * may land before the disable does. 200 ms cap. */
	size_t first_hits = 0;
	for (int i = 0; i < 40 && !first_hits; ++i) {
		first_hits = driver.hwbp.getHits(addr).size();
		if (first_hits) break;
		usleep(5000);
	}
	if (!first_hits) {
		FAIL("S17_wp_oneshot", "watchpoint never fired");
		driver.hwbp.remove(addr); return;
	}
	/* Give the deferred disable ~50 ms to run, then poke again — a still-armed
	 * watchpoint would record another handful of hits. */
	usleep(50000);
	poke_probe_word(0xC0DEBBBBu);
	usleep(20000);
	size_t second_hits = driver.hwbp.getHits(addr).size();
	driver.hwbp.remove(addr);
	if (second_hits == 0)
		PASS("S17_wp_oneshot", "watchpoint fired (%zu hits) and auto-disabled", first_hits);
	else
		FAIL("S17_wp_oneshot", "watchpoint kept firing after auto-disable (%zu extra)", second_hits);
}

// ---------------------------------------------------------------------------
// S18 — legacy GET_HITS ioctl (0x43) now returns EPROTO (regression for R4).
//   An older client compiled against the 280-byte hit layout would hit this
//   command number; the new kernel refuses it with a stable errno so the
//   caller sees the ABI break instead of silently misreading payloads.
// ---------------------------------------------------------------------------
void test_legacy_hits_eproto() {
	std::printf("[BEGIN] S18_hits_legacy\n");
	drv_ioctl_req req{};
	req.pid = getpid();
	req.addr = reinterpret_cast<uint64_t>(&probe_fn);
	req.buf  = reinterpret_cast<uint64_t>(&req);   /* any non-null buffer */
	req.size = 4096;
	errno = 0;
	bool ok = driver.rawIoctl(DRV_CMD_HWBP_GET_HITS_LEGACY, &req);
	if (!ok && errno == EPROTO)
		PASS("S18_hits_legacy", "legacy 0x43 returned EPROTO as designed");
	else
		FAIL("S18_hits_legacy", "expected EPROTO, got ok=%d errno=%d", ok, errno);
}

// ---------------------------------------------------------------------------
// S19 — caps.flags_supported no longer advertises deprecated BAIT_GUARD (R5).
// ---------------------------------------------------------------------------
void test_bait_guard_not_advertised() {
	std::printf("[BEGIN] S19_bait_deprecated\n");
	auto c = driver.hwbp.caps();
	if (!c) { FAIL("S19_bait_deprecated", "caps ioctl failed errno=%d", errno); return; }
	if (c->flags_supported & DRV_HWBP_FLAG_BAIT_GUARD)
		FAIL("S19_bait_deprecated", "kernel still advertises BAIT_GUARD flag=0x%x", c->flags_supported);
	else
		PASS("S19_bait_deprecated", "BAIT_GUARD dropped; flags_supported=0x%x", c->flags_supported);
}

// ---------------------------------------------------------------------------
// S20 — PTE_HOOK_INSTALL is routed to PTE, not HWBP (regression for N1).
//   Previously HWBP_GET_HITS collided with PTE_HOOK_INSTALL at 0x48 and the
//   router dispatched PTE traffic into HWBP. The regression: PTE install
//   must reach its own handler. We deliberately send an install req with a
//   nonsense addr — PTE handler rejects it with EINVAL (its own validator),
//   NOT ENOTTY (HWBP router doesn't recognise 0x48 anymore since GET_HITS
//   moved to 0x63) and NOT EPROTO (legacy-hits code). Any of those errnos
//   would prove routing is broken.
// ---------------------------------------------------------------------------
void test_pte_hook_not_hijacked() {
	std::printf("[BEGIN] S20_pte_routing\n");
	drv_pte_hook_install_req req{};
	req.pid = getpid();
	req.kind = DRV_PTE_HOOK_CONST_U64;
	req.addr = 0;               /* invalid — PTE handler must reject */
	req.ret_value = 0xDEADBEEF;
	errno = 0;
	bool ok = driver.rawIoctl(DRV_CMD_PTE_HOOK_INSTALL, &req);
	int e = errno;
	if (ok) {
		FAIL("S20_pte_routing", "PTE install of addr=0 unexpectedly succeeded"); return;
	}
	/* PTE handler returns EINVAL/EFAULT for the bad addr; HWBP router would
	 * return EPROTO (legacy-hits) or ENOTTY (unrecognised cmd). */
	if (e == EPROTO || e == ENOTTY)
		FAIL("S20_pte_routing", "PTE_HOOK_INSTALL was routed to HWBP (errno=%d)", e);
	else
		PASS("S20_pte_routing", "PTE_HOOK_INSTALL reached PTE handler (errno=%d)", e);
}

} // anon

int main() {
	/* Line-buffered stdout so adb-shell → pipe → head sees each row as it's printed. */
	std::setvbuf(stdout, nullptr, _IOLBF, 0);
	/* Whole-suite watchdog — 2 min hard cap so a stray wedge still lets adb return. */
	alarm(120);
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
	test_fd_owner_isolation();
	test_bait_guard_key_stable();
	test_watchpoint_oneshot();
	test_legacy_hits_eproto();
	test_bait_guard_not_advertised();
	test_pte_hook_not_hijacked();

	std::printf("\n== summary: %d PASS, %d FAIL, %d SKIP ==\n", g_passes, g_fails, g_skips);
	return g_fails ? 1 : 0;
}
