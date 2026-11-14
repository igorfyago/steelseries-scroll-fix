// store.h : everything the tuner page shows and everything written to disk.
// One thread feeds it, the HTTP thread reads it, so it sits behind a mutex.
// The mouse hook never touches this file's code.
//
// Recorded per event: time, button or wheel size, the gap to the event before,
// and what the filter did. Never recorded: cursor position, window titles,
// keyboard input, mouse movement.
#pragma once
#include <cstdio>
#include <ctime>
#include <deque>
#include <share.h>
#include <mutex>
#include <string>
#include <vector>

#include "engine.h"

namespace fbw {

struct AppConfig {
    bool   enabled = false;     // false: record only, change nothing
    Config rules;
};

enum TestId : int { kNoTest = 0, kTestClicks = 1, kTestUp = 2, kTestDown = 3, kTestCount = 4 };
inline const char* testName(int id) {
    static const char* names[kTestCount] = {"", "clicks", "up", "down"};
    return (id > 0 && id < kTestCount) ? names[id] : "";
}

// A press that follows a release this soon cannot be a deliberate second click
// in a test that asks for one click at a time.
constexpr double kExtraPressMs = 200;
// A test ends this long after its last counted event, so a late bounce or a
// tick from the wheel settling still lands inside the test.
constexpr double kTestTailMs = 600;
// The click on the Start button is not part of the clicks test, and neither is
// a bounce of that click.
constexpr double kTestLeadMs = 350;

struct TestRun {
    int    id = kNoTest;
    bool   active = false;
    bool   guarded = false;          // guard state while it ran
    int    target = 0;
    int    good = 0;                 // clicks: deliberate presses. scroll: ticks in the asked direction
    int    wrong = 0;                // clicks: extra presses. scroll: ticks the other way
    int    caught = 0;               // faults the guard removed
    double startedT = 0, lastT = 0;
    int    goodBy[kButtons] = {};
    int    wrongBy[kButtons] = {};
    std::vector<float> wrongGaps;    // ms, one per wrong event
    std::string finishedAt;          // local wall clock
};

struct Hist {
    static constexpr int kBins = 100;
    double   binMs = 2;
    uint32_t bins[kBins] = {};
    uint32_t over = 0;
    void add(double gapMs) {
        if (gapMs < 0) return;
        const int i = static_cast<int>(gapMs / binMs);
        if (i >= kBins) ++over; else ++bins[i];
    }
    void clear() { for (uint32_t& b : bins) b = 0; over = 0; }
};

struct Counters {
    uint64_t presses[kButtons] = {};
    uint64_t bounces[kButtons] = {};
    uint64_t bridged = 0;
    uint64_t ticks = 0;
    uint64_t reversalsDropped = 0;
    uint64_t reversalsConfirmed = 0;
    uint64_t otherPrograms = 0;
};

class Store {
public:
    ~Store() { close(); }

    void open(const std::wstring& dataDir) {
        std::lock_guard<std::mutex> lock(mu_);
        dataDir_ = dataDir;
        pressGap_[0].binMs = pressGap_[1].binMs = pressGap_[2].binMs = 2;
        holdLen_.binMs = 2;
        reverseGap_.binMs = 5;
        sameGap_.binMs = 5;
        openLog();
    }

    void close() {
        std::lock_guard<std::mutex> lock(mu_);
        if (log_) { fclose(log_); log_ = nullptr; }
    }

    // Store thread: one record from the hook.
    void add(const Rec& r) {
        std::lock_guard<std::mutex> lock(mu_);
        Item it;
        it.r = r;
        it.seq = ++seq_;
        it.test = run_.active ? run_.id : kNoTest;
        account(it);
        events_.push_back(it);
        if (events_.size() > kKeep) events_.pop_front();
        writeLog(it);
    }

    // Store thread, a few times a second: end a test whose tail has run out.
    void poll(double now) {
        std::lock_guard<std::mutex> lock(mu_);
        if (run_.active && run_.good >= run_.target && now - run_.lastT >= kTestTailMs) finish();
        if (log_) fflush(log_);
    }

    bool startTest(int id, int target, bool guarded, double now) {
        std::lock_guard<std::mutex> lock(mu_);
        if (id <= kNoTest || id >= kTestCount || target < 1 || target > 5000) return false;
        if (run_.active) finish();
        run_ = TestRun{};
        run_.id = id;
        run_.active = true;
        run_.guarded = guarded;
        run_.target = target;
        run_.startedT = run_.lastT = now;
        for (int b = 0; b < kButtons; ++b) testUp_[b] = -1;
        return true;
    }

