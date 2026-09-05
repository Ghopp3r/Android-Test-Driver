// SPDX-License-Identifier: GPL-2.0
//
// JNI bridge for MyDriverTest.apk. Each Java_com_mydriver_test_NativeBridge_run*
// entry point mirrors one S1..S14 scenario from client/tests/test_all.cpp and
// returns a "TAG|detail" jstring (TAG in {PASS, FAIL, SKIP}).
//
// Driver.cpp is compiled into this same .so, so we share its global `driver`.

#include <jni.h>
#include <android/log.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "Driver.h"
#include "driver/uapi.h"

#define LOG_TAG "MyDriverTest"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

// ----------------------------------------------------------------------------
// Small helpers
// ----------------------------------------------------------------------------
std::string mk(const char* tag, const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::string out = tag;
    out += '|';
    out += buf;
    return out;
}
#define PASS(...) mk("PASS", __VA_ARGS__)
#define FAIL(...) mk("FAIL", __VA_ARGS__)
#define SKIP(...) mk("SKIP", __VA_ARGS__)

jstring toJ(JNIEnv* env, const std::string& s) {
    return env->NewStringUTF(s.c_str());
}

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
    std::string p = std::string(path) + "/" + needle;
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

// ----------------------------------------------------------------------------
// Probe symbols used by S4..S11. These live in the .so's .text so their address
// is stable for the life of the process.
// ----------------------------------------------------------------------------
volatile uint32_t g_probe_word __attribute__((used));

__attribute__((noinline)) void probe_fn() {
    asm volatile("nop");
    asm volatile("nop");
}

__attribute__((noinline)) void probe_fn_cond(uint64_t x0) {
    asm volatile("" : : "r"(x0));
}

__attribute__((noinline)) void probe_fn_fp() {
    asm volatile("nop");
}

size_t hits_for_probe_fn(int reps) {
    for (int i = 0; i < reps; ++i) probe_fn();
    return driver.hwbp.getHits(reinterpret_cast<uint64_t>(&probe_fn)).size();
}

uint64_t bench_ns(int reps, void (*fn)()) {
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < reps; ++i) fn();
    auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
}

// ----------------------------------------------------------------------------
// S8: async SIGRTMIN+1 notify handler.
// ----------------------------------------------------------------------------
std::atomic<int> g_sig_int{0};
void sigrt_handler(int, siginfo_t* si, void*) {
    g_sig_int.store(si->si_int, std::memory_order_release);
}

// ----------------------------------------------------------------------------
// S12: hide_name_add/remove using raw ioctl req.
// ----------------------------------------------------------------------------
bool ioctl_name(uint32_t cmd, const char* name) {
    drv_ioctl_req req{};
    req.buf = reinterpret_cast<uint64_t>(name);
    req.size = std::strlen(name);
    return driver.rawIoctl(cmd, &req);
}

} // anon

