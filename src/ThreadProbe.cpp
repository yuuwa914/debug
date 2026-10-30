/*
 * ThreadProbe - read-only diagnostic mod for stutter hunting.
 *
 * Does NOT hook, patch or change anything in the game or in thread
 * priorities/affinity. Every few seconds it logs (logcat tag "ThreadProbe"):
 *   - busiest threads: name, tid, core they last ran on, CPU %, nice value
 *   - "wait": time a thread sat runnable but without a CPU (ms per second).
 *     High wait on a busy thread = the scheduler is starving it.
 *   - timer jitter: how late this probe's own 100 ms sleeps wake up. Big
 *     spikes = the whole CPU is contended or throttled at that moment.
 *   - CPU frequencies / max temperature, when the system lets apps read them.
 */
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <android/log.h>
#include <unistd.h>

#include <pl/Mod.hpp>

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

constexpr const char *kTag = "ThreadProbe";
constexpr auto kTick = 100ms;          /* jitter sampling period          */
constexpr int kTicksPerReport = 50;    /* 50 * 100 ms = 5 s               */
constexpr size_t kTopThreads = 6;
constexpr double kWaitFlagMsPerSec = 100.0; /* mark threads waiting > this */
constexpr double kJitterBadMs = 20.0;

void logLine(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void logLine(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    __android_log_print(ANDROID_LOG_INFO, kTag, "%s", buf);
}

struct Prev {
    unsigned long long runNs = 0;
    unsigned long long waitNs = 0;
};

struct Row {
    std::string name;
    int tid = 0;
    int core = -1;
    int nice = 0;
    double cpuPct = 0;
    double waitMsPerSec = 0;
};

bool readFile(const char *path, char *buf, size_t cap) {
    FILE *f = std::fopen(path, "r");
    if (!f) return false;
    size_t n = std::fread(buf, 1, cap - 1, f);
    std::fclose(f);
    buf[n] = '\0';
    return n > 0;
}

/* Nth whitespace-separated token (0-based) starting at p. */
bool token(const char *p, int index, char *out, size_t cap) {
    for (int i = 0; i <= index; ++i) {
        while (*p == ' ') ++p;
        if (!*p) return false;
        const char *s = p;
        while (*p && *p != ' ' && *p != '\n') ++p;
        if (i == index) {
            size_t len = std::min(static_cast<size_t>(p - s), cap - 1);
            std::memcpy(out, s, len);
            out[len] = '\0';
            return true;
        }
    }
    return false;
}

/* /proc/self/task/<tid>/stat: fields after the ")" of the comm field.
 * token 0=state(3) 16=nice(19) 36=processor(39). */
bool readStat(int tid, int &nice, int &core) {
    char path[64], buf[1024], t[32];
    std::snprintf(path, sizeof(path), "/proc/self/task/%d/stat", tid);
    if (!readFile(path, buf, sizeof(buf))) return false;
    const char *p = std::strrchr(buf, ')');
    if (!p) return false;
    ++p;
    if (!token(p, 16, t, sizeof(t))) return false;
    nice = std::atoi(t);
    if (!token(p, 36, t, sizeof(t))) return false;
    core = std::atoi(t);
    return true;
}

bool readSched(int tid, unsigned long long &run, unsigned long long &wait) {
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/self/task/%d/schedstat", tid);
    FILE *f = std::fopen(path, "r");
    if (!f) return false;
    unsigned long long slices = 0;
    int n = std::fscanf(f, "%llu %llu %llu", &run, &wait, &slices);
    std::fclose(f);
    return n >= 2;
}

std::string readComm(int tid) {
    char path[64], buf[64];
    std::snprintf(path, sizeof(path), "/proc/self/task/%d/comm", tid);
    if (!readFile(path, buf, sizeof(buf))) return "?";
    size_t n = std::strlen(buf);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == ' ')) buf[--n] = '\0';
    return buf;
}

void logFreqAndThermal(bool &freqWarned, bool &thermWarned) {
    /* CPU frequencies */
    char line[200];
    size_t used = 0;
    int readable = 0;
    used += std::snprintf(line, sizeof(line), "freq MHz:");
    for (int c = 0; c < 16 && used + 12 < sizeof(line); ++c) {
        char path[96], buf[32];
        std::snprintf(path, sizeof(path),
                      "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq",
                      c);
        if (!readFile(path, buf, sizeof(buf))) continue;
        ++readable;
        used += std::snprintf(line + used, sizeof(line) - used, " %d:%ld", c,
                              std::atol(buf) / 1000);
    }
    if (readable) logLine("%s", line);
    else if (!freqWarned) {
        logLine("cpufreq not readable by apps on this device");
        freqWarned = true;
    }

    /* Hottest thermal zone */
    long maxMilli = -1;
    std::string maxType;
    for (int z = 0; z < 64; ++z) {
        char path[96], buf[32];
        std::snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%d/temp", z);
        if (!readFile(path, buf, sizeof(buf))) continue;
        long v = std::atol(buf);
        if (v > maxMilli) {
            maxMilli = v;
            std::snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%d/type", z);
            char tb[48] = "?";
            if (readFile(path, tb, sizeof(tb))) {
                size_t n = std::strlen(tb);
                while (n && tb[n - 1] == '\n') tb[--n] = '\0';
            }
            maxType = tb;
        }
    }
    if (maxMilli >= 0) logLine("hottest zone: %s %.1f C", maxType.c_str(), maxMilli / 1000.0);
    else if (!thermWarned) {
        logLine("thermal zones not readable by apps on this device");
        thermWarned = true;
    }
}