    void stopTest() {
        std::lock_guard<std::mutex> lock(mu_);
        if (run_.active) finish();
    }

    void resetStats() {
        std::lock_guard<std::mutex> lock(mu_);
        counters_ = Counters{};
        for (Hist& h : pressGap_) h.clear();
        holdLen_.clear();
        reverseGap_.clear();
        sameGap_.clear();
    }

    uint64_t seq() {
        std::lock_guard<std::mutex> lock(mu_);
        return seq_;
    }

    std::string stateJson(uint64_t since, const AppConfig& cfg, const Engine& eng, double now, int port) {
        std::lock_guard<std::mutex> lock(mu_);
        std::string j;
        j.reserve(16384);
        j += "{";
        num(j, "seq", static_cast<double>(seq_));
        num(j, "now", now, 1);
        num(j, "port", port);
        boolean(j, "enabled", cfg.enabled);

        j += "\"config\":{";
        boolean(j, "clickGuard", cfg.rules.clickGuard);
        num(j, "bounceLeft", cfg.rules.bounceMs[Left]);
        num(j, "bounceRight", cfg.rules.bounceMs[Right]);
        num(j, "bounceMiddle", cfg.rules.bounceMs[Middle]);
        num(j, "bounceSide", cfg.rules.bounceMs[X1]);
        boolean(j, "holdGuard", cfg.rules.holdGuard);
        num(j, "holdMs", cfg.rules.holdMs);
        boolean(j, "wheelGuard", cfg.rules.wheelGuard);
        num(j, "reverseMs", cfg.rules.reverseMs);
        num(j, "confirmMs", cfg.rules.confirmMs);
        endObj(j);

        j += "\"counters\":{";
        arr(j, "presses", counters_.presses, kButtons);
        arr(j, "bounces", counters_.bounces, kButtons);
        num(j, "bridged", static_cast<double>(counters_.bridged));
        num(j, "ticks", static_cast<double>(counters_.ticks));
        num(j, "reversalsDropped", static_cast<double>(counters_.reversalsDropped));
        num(j, "reversalsConfirmed", static_cast<double>(counters_.reversalsConfirmed));
        num(j, "otherPrograms", static_cast<double>(counters_.otherPrograms));
        endObj(j);

        j += "\"hook\":{";
        num(j, "calls", static_cast<double>(eng.hookCalls.load()));
        num(j, "rehooks", eng.rehooks.load());
        num(j, "lost", static_cast<double>(eng.lostRecords.load()));
        num(j, "sendFailures", eng.sendFailures.load());
        endObj(j);

        j += "\"test\":";
        testJson(j, run_, now);
        j += ",\"runs\":[";
        for (size_t i = 0; i < runs_.size(); ++i) {
            if (i) j += ",";
            testJson(j, runs_[i], now);
        }
        j += "],";

        j += "\"hist\":{";
        hist(j, "pressLeft", pressGap_[0]);
        hist(j, "pressRight", pressGap_[1]);
        hist(j, "pressOther", pressGap_[2]);
        hist(j, "hold", holdLen_);
        hist(j, "reverse", reverseGap_);
        hist(j, "same", sameGap_);
        endObj(j);

        j += "\"events\":[";
        bool first = true;
        size_t start = 0;
        if (events_.size() > kSend) start = events_.size() - kSend;
        for (size_t i = start; i < events_.size(); ++i) {
            const Item& it = events_[i];
            if (it.seq <= since) continue;
            if (!first) j += ",";
            first = false;
            char buf[200];
            snprintf(buf, sizeof(buf),
                     "{\"s\":%llu,\"t\":%.1f,\"k\":%d,\"b\":%d,\"d\":%d,\"w\":%d,\"src\":%d,\"x\":%d,\"v\":%d,\"st\":%d,\"g\":%.1f}",
                     static_cast<unsigned long long>(it.seq), it.r.t, it.r.kind, it.r.button, it.r.down, it.r.delta,
                     it.r.source, it.r.blocked, it.r.verdict, it.r.settled, it.gap);
            j += buf;
        }
        j += "]}";
        return j;
    }

private:
    static constexpr size_t kKeep = 2000;
    static constexpr size_t kSend = 300;

    struct Item {
        Rec      r;
        uint64_t seq = 0;
        int      test = kNoTest;
        float    gap = -1;     // physical gap to the event before, ms
    };