// ============================================================================
// JNI entry points
// ============================================================================
extern "C" {

JNIEXPORT jboolean JNICALL
Java_com_mydriver_test_NativeBridge_openDriver(JNIEnv*, jobject) {
    bool ok = driver.open();
    if (!ok) LOGE("driver.open failed errno=%d (%s)", errno, std::strerror(errno));
    return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_mydriver_test_NativeBridge_driverIsOpen(JNIEnv*, jobject) {
    return driver.isOpen() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jstring JNICALL
Java_com_mydriver_test_NativeBridge_getCapsString(JNIEnv* env, jobject) {
    auto c = driver.hwbp.caps();
    if (!c) return toJ(env, "caps: <ioctl failed>");
    char buf[192];
    std::snprintf(buf, sizeof(buf),
        "brps=%u wrps=%u ring=%u max_ov=%u hit_bytes=%u install_req=%u flags=0x%08x fp_ready=%u",
        c->num_brps, c->num_wrps, c->ring_slots, c->max_overrides,
        c->hit_bytes, c->install_req_bytes, c->flags_supported, c->fp_ready);
    return toJ(env, buf);
}

// ----- S2 stealth --------------------------------------------------------
JNIEXPORT jstring JNICALL
Java_com_mydriver_test_NativeBridge_runStealthSurface(JNIEnv* env, jobject) {
    // Aggregate all three sub-checks. Any FAIL taints the whole scenario;
    // otherwise emit a compact per-surface breakdown.
    bool procMod = file_grep("/proc/modules", "my_driver") || file_grep("/proc/modules", "my-driver");
    bool sysMod  = dir_has("/sys/module", "my_driver") || dir_has("/sys/module", "my-driver");
    bool vmalloc = file_grep("/proc/vmallocinfo", "my_driver");

    if (procMod || sysMod || vmalloc) {
        return toJ(env, FAIL("proc=%d sysfs=%d vmalloc=%d — module still visible",
                             (int)procMod, (int)sysMod, (int)vmalloc));
    }
    return toJ(env, PASS("hidden from /proc/modules, /sys/module, /proc/vmallocinfo"));
}

// ----- S3 caps ioctl -----------------------------------------------------
JNIEXPORT jstring JNICALL
Java_com_mydriver_test_NativeBridge_runCaps(JNIEnv* env, jobject) {
    auto c = driver.hwbp.caps();
    if (!c) return toJ(env, FAIL("ioctl failed errno=%d (%s)", errno, std::strerror(errno)));
    if (c->num_brps == 0 || c->num_brps > 16 || c->num_wrps == 0 || c->num_wrps > 16)
        return toJ(env, FAIL("insane brps=%u wrps=%u", c->num_brps, c->num_wrps));
    if (c->hit_bytes != sizeof(drv_hwbp_hit))
        return toJ(env, FAIL("hit_bytes=%u expected=%zu", c->hit_bytes, sizeof(drv_hwbp_hit)));
    return toJ(env, PASS("brps=%u wrps=%u ring=%u fp_ready=%u",
                         c->num_brps, c->num_wrps, c->ring_slots, c->fp_ready));
}

// ----- S4 install matrix -------------------------------------------------
JNIEXPORT jstring JNICALL
Java_com_mydriver_test_NativeBridge_runInstallMatrix(JNIEnv* env, jobject) {
    driver.setTarget(getpid());
    uint64_t x_addr = reinterpret_cast<uint64_t>(&probe_fn);
    uint64_t w_addr = reinterpret_cast<uint64_t>(&g_probe_word);

    struct C { uint32_t type; uint32_t len; bool pt; bool ok; const char* label; };
    C cases[] = {
        {DRV_HWBP_TYPE_X,  DRV_HWBP_LEN_4, false, true,  "X4"},
        {DRV_HWBP_TYPE_X,  DRV_HWBP_LEN_8, false, false, "X8-rej"},
        {DRV_HWBP_TYPE_R,  DRV_HWBP_LEN_4, false, true,  "R4"},
        {DRV_HWBP_TYPE_W,  DRV_HWBP_LEN_1, false, true,  "W1"},
        {DRV_HWBP_TYPE_RW, DRV_HWBP_LEN_4, false, true,  "RW4"},
        {DRV_HWBP_TYPE_R,  DRV_HWBP_LEN_4, true,  false, "R-pt-rej"},
    };
    std::string bad;
    int pass = 0, total = 0;
    for (auto& c : cases) {
        uint64_t a = (c.type == DRV_HWBP_TYPE_X) ? x_addr : w_addr;
        bool got = driver.hwbp.install(a, {}, c.pt, c.type, c.len);
        ++total;
        if (got == c.ok) ++pass;
        else {
            if (!bad.empty()) bad += ",";
            bad += c.label;
            bad += "(got=";
            bad += got ? "ok" : "err";
            bad += ")";
        }
        if (got) driver.hwbp.remove(a);
    }
    driver.hwbp.clearAll();
    if (pass == total) return toJ(env, PASS("%d/%d cases behave as expected", pass, total));
    return toJ(env, FAIL("%d/%d ok, mismatches: %s", pass, total, bad.c_str()));
}

// ----- S5 bypass_pid -----------------------------------------------------
JNIEXPORT jstring JNICALL
Java_com_mydriver_test_NativeBridge_runBypassPid(JNIEnv* env, jobject) {
    driver.hwbp.clearAll();
    driver.setTarget(getpid());
    uint64_t addr = reinterpret_cast<uint64_t>(&probe_fn);

    if (!driver.hwbp.install(addr, {}, false, DRV_HWBP_TYPE_X, DRV_HWBP_LEN_4))
        return toJ(env, FAIL("baseline install failed errno=%d", errno));
    size_t base = hits_for_probe_fn(2);
    driver.hwbp.remove(addr);
    driver.hwbp.clearAll();

    if (!driver.hwbp.install(addr, {}, false, DRV_HWBP_TYPE_X, DRV_HWBP_LEN_4))
        return toJ(env, FAIL("reinstall failed errno=%d", errno));
    if (!driver.hwbp.setBypassPid(addr, getpid())) {
        driver.hwbp.remove(addr);
        return toJ(env, FAIL("setBypassPid failed errno=%d", errno));
    }
    size_t bypassed = hits_for_probe_fn(2);
    driver.hwbp.remove(addr);
    driver.hwbp.clearAll();

    if (base > 0 && bypassed < base)
        return toJ(env, PASS("baseline=%zu bypassed=%zu (delta=%zu)", base, bypassed, base - bypassed));
    return toJ(env, FAIL("baseline=%zu bypassed=%zu — gate did not swallow", base, bypassed));
}

// ----- S6 sample gate ----------------------------------------------------
JNIEXPORT jstring JNICALL
Java_com_mydriver_test_NativeBridge_runSampleGate(JNIEnv* env, jobject) {
    driver.hwbp.clearAll();
    driver.setTarget(getpid());
    uint64_t addr = reinterpret_cast<uint64_t>(&probe_fn);

    if (!driver.hwbp.install(addr, {}, false, DRV_HWBP_TYPE_X, DRV_HWBP_LEN_4))
        return toJ(env, FAIL("baseline install failed"));
    size_t base = hits_for_probe_fn(6);
    driver.hwbp.remove(addr);
    driver.hwbp.clearAll();

    if (!driver.hwbp.install(addr, {}, false, DRV_HWBP_TYPE_X, DRV_HWBP_LEN_4))
        return toJ(env, FAIL("reinstall failed"));
    if (!driver.hwbp.setSample(addr, 3)) {
        driver.hwbp.remove(addr);
        return toJ(env, FAIL("setSample failed"));
    }
    size_t gated = hits_for_probe_fn(6);
    driver.hwbp.remove(addr);
    driver.hwbp.clearAll();

    size_t hi = base / 3 + 1;
    if (base > 0 && gated > 0 && gated <= hi)
        return toJ(env, PASS("baseline=%zu every=3 gated=%zu (~<= %zu)", base, gated, hi));
    return toJ(env, FAIL("baseline=%zu gated=%zu — gate did not divide", base, gated));
}

// ----- S7 conditional ----------------------------------------------------
JNIEXPORT jstring JNICALL
Java_com_mydriver_test_NativeBridge_runConditional(JNIEnv* env, jobject) {
    driver.hwbp.clearAll();
    driver.setTarget(getpid());
    uint64_t addr = reinterpret_cast<uint64_t>(&probe_fn_cond);

    if (!driver.hwbp.install(addr, {}, false, DRV_HWBP_TYPE_X, DRV_HWBP_LEN_4))
        return toJ(env, FAIL("baseline install failed"));
    probe_fn_cond(1); probe_fn_cond(99); probe_fn_cond(42);
    size_t base = driver.hwbp.getHits(addr).size();
    driver.hwbp.remove(addr);
    driver.hwbp.clearAll();

    if (!driver.hwbp.install(addr, {}, false, DRV_HWBP_TYPE_X, DRV_HWBP_LEN_4))
        return toJ(env, FAIL("reinstall failed"));
    if (!driver.hwbp.setCondition(addr, 0, DRV_HWBP_COND_EQ, 42)) {
        driver.hwbp.remove(addr);
        return toJ(env, FAIL("setCondition failed"));
    }
    probe_fn_cond(1); probe_fn_cond(99); probe_fn_cond(42);
    size_t gated = driver.hwbp.getHits(addr).size();
    driver.hwbp.remove(addr);
    driver.hwbp.clearAll();

    if (base > 0 && gated >= 1 && gated < base)
        return toJ(env, PASS("baseline=%zu gated=%zu (only X0==42 leaked)", base, gated));
    return toJ(env, FAIL("baseline=%zu gated=%zu — condition did not filter", base, gated));
}

// ----- S8 notify ---------------------------------------------------------
JNIEXPORT jstring JNICALL
Java_com_mydriver_test_NativeBridge_runNotify(JNIEnv* env, jobject) {
    driver.hwbp.clearAll();
    struct sigaction sa{};
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sa.sa_sigaction = sigrt_handler;
    sigaction(SIGRTMIN + 1, &sa, nullptr);

    driver.setTarget(getpid());
    uint64_t addr = reinterpret_cast<uint64_t>(&probe_fn);
    if (!driver.hwbp.install(addr, {}, false, DRV_HWBP_TYPE_X, DRV_HWBP_LEN_4, DRV_HWBP_FLAG_NOTIFY))
        return toJ(env, FAIL("install failed"));
    if (!driver.hwbp.setNotify(addr, getpid())) {
        driver.hwbp.remove(addr);
        return toJ(env, FAIL("setNotify failed"));
    }
    g_sig_int.store(0);
    probe_fn();
    for (int i = 0; i < 100 && g_sig_int.load() == 0; ++i) usleep(1000);
    int payload = g_sig_int.load();
    driver.hwbp.remove(addr);
    driver.hwbp.clearAll();

    if (payload != 0) return toJ(env, PASS("received signal, si_int=%d", payload));
    return toJ(env, FAIL("no signal within 100ms"));
}

// ----- S9 FPSIMD capture -------------------------------------------------
JNIEXPORT jstring JNICALL
Java_com_mydriver_test_NativeBridge_runFpsimdCapture(JNIEnv* env, jobject) {
    driver.hwbp.clearAll();
    auto c = driver.hwbp.caps();
    if (!c || !c->fp_ready)
        return toJ(env, SKIP("fp_ready=0 (fpsimd_preserve_current_state unresolved)"));

    driver.setTarget(getpid());
    uint64_t addr = reinterpret_cast<uint64_t>(&probe_fn_fp);
    if (!driver.hwbp.install(addr, {}, false, DRV_HWBP_TYPE_X, DRV_HWBP_LEN_4, DRV_HWBP_FLAG_CAPTURE_FP))
        return toJ(env, FAIL("install failed"));

    const uint64_t lo = 0xCAFEF00DDEADBEEFULL;
    const uint64_t hi = 0x0123456789ABCDEFULL;
    asm volatile("fmov d0, %0\n\tfmov v0.d[1], %1\n\t" : : "r"(lo), "r"(hi) : "v0");
    probe_fn_fp();
    auto hits = driver.hwbp.getHits(addr);
    driver.hwbp.remove(addr);
    driver.hwbp.clearAll();
    if (hits.empty()) return toJ(env, FAIL("no hits"));
    if (hits[0].q_lo[0] == lo && hits[0].q_hi[0] == hi)
        return toJ(env, PASS("Q0 captured lo=%016llx hi=%016llx",
                             (unsigned long long)hits[0].q_lo[0],
                             (unsigned long long)hits[0].q_hi[0]));
    return toJ(env, FAIL("Q0 mismatch lo=%016llx hi=%016llx",
                         (unsigned long long)hits[0].q_lo[0],
                         (unsigned long long)hits[0].q_hi[0]));
}

// ----- S10 translate_bait ------------------------------------------------
JNIEXPORT jstring JNICALL
Java_com_mydriver_test_NativeBridge_runTranslateBait(JNIEnv* env, jobject) {
    driver.setTarget(getpid());
    uint64_t addr = reinterpret_cast<uint64_t>(&probe_fn);
    auto out = driver.hwbp.translateBait(addr);
    if (!out) return toJ(env, FAIL("ioctl failed errno=%d (%s)", errno, std::strerror(errno)));
    if (*out == 0) return toJ(env, FAIL("returned 0"));
    return toJ(env, PASS("in=%p out=%p", (void*)addr, (void*)*out));
}

// ----- S11 timing --------------------------------------------------------
JNIEXPORT jstring JNICALL
Java_com_mydriver_test_NativeBridge_runTimingDetector(JNIEnv* env, jobject) {
    driver.hwbp.clearAll();
    driver.setTarget(getpid());
    uint64_t addr = reinterpret_cast<uint64_t>(&probe_fn);
    const int REPS = 500;
    for (int i = 0; i < 1000; ++i) probe_fn();

    uint64_t base_ns = bench_ns(REPS, probe_fn);
    if (!driver.hwbp.install(addr, {}, false, DRV_HWBP_TYPE_X, DRV_HWBP_LEN_4))
        return toJ(env, FAIL("hwbp install failed"));
    uint64_t hwbp_ns = bench_ns(REPS, probe_fn);
    driver.hwbp.clearAll();

    if (!driver.hwbp.install(addr, {}, false, DRV_HWBP_TYPE_X, DRV_HWBP_LEN_4, DRV_HWBP_FLAG_TIMING_BYPASS))
        return toJ(env, FAIL("timing_bypass install failed"));
    uint64_t tb_ns = bench_ns(REPS, probe_fn);
    driver.hwbp.clearAll();

    double b = (double)base_ns / REPS;
    double h = (double)hwbp_ns / REPS;
    double t = (double)tb_ns   / REPS;
    return toJ(env, PASS("base=%.0fns hwbp=%.0fns tbypass=%.0fns (x%.1f vs x%.1f)",
                         b, h, t, h / (b > 0 ? b : 1.0), t / (b > 0 ? b : 1.0)));
}

// ----- S12 file hide -----------------------------------------------------
JNIEXPORT jstring JNICALL
Java_com_mydriver_test_NativeBridge_runFileHide(JNIEnv* env, jobject) {
    const char* marker = "test_hidden_marker_9f37";
    std::string path = std::string("/data/local/tmp/") + marker;
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return toJ(env, SKIP("cannot create %s errno=%d (app has no write perm)", path.c_str(), errno));
    std::fclose(f);

    if (!ioctl_name(DRV_CMD_HIDE_NAME_ADD, marker)) {
        int e = errno;
        unlink(path.c_str());
        return toJ(env, FAIL("hide_name_add failed errno=%d", e));
    }
    bool seen_after = false;
    FILE* d = popen("ls /data/local/tmp/ 2>/dev/null", "r");
    if (d) {
        char line[512];
        while (std::fgets(line, sizeof(line), d))
            if (std::strstr(line, marker)) { seen_after = true; break; }
        pclose(d);
    }
    ioctl_name(DRV_CMD_HIDE_NAME_REMOVE, marker);
    unlink(path.c_str());
    if (!seen_after) return toJ(env, PASS("marker vanished from readdir"));
    return toJ(env, FAIL("marker still visible in ls output"));
}

// ----- S13 pid hide ------------------------------------------------------
JNIEXPORT jstring JNICALL
Java_com_mydriver_test_NativeBridge_runPidHide(JNIEnv* env, jobject) {
    pid_t child = fork();
    if (child == 0) { pause(); _exit(0); }
    if (child < 0) return toJ(env, FAIL("fork failed"));

    drv_ioctl_req req{};
    req.pid = child;
    if (!driver.rawIoctl(DRV_CMD_HIDE_PID_ADD, &req)) {
        int e = errno;
        kill(child, SIGKILL); waitpid(child, nullptr, 0);
        return toJ(env, FAIL("HIDE_PID_ADD failed errno=%d", e));
    }
    bool seen = false;
    char cmd[128];
    std::snprintf(cmd, sizeof(cmd), "ls /proc/ 2>/dev/null | grep -w %d", child);
    FILE* p = popen(cmd, "r");
    if (p) {
        char l[64];
        if (std::fgets(l, sizeof(l), p)) seen = true;
        pclose(p);
    }
    drv_ioctl_req rem{}; rem.pid = child;
    driver.rawIoctl(DRV_CMD_HIDE_PID_REMOVE, &rem);
    kill(child, SIGKILL); waitpid(child, nullptr, 0);
    if (!seen) return toJ(env, PASS("pid %d vanished from /proc", child));
    return toJ(env, FAIL("pid %d still in /proc", child));
}

// ----- S14 fd-scoped cleanup --------------------------------------------
JNIEXPORT jstring JNICALL
Java_com_mydriver_test_NativeBridge_runFdScopedCleanup(JNIEnv* env, jobject) {
    Driver alt;
    if (!alt.open()) return toJ(env, SKIP("cannot open secondary fd errno=%d", errno));
    alt.setTarget(getpid());
    uint64_t addr = reinterpret_cast<uint64_t>(&probe_fn);
    if (!alt.hwbp.install(addr, {}, false, DRV_HWBP_TYPE_X, DRV_HWBP_LEN_4))
        return toJ(env, FAIL("install on alt fd failed"));
    alt.close();
    usleep(20000);
    drv_ioctl_req req{}; req.pid = getpid(); req.addr = addr;
    bool still_there = driver.rawIoctl(DRV_CMD_HWBP_REMOVE, &req);
    if (!still_there) return toJ(env, PASS("tracker gone after alt fd close"));
    return toJ(env, FAIL("tracker still present — cleanup missed"));
}

} // extern "C"