class Probe {
public:
    void start() {
        std::lock_guard<std::mutex> lock(mMutex);
        if (mRunning) return;
        mStop = false;
        mRunning = true;
        mThread = std::thread([this] { run(); });
    }
    void stop() {
        {
            std::lock_guard<std::mutex> lock(mMutex);
            if (!mRunning) return;
            mStop = true;
        }
        mCv.notify_all();
        if (mThread.joinable()) mThread.join();
        std::lock_guard<std::mutex> lock(mMutex);
        mRunning = false;
    }

private:
    /* returns false when asked to stop */
    bool tickSleep() {
        std::unique_lock<std::mutex> lock(mMutex);
        return !mCv.wait_for(lock, kTick, [this] { return mStop; });
    }

    void report(double intervalSec, double jitterMax, int jitterBad) {
        DIR *d = opendir("/proc/self/task");
        if (!d) return;
        std::vector<Row> rows;
        std::unordered_map<int, Prev> now;
        while (dirent *e = readdir(d)) {
            int tid = std::atoi(e->d_name);
            if (tid <= 0) continue;
            unsigned long long run = 0, wait = 0;
            if (!readSched(tid, run, wait)) continue;
            now[tid] = {run, wait};
            auto it = mPrev.find(tid);
            if (it == mPrev.end()) continue; /* new thread: need a baseline */
            Row r;
            r.tid = tid;
            r.cpuPct = (run - it->second.runNs) / 1e9 / intervalSec * 100.0;
            r.waitMsPerSec = (wait - it->second.waitNs) / 1e6 / intervalSec;
            if (!readStat(tid, r.nice, r.core)) { r.nice = 0; r.core = -1; }
            r.name = readComm(tid);
            rows.push_back(std::move(r));
        }
        closedir(d);
        const bool baseline = mPrev.empty();
        mPrev.swap(now);
        if (baseline) {
            logLine("baseline taken, first report in 5 s");
            return;
        }

        double total = 0;
        for (auto &r : rows) total += r.cpuPct;
        std::sort(rows.begin(), rows.end(),
                  [](const Row &a, const Row &b) { return a.cpuPct > b.cpuPct; });

        logLine("--- %.0fs window: threads=%zu cores=%ld cpuTotal=%.0f%% | "
                "jitter max=%.0fms (>%.0fms x%d)",
                intervalSec, rows.size(), sysconf(_SC_NPROCESSORS_ONLN), total,
                jitterMax, kJitterBadMs, jitterBad);

        size_t shown = 0;
        for (auto &r : rows) {
            bool top = shown < kTopThreads;
            bool starved = r.waitMsPerSec >= kWaitFlagMsPerSec && r.cpuPct >= 5.0;
            if (!top && !starved) continue;
            logLine("  %-15.15s tid=%-6d core=%-2d cpu=%3.0f%% wait=%4.0fms/s nice=%d%s",
                    r.name.c_str(), r.tid, r.core, r.cpuPct, r.waitMsPerSec,
                    r.nice, r.waitMsPerSec >= kWaitFlagMsPerSec ? "  <- waiting" : "");
            ++shown;
        }
        logFreqAndThermal(mFreqWarned, mThermWarned);
    }

    void run() {
        auto lastReport = Clock::now();
        auto expected = lastReport + kTick;
        double jitterMax = 0;
        int jitterBad = 0;
        int ticks = 0;
        for (;;) {
            if (!tickSleep()) return;
            auto t = Clock::now();
            double late = std::chrono::duration<double, std::milli>(t - expected).count();
            if (late < 0) late = 0;
            jitterMax = std::max(jitterMax, late);
            if (late > kJitterBadMs) ++jitterBad;
            expected = t + kTick;

            if (++ticks >= kTicksPerReport) {
                double sec = std::chrono::duration<double>(t - lastReport).count();
                report(sec, jitterMax, jitterBad);
                lastReport = t;
                jitterMax = 0;
                jitterBad = 0;
                ticks = 0;
            }
        }
    }

    std::mutex mMutex;
    std::condition_variable mCv;
    std::thread mThread;
    bool mStop = false;
    bool mRunning = false;
    std::unordered_map<int, Prev> mPrev;
    bool mFreqWarned = false;
    bool mThermWarned = false;
};

Probe gProbe;

} // namespace

class ThreadProbeMod {
public:
    bool load() { return true; }
    bool enable() {
        gProbe.start();
        logLine("enabled (read-only diagnostics, report every 5 s)");
        return true;
    }
    bool disable() {
        gProbe.stop();
        return true;
    }
    bool unload() {
        gProbe.stop();
        return true;
    }
};

static ThreadProbeMod probeMod;
PL_REGISTER_MOD(ThreadProbeMod, probeMod)