    // ---- accounting -------------------------------------------------------

    void account(Item& it) {
        const Rec& r = it.r;
        if (r.source == kOtherProgram) { ++counters_.otherPrograms; return; }

        if (r.kind == kSettled) {
            if (r.settled == static_cast<uint8_t>(Settled::WheelDropped)) {
                ++counters_.reversalsDropped;
                if (run_.active && run_.id != kTestClicks) ++run_.caught;
            }
            return;
        }

        const bool ours = r.source == kOurs;
        const bool delivered = !r.blocked;      // what applications receive

        if (r.kind == kButton) {
            const int b = r.button < kButtons ? r.button : 0;
            if (!ours) {
                // physical timing, whatever the filter decided
                if (r.down) {
                    ++counters_.presses[b];
                    if (physUp_[b] >= 0) {
                        it.gap = static_cast<float>(r.t - physUp_[b]);
                        pressGap_[b == Left ? 0 : b == Right ? 1 : 2].add(it.gap);
                    }
                    physDown_[b] = r.t;
                } else {
                    if (physDown_[b] >= 0) {
                        it.gap = static_cast<float>(r.t - physDown_[b]);
                        holdLen_.add(it.gap);
                    }
                    physUp_[b] = r.t;
                }
                if (r.verdict == static_cast<uint8_t>(Verdict::BouncePress)) ++counters_.bounces[b];
                if (r.verdict == static_cast<uint8_t>(Verdict::BridgedPress)) ++counters_.bridged;
            }
            if (run_.active && run_.id == kTestClicks && r.t - run_.startedT >= kTestLeadMs) {
                if (!ours && (r.verdict == static_cast<uint8_t>(Verdict::BouncePress) ||
                              r.verdict == static_cast<uint8_t>(Verdict::BridgedPress))) {
                    ++run_.caught;
                }
                if (delivered && r.down) {
                    const double gap = testUp_[b] >= 0 ? r.t - testUp_[b] : -1;
                    if (gap >= 0 && gap < kExtraPressMs) {
                        ++run_.wrong;
                        ++run_.wrongBy[b];
                        if (run_.wrongGaps.size() < 500) run_.wrongGaps.push_back(static_cast<float>(gap));
                    } else {
                        ++run_.good;
                        ++run_.goodBy[b];
                    }
                    run_.lastT = r.t;
                } else if (delivered && !r.down) {
                    testUp_[b] = r.t;
                }
            }
            return;
        }

        if (r.kind == kWheel) {
            const int dir = r.delta > 0 ? 1 : -1;
            if (!ours) {
                ++counters_.ticks;
                if (physTick_ >= 0) {
                    it.gap = static_cast<float>(r.t - physTick_);
                    (dir == physDir_ ? sameGap_ : reverseGap_).add(it.gap);
                }
                physTick_ = r.t;
                physDir_ = dir;
                if (r.settled == static_cast<uint8_t>(Settled::WheelDropped)) {
                    ++counters_.reversalsDropped;
                    if (run_.active && run_.id != kTestClicks) ++run_.caught;
                }
                if (r.settled == static_cast<uint8_t>(Settled::WheelConfirmed)) ++counters_.reversalsConfirmed;
            }
            if (run_.active && (run_.id == kTestUp || run_.id == kTestDown) && delivered) {
                const int want = run_.id == kTestUp ? 1 : -1;
                if (dir == want) {
                    ++run_.good;
                } else {
                    ++run_.wrong;
                    const double gap = testTick_ >= 0 ? r.t - testTick_ : -1;
                    if (run_.wrongGaps.size() < 500) run_.wrongGaps.push_back(static_cast<float>(gap));
                }
                testTick_ = r.t;
                run_.lastT = r.t;
            }
        }
    }

    void finish() {
        run_.active = false;
        testTick_ = -1;
        if (run_.good == 0 && run_.wrong == 0) return;   // started and stopped with nothing in it: not a run
        char stamp[32];
        const time_t now = time(nullptr);
        tm local{};
        localtime_s(&local, &now);
        strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &local);
        run_.finishedAt = stamp;
        runs_.push_back(run_);
        if (runs_.size() > 40) runs_.erase(runs_.begin());
        testTick_ = -1;

        const std::wstring path = dataDir_ + L"\\benchmarks.jsonl";
        FILE* f = nullptr;
        if (_wfopen_s(&f, path.c_str(), L"ab") == 0 && f) {
            std::string line;
            testJson(line, run_, run_.lastT);
            line += "\n";
            fwrite(line.data(), 1, line.size(), f);
            fclose(f);
        }
    }

    // ---- disk -------------------------------------------------------------

    void openLog() {
        char day[16];
        const time_t now = time(nullptr);
        tm local{};
        localtime_s(&local, &now);
        strftime(day, sizeof(day), "%Y%m%d", &local);
        wchar_t name[64];
        swprintf_s(name, L"\\events-%hs.csv", day);
        const std::wstring path = dataDir_ + name;
        const bool fresh = GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES;
        // Opened so that other programs can read the recording while it grows.
        log_ = _wfsopen(path.c_str(), L"ab", _SH_DENYWR);
        if (!log_) return;
        if (fresh) fputs("seq,t_ms,kind,button,down,delta,source,blocked,verdict,settled,rule_gap_ms,phys_gap_ms,test\n", log_);
        char stamp[32];
        strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S", &local);
        fprintf(log_, "# started %s, t_ms counts from here\n", stamp);
    }

    void writeLog(const Item& it) {
        if (!log_) return;
        fprintf(log_, "%llu,%.3f,%d,%d,%d,%d,%d,%d,%d,%d,%.3f,%.3f,%s\n",
                static_cast<unsigned long long>(it.seq), it.r.t, it.r.kind, it.r.button, it.r.down, it.r.delta,
                it.r.source, it.r.blocked, it.r.verdict, it.r.settled, it.r.gap, it.gap, testName(it.test));
    }

    // ---- json -------------------------------------------------------------

    static void num(std::string& j, const char* key, double v, int decimals = 0) {
        char buf[96];
        snprintf(buf, sizeof(buf), "\"%s\":%.*f,", key, decimals, v);
        j += buf;
    }
    static void boolean(std::string& j, const char* key, bool v) {
        j += "\"";
        j += key;
        j += v ? "\":true," : "\":false,";
    }
    static void endObj(std::string& j) {
        if (!j.empty() && j.back() == ',') j.pop_back();
        j += "},";
    }
    template <typename T>
    static void arr(std::string& j, const char* key, const T* v, int n) {
        j += "\"";
        j += key;
        j += "\":[";
        for (int i = 0; i < n; ++i) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%s%llu", i ? "," : "", static_cast<unsigned long long>(v[i]));
            j += buf;
        }
        j += "],";
    }
    static void hist(std::string& j, const char* key, const Hist& h) {
        char buf[64];
        snprintf(buf, sizeof(buf), "\"%s\":{\"binMs\":%.0f,\"over\":%u,\"bins\":[", key, h.binMs, h.over);
        j += buf;
        for (int i = 0; i < Hist::kBins; ++i) {
            snprintf(buf, sizeof(buf), "%s%u", i ? "," : "", h.bins[i]);
            j += buf;
        }
        j += "]},";
    }
    static void testJson(std::string& j, const TestRun& t, double now) {
        j += "{";
        j += "\"name\":\"";
        j += testName(t.id);
        j += "\",";
        boolean(j, "active", t.active);
        boolean(j, "guarded", t.guarded);
        num(j, "target", t.target);
        num(j, "good", t.good);
        num(j, "wrong", t.wrong);
        num(j, "caught", t.caught);
        num(j, "seconds", ((t.active ? now : t.lastT) - t.startedT) / 1000.0, 1);
        arr(j, "goodBy", t.goodBy, kButtons);
        arr(j, "wrongBy", t.wrongBy, kButtons);
        j += "\"wrongGaps\":[";
        for (size_t i = 0; i < t.wrongGaps.size(); ++i) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%s%.1f", i ? "," : "", t.wrongGaps[i]);
            j += buf;
        }
        j += "],\"finishedAt\":\"";
        j += t.finishedAt;
        j += "\"}";
    }

    std::mutex mu_;
    std::wstring dataDir_;
    FILE* log_ = nullptr;

    uint64_t seq_ = 0;
    std::deque<Item> events_;
    Counters counters_;
    Hist pressGap_[3];        // left, right, every other button
    Hist holdLen_;
    Hist reverseGap_, sameGap_;

    double physUp_[kButtons] = {-1, -1, -1, -1, -1};
    double physDown_[kButtons] = {-1, -1, -1, -1, -1};
    double physTick_ = -1;
    int    physDir_ = 0;

    TestRun run_;
    std::vector<TestRun> runs_;
    double testUp_[kButtons] = {-1, -1, -1, -1, -1};
    double testTick_ = -1;
};

}  // namespace fbw
