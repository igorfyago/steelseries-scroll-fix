// store.h : everything the tuner page shows and everything written to disk.
// One thread feeds it, the HTTP thread reads it, so it sits behind a mutex.
// The mouse hook never touches this file's code.
//
// Recorded per event: time, button or wheel size, the gap to the event before,
// and what the filter did. Never recorded: cursor position, window titles,
// keyboard input, mouse movement.
#pragma once
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <share.h>
#include <mutex>
#include <string>
#include <vector>

#include "engine.h"
#include "pairs.h"

namespace fbw {

struct AppConfig {
    bool   enabled = true;      // false: record only, change nothing
    Config rules;
    // The one mouse the guard and the tests are about, by USB vendor and
    // product id, chosen on the page. 0 and 0: every mouse, until one is chosen.
    uint16_t vid = 0, pid = 0;
    int    keepDays = 14;       // recordings older than this are deleted; 0 keeps them all
};

// What the page shows about the program itself.
struct AppInfo {
    int         port = 0;
    bool        hotkey = false;     // the keyboard off switch is registered
    const char* build = "";
};

enum TestId : int { kNoTest = 0, kTestClicks = 1, kTestUp = 2, kTestDown = 3, kTestCount = 4 };
inline const char* testName(int id) {
    static const char* names[kTestCount] = {"", "clicks", "up", "down"};
    return (id > 0 && id < kTestCount) ? names[id] : "";
}

// A test ends this long after its last counted event, so a late bounce or a
// tick from the wheel settling still lands inside the test.
constexpr double kTestTailMs = 600;
// The click on the Start button is not part of the clicks test, and neither is
// a bounce of that click.
constexpr double kTestLeadMs = 350;
// The mouse under test double clicks by itself, and the second click of such a
// pair would land on the button that took the place of the one just clicked.
// A stop this soon after the start, or a start this soon after a stop, is that
// bounce.
constexpr double kStopGuardMs = 500;
// A click this soon before a stop request is the click on the Stop button.
constexpr double kStopClickMs = 400;
// Runs are counted by rules that have changed. Runs written under older rules
// are still listed, marked as such, but the test cards only use current ones.
constexpr int kRunsVersion = 3;

struct TestRun {
    int    id = kNoTest;
    bool   active = false;
    bool   guarded = false;          // guard state while it ran
    int    target = 0;
    int    good = 0;                 // clicks: presses a finger made. scroll: ticks in the asked direction
    int    wrong = 0;                // clicks: presses no finger made. scroll: ticks the other way
    int    fast = 0;                 // clicks: of the good ones, double clicks by hand
    float  fastest = -1;             // clicks: the shortest release to press gap a finger made, ms
    int    caught = 0;               // faults the guard removed
    int    eaten = 0;                // clicks: presses a finger made that the guard dropped
    int    version = kRunsVersion;   // the counting rules it was written under
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
        if (!(gapMs >= 0)) return;                              // negative, or not a number
        if (gapMs >= kBins * binMs) { ++over; return; }         // compared before the cast: a gap of weeks does not fit an int
        ++bins[static_cast<int>(gapMs / binMs)];
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
    uint64_t ticksTurned = 0;
    uint64_t otherPrograms = 0;
    uint64_t otherMice = 0;
    uint64_t unknownMouse = 0;       // not judged because the mouse could not be told
    uint64_t lateEvents = 0;         // not judged because the callback began too late
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
        const time_t now = time(nullptr);
        tm local{};
        localtime_s(&local, &now);
        char stamp[32];
        strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S", &local);
        startedAt_ = stamp;
        loadRuns();
        openLog();
    }

    void close() {
        std::lock_guard<std::mutex> lock(mu_);
        if (run_.active) finish(lastNow_);
        if (log_) { fclose(log_); log_ = nullptr; }
    }

    // Store thread: one record from the hook.
    void add(const Rec& r) {
        std::lock_guard<std::mutex> lock(mu_);
        if (r.t > lastNow_) lastNow_ = r.t;
        Item it;
        it.r = r;
        it.seq = ++seq_;
        it.test = run_.active ? run_.id : kNoTest;
        account(it);
        events_.push_back(it);
        if (events_.size() > kKeep) events_.pop_front();
        writeLog(it);
    }

    // Store thread, a few times a second: end a test whose tail has run out,
    // and start a new recording file when the date changes.
    void poll(double now) {
        std::lock_guard<std::mutex> lock(mu_);
        lastNow_ = now;
        if (run_.active && run_.good >= run_.target && now - run_.lastT >= kTestTailMs) {
            stoppedT_ = now;          // the page swaps Stop for Start now too
            finish(now);
        }
        if (log_) fflush(log_);
        ++pollCount_;
        if (!log_ && pollCount_ % 50 == 0) openLog();           // the file could not be opened: keep trying
        if (pollCount_ % 300 == 0 && today() != logDay_) {
            if (log_) { fclose(log_); log_ = nullptr; }
            openLog();
        }
    }

    // The thresholds in force go into the recording, so every row can be read
    // against the settings it was judged with.
    void noteConfig(const AppConfig& c, double now) {
        std::lock_guard<std::mutex> lock(mu_);
        char buf[400];
        snprintf(buf, sizeof(buf),
                 "enabled=%d mouse=%04X:%04X click=%d bounce=%.0f/%.0f/%.0f/%.0f/%.0f hold=%.0f/%.0f/%.0f/%.0f/%.0f "
                 "blip=%.0f wheel=%d reverse=%.0f confirm=%.0f turn=%.0f first=%.0f flip=%d resend=%d",
                 c.enabled ? 1 : 0, c.vid, c.pid, c.rules.clickGuard ? 1 : 0, c.rules.bounceMs[0], c.rules.bounceMs[1],
                 c.rules.bounceMs[2], c.rules.bounceMs[3], c.rules.bounceMs[4], c.rules.holdMs[0], c.rules.holdMs[1],
                 c.rules.holdMs[2], c.rules.holdMs[3], c.rules.holdMs[4], c.rules.blipMs, c.rules.wheelGuard ? 1 : 0,
                 c.rules.reverseMs, c.rules.confirmMs, c.rules.turnMs, c.rules.firstMs, c.rules.wheelFlip ? 1 : 0,
                 c.rules.wheelResend ? 1 : 0);
        configLine_ = buf;
        keepDays_ = c.keepDays;
        if (log_) fprintf(log_, "# config t_ms=%.3f %s\n", now, configLine_.c_str());
    }

    // A mouse was seen for the first time: its number and name go into the
    // recording, so the device column can be read later.
    void noteDevice(int number, const Device& d, const std::string& name) {
        std::lock_guard<std::mutex> lock(mu_);
        DeviceRow row;
        row.vid = d.vid;
        row.pid = d.pid;
        // The name ends up in JSON and in the page, so only plain characters are
        // kept. The path only goes into the recording, as Windows wrote it.
        for (char c : name) row.name += plain(c) ? c : ' ';
        for (const wchar_t* c = d.path; *c; ++c) row.path += (*c >= 32 && *c < 127) ? static_cast<char>(*c) : '?';
        if (static_cast<int>(devices_.size()) < number) devices_.resize(static_cast<size_t>(number));
        devices_[static_cast<size_t>(number - 1)] = row;
        if (log_) fprintf(log_, "# device %d vid=%04X pid=%04X name=%s path=%s\n", number, d.vid, d.pid, row.name.c_str(), row.path.c_str());
    }

    static bool plain(char c) {
        return c >= 32 && c < 127 && c != '"' && c != '\\' && c != '<' && c != '>' && c != '&' && c != '\'';
    }

    bool startTest(int id, int target, bool guarded, double now) {
        std::lock_guard<std::mutex> lock(mu_);
        if (id <= kNoTest || id >= kTestCount || target < 1 || target > 5000) return false;
        // A second Start for the test that only just began is the same click bouncing.
        if (run_.active && run_.id == id && now - run_.startedT < kStopGuardMs) return true;
        // So is a Start right after a Stop: the page had put Start where Stop was.
        if (!run_.active && now - stoppedT_ < kStopGuardMs) return true;
        if (run_.active) finish(now);
        run_ = TestRun{};
        run_.id = id;
        run_.active = true;
        run_.guarded = guarded;
        run_.target = target;
        run_.startedT = run_.lastT = now;
        for (int b = 0; b < kButtons; ++b) { testUp_[b] = -1; testDown_[b] = -1; testHold_[b] = -1; suspectGap_[b] = -1; suspectDropped_[b] = false; testUpSysOk_[b] = false; }
        lastGoodT_ = -1;
        if (log_) fprintf(log_, "# test %s start t_ms=%.3f target=%d guarded=%d\n", testName(id), now, target, guarded ? 1 : 0);
        return true;
    }

    void stopTest(double now) {
        std::lock_guard<std::mutex> lock(mu_);
        if (!run_.active) {
            stoppedT_ = now;          // the test ended by itself as Stop was clicked: a bounce lands on Start
            return;
        }
        if (now - run_.startedT < kStopGuardMs) return;
        // The click on Stop was made with the mouse under test and has been
        // counted as a click of the test. Take it back.
        if (run_.id == kTestClicks && lastGoodT_ >= 0 && now - lastGoodT_ < kStopClickMs && run_.good > 0) {
            --run_.good;
            if (run_.goodBy[lastGoodB_] > 0) --run_.goodBy[lastGoodB_];
            if (lastGoodFast_ && run_.fast > 0) --run_.fast;
            run_.fastest = fastestBefore_;
        }
        stoppedT_ = now;
        finish(now);
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

    std::string stateJson(uint64_t since, const AppConfig& cfg, const Engine& eng, double now, const AppInfo& info) {
        std::lock_guard<std::mutex> lock(mu_);
        std::string j;
        j.reserve(16384);
        j += "{";
        num(j, "seq", static_cast<double>(seq_));
        num(j, "now", now, 1);
        num(j, "port", info.port);
        j += "\"build\":\"";
        j += info.build;
        j += "\",";
        boolean(j, "hotkey", info.hotkey);
        boolean(j, "enabled", cfg.enabled);
        boolean(j, "rawInput", eng.rawInput.load());
        boolean(j, "recording", log_ != nullptr);
        {
            char mouse[16];
            if (cfg.vid || cfg.pid) snprintf(mouse, sizeof(mouse), "%04X:%04X", cfg.vid, cfg.pid); else mouse[0] = 0;
            j += "\"mouse\":\"";
            j += mouse;
            j += "\",";
        }
        j += "\"devices\":[";
        for (size_t i = 0; i < devices_.size(); ++i) {
            char buf[64];
            snprintf(buf, sizeof(buf), "%s{\"n\":%zu,\"id\":\"%04X:%04X\",\"events\":%llu,\"name\":\"", i ? "," : "", i + 1,
                     devices_[i].vid, devices_[i].pid, static_cast<unsigned long long>(devices_[i].events));
            j += buf;
            j += devices_[i].name;
            j += "\"}";
        }
        j += "],";

        j += "\"config\":{";
        boolean(j, "clickGuard", cfg.rules.clickGuard);
        num(j, "bounceLeft", cfg.rules.bounceMs[Left]);
        num(j, "bounceRight", cfg.rules.bounceMs[Right]);
        num(j, "bounceMiddle", cfg.rules.bounceMs[Middle]);
        num(j, "bounceSide", cfg.rules.bounceMs[X1]);
        num(j, "holdLeft", cfg.rules.holdMs[Left]);
        num(j, "holdRight", cfg.rules.holdMs[Right]);
        num(j, "holdMiddle", cfg.rules.holdMs[Middle]);
        num(j, "holdSide", cfg.rules.holdMs[X1]);
        boolean(j, "wheelGuard", cfg.rules.wheelGuard);
        boolean(j, "wheelFlip", cfg.rules.wheelFlip);
        boolean(j, "wheelResend", cfg.rules.wheelResend);
        num(j, "blipMs", cfg.rules.blipMs);
        num(j, "keepDays", cfg.keepDays);
        num(j, "reverseMs", cfg.rules.reverseMs);
        num(j, "confirmMs", cfg.rules.confirmMs);
        num(j, "turnMs", cfg.rules.turnMs);
        num(j, "firstMs", cfg.rules.firstMs);
        endObj(j);

        j += "\"counters\":{";
        arr(j, "presses", counters_.presses, kButtons);
        arr(j, "bounces", counters_.bounces, kButtons);
        num(j, "bridged", static_cast<double>(counters_.bridged));
        num(j, "ticks", static_cast<double>(counters_.ticks));
        num(j, "reversalsDropped", static_cast<double>(counters_.reversalsDropped));
        num(j, "reversalsConfirmed", static_cast<double>(counters_.reversalsConfirmed));
        num(j, "ticksTurned", static_cast<double>(counters_.ticksTurned));
        num(j, "otherPrograms", static_cast<double>(counters_.otherPrograms));
        num(j, "otherMice", static_cast<double>(counters_.otherMice));
        num(j, "unknownMouse", static_cast<double>(counters_.unknownMouse));
        num(j, "lateEvents", static_cast<double>(counters_.lateEvents));
        endObj(j);

        j += "\"hook\":{";
        num(j, "calls", static_cast<double>(eng.hookCalls.load()));
        num(j, "rehooks", eng.rehooks.load());
        num(j, "lost", static_cast<double>(eng.lostRecords.load()));
        num(j, "sendFailures", eng.sendFailures.load());
        num(j, "slow", eng.slowCalls.load());
        num(j, "stalls", eng.stalls.load());
        num(j, "maxOwnUs", eng.maxOwnUs.load());
        num(j, "unknownDevice", eng.unknownDevice.load());
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
            char buf[240];
            snprintf(buf, sizeof(buf),
                     "{\"s\":%llu,\"t\":%.1f,\"k\":%d,\"b\":%d,\"d\":%d,\"w\":%d,\"src\":%d,\"x\":%d,\"v\":%d,\"st\":%d,\"g\":%.1f,"
                     "\"dev\":%d,\"j\":%d}",
                     static_cast<unsigned long long>(it.seq), it.r.t, it.r.kind, it.r.button, it.r.down, it.r.delta,
                     it.r.source, it.r.blocked, it.r.verdict, it.r.settled, it.gap, it.r.device, it.r.judged);
            j += buf;
        }
        j += "]}";
        return j;
    }

private:
    static constexpr size_t kKeep = 2000;
    static constexpr size_t kSend = 300;
    static constexpr size_t kKeepRuns = 40;

    struct Item {
        Rec      r;
        uint64_t seq = 0;
        int      test = kNoTest;
        float    gap = -1;     // physical gap to the event before, ms
    };

    struct DeviceRow {
        uint16_t    vid = 0, pid = 0;
        std::string name;      // as the device calls itself
        std::string path;
        uint64_t    events = 0;
    };

    // ---- accounting -------------------------------------------------------

    void account(Item& it) {
        const Rec& r = it.r;
        if (r.source == kOtherProgram) { ++counters_.otherPrograms; return; }
        if (r.source == kHardware && r.device >= 1 && static_cast<size_t>(r.device) <= devices_.size()) ++devices_[static_cast<size_t>(r.device) - 1].events;
        if (r.kind == kHWheel) return;      // the sideways wheel is never judged and never counted
        // Known to be another mouse: recorded, but not what the counts and the
        // tests are about. An event that went past the rules for another reason
        // (its mouse could not be told, or its callback ran too late to judge)
        // is different: it stays in the counts, where a fault in it shows.
        if (r.source == kHardware && r.kind != kSettled && !r.judged) {
            if (r.stalled) ++counters_.lateEvents;
            else if (r.device) { ++counters_.otherMice; return; }
            else ++counters_.unknownMouse;
        }

        if (r.kind == kSettled) {
            countWheelOutcome(r.settled);
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
                // only what really was dropped: an event let through after a stall keeps its verdict
                if (r.blocked && r.verdict == static_cast<uint8_t>(Verdict::BouncePress)) ++counters_.bounces[b];
                if (r.blocked && r.verdict == static_cast<uint8_t>(Verdict::BridgedPress)) ++counters_.bridged;
            }
            // The clicks test goes by what the mouse itself sent, and by when.
            // What the guard did with an event (dropped it, held it, sent it
            // late) decides where the press is counted, never how fast it was:
            // a release that the guard delays must not make a double click by
            // hand look like a fault.
            if (!ours && run_.active && run_.id == kTestClicks && r.t - run_.startedT >= kTestLeadMs) {
                if (r.down) {
                    double gap = testUp_[b] >= 0 ? r.t - testUp_[b] : -1;
                    // Two late callbacks can make a long gap look short on the
                    // fine clock. Windows' own stamps can tell: a gap they put
                    // well above the suspect range is a hand's.
                    if (gap >= 0 && gap < kSuspectGapMs && testUpSysOk_[b]) {
                        const uint32_t coarse = r.sys - testUpSys_[b];
                        if (coarse >= kSuspectGapMs + kCoarseStepMs && coarse < 60000u) gap = coarse;
                    }
                    const bool dropped = r.blocked != 0;
                    settlePress(b, -1);                 // a press with no release seen in between: judged without its hold
                    if (gap >= 0 && gap < kSuspectGapMs) {
                        suspectGap_[b] = gap;           // judged when it is released
                        suspectDropped_[b] = dropped;
                    } else {
                        countPress(b, gap, false, dropped);
                    }
                    testDown_[b] = r.t;
                    run_.lastT = r.t;
                } else {
                    const double hold = testDown_[b] >= 0 ? r.t - testDown_[b] : -1;
                    settlePress(b, hold);
                    testHold_[b] = hold;
                    testUpSys_[b] = r.sys;
                    testUpSysOk_[b] = true;
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
                countWheelOutcome(r.settled);
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

    // What became of a held wheel tick. A tick dropped or turned round is a
    // fault the guard removed.
    void countWheelOutcome(uint8_t settled) {
        const bool dropped = settled == static_cast<uint8_t>(Settled::WheelDropped);
        const bool turned = settled == static_cast<uint8_t>(Settled::WheelFlipped);
        if (dropped) ++counters_.reversalsDropped;
        if (turned) ++counters_.ticksTurned;
        if (settled == static_cast<uint8_t>(Settled::WheelConfirmed)) ++counters_.reversalsConfirmed;
        if ((dropped || turned) && run_.active && run_.id != kTestClicks) ++run_.caught;
    }

    // Counts one press of a clicks test. Four places it can go:
    //   the switch fired by itself and the guard dropped it      caught
    //   the switch fired by itself and it got through            wrong
    //   a finger made it and the guard dropped it                eaten
    //   a finger made it and it got through                      good
    void countPress(int b, double gap, bool bugged, bool dropped) {
        if (bugged && dropped) { ++run_.caught; return; }
        if (bugged) {
            ++run_.wrong;
            ++run_.wrongBy[b];
            if (run_.wrongGaps.size() < 500) run_.wrongGaps.push_back(static_cast<float>(gap));
            return;
        }
        if (dropped) { ++run_.eaten; return; }
        lastGoodT_ = lastNow_;
        lastGoodB_ = b;
        lastGoodFast_ = gap >= 0 && gap < kByHandMs;
        fastestBefore_ = run_.fastest;
        ++run_.good;
        ++run_.goodBy[b];
        if (lastGoodFast_) ++run_.fast;
        if (gap >= 0 && (run_.fastest < 0 || gap < run_.fastest)) run_.fastest = static_cast<float>(gap);
    }

    // A press that came soon after a release waits for its own release before
    // it is counted, because how long it lasted is part of the verdict.
    void settlePress(int b, double hold) {
        if (suspectGap_[b] < 0) return;
        const double gap = suspectGap_[b];
        suspectGap_[b] = -1;
        countPress(b, gap, buggedPair(gap, testHold_[b], hold), suspectDropped_[b]);
    }

    void finish(double now) {
        for (int b = 0; b < kButtons; ++b) settlePress(b, -1);     // still held down when the test ended
        run_.active = false;
        testTick_ = -1;
        if (log_) {
            fprintf(log_, "# test %s end t_ms=%.3f good=%d wrong=%d caught=%d eaten=%d\n", testName(run_.id), now, run_.good,
                    run_.wrong, run_.caught, run_.eaten);
        }
        if (run_.good == 0 && run_.wrong == 0 && run_.eaten == 0) return;   // started and stopped with nothing in it: not a run
        char stamp[32];
        const time_t wall = time(nullptr);
        tm local{};
        localtime_s(&local, &wall);
        strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &local);
        run_.finishedAt = stamp;
        runs_.push_back(run_);
        if (runs_.size() > kKeepRuns) runs_.erase(runs_.begin());

        const std::wstring path = dataDir_ + L"\\benchmarks.jsonl";
        FILE* f = nullptr;
        if (_wfopen_s(&f, path.c_str(), L"ab+") == 0 && f) {
            // If the line before was cut short (a crash mid write), this one
            // still starts on a line of its own.
            std::string line;
            if (fseek(f, -1, SEEK_END) == 0 && fgetc(f) != '\n') line = "\n";
            fseek(f, 0, SEEK_END);         // a read was the last operation: position before writing
            testJson(line, run_, run_.lastT);
            line += "\n";
            fwrite(line.data(), 1, line.size(), f);
            fclose(f);
        }
    }

    // ---- disk -------------------------------------------------------------

    static const char* logHeader() {
        return "seq,t_ms,kind,button,down,delta,source,blocked,verdict,settled,rule_gap_ms,phys_gap_ms,test,"
               "sys_ms,lag_ms,flags,vetoed,stalled,device,device_how,judged";
    }

    static int today() {
        const time_t now = time(nullptr);
        tm local{};
        localtime_s(&local, &now);
        return (local.tm_year + 1900) * 10000 + (local.tm_mon + 1) * 100 + local.tm_mday;
    }

    // Deletes recordings older than keepDays. Only files named events-YYYYMMDD*.csv
    // right in the data folder: anything moved into a subfolder is kept.
    void pruneOld() {
        if (keepDays_ <= 0) return;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dataDir_ + L"\\events-*.csv").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        const time_t now = time(nullptr);
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            int y = 0, mo = 0, d = 0;
            if (swscanf_s(fd.cFileName, L"events-%4d%2d%2d", &y, &mo, &d) != 3) continue;
            tm when{};
            when.tm_year = y - 1900;
            when.tm_mon = mo - 1;
            when.tm_mday = d;
            when.tm_hour = 12;
            const time_t t = mktime(&when);
            if (t == static_cast<time_t>(-1)) continue;
            if (difftime(now, t) > (keepDays_ + 0.5) * 86400.0) DeleteFileW((dataDir_ + L"\\" + fd.cFileName).c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    void openLog() {
        logDay_ = today();
        pruneOld();
        wchar_t name[64];
        swprintf_s(name, L"\\events-%08d.csv", logDay_);
        const std::wstring path = dataDir_ + name;
        bool fresh = GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES;
        if (!fresh && !headerMatches(path)) {
            // A recording from a build with other columns. Keep it under another
            // name so the two layouts never share a file.
            const time_t now = time(nullptr);
            tm local{};
            localtime_s(&local, &now);
            wchar_t old[96];
            swprintf_s(old, L"\\events-%08d-until-%02d%02d%02d.csv", logDay_, local.tm_hour, local.tm_min, local.tm_sec);
            if (MoveFileW(path.c_str(), (dataDir_ + old).c_str())) {
                fresh = true;
            } else {
                // The old file could not be moved out of the way (someone has it
                // open). Write this layout to a file of its own instead.
                swprintf_s(old, L"\\events-%08d-from-%02d%02d%02d.csv", logDay_, local.tm_hour, local.tm_min, local.tm_sec);
                log_ = _wfsopen((dataDir_ + old).c_str(), L"ab", _SH_DENYWR);
                if (!log_) return;
                writeLogHead(true);
                return;
            }
        }
        // Opened so that other programs can read the recording while it grows.
        log_ = _wfsopen(path.c_str(), L"ab", _SH_DENYWR);
        if (!log_) return;
        writeLogHead(fresh);
    }

    void writeLogHead(bool fresh) {
        if (fresh) fprintf(log_, "%s\n", logHeader());
        fprintf(log_, "# started %s, t_ms counts from there\n", startedAt_.c_str());
        if (!configLine_.empty()) fprintf(log_, "# config t_ms=0.000 %s\n", configLine_.c_str());
        for (size_t i = 0; i < devices_.size(); ++i) {
            fprintf(log_, "# device %zu vid=%04X pid=%04X name=%s path=%s\n", i + 1, devices_[i].vid, devices_[i].pid,
                    devices_[i].name.c_str(), devices_[i].path.c_str());
        }
    }

    static bool headerMatches(const std::wstring& path) {
        FILE* f = _wfsopen(path.c_str(), L"rb", _SH_DENYNO);
        if (!f) return false;
        char line[512] = {};
        const bool got = fgets(line, sizeof(line), f) != nullptr;
        fclose(f);
        if (!got) return false;
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        return strcmp(line, logHeader()) == 0;
    }

    void writeLog(const Item& it) {
        if (!log_) return;
        fprintf(log_, "%llu,%.3f,%d,%d,%d,%d,%d,%d,%d,%d,%.3f,%.3f,%s,%u,%d,%d,%d,%d,%d,%d,%d\n",
                static_cast<unsigned long long>(it.seq), it.r.t, it.r.kind, it.r.button, it.r.down, it.r.delta,
                it.r.source, it.r.blocked, it.r.verdict, it.r.settled, it.r.gap, it.gap, testName(it.test),
                it.r.sys, it.r.lag, it.r.flags, it.r.vetoed, it.r.stalled, it.r.device, it.r.deviceHow, it.r.judged);
    }

    // Finished runs are kept in benchmarks.jsonl, one per line, in the shape
    // testJson writes. Read the latest ones back so the page still shows them
    // after a restart.
    void loadRuns() {
        const std::wstring path = dataDir_ + L"\\benchmarks.jsonl";
        FILE* f = _wfsopen(path.c_str(), L"rb", _SH_DENYNO);
        if (!f) return;
        std::string line;
        char buf[4096];
        while (fgets(buf, sizeof(buf), f)) {
            line += buf;
            if (line.empty() || line.back() != '\n') continue;      // a long line: keep reading it
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
            TestRun t;
            if (line.size() > 2 && line.front() == '{' && line.back() == '}' && parseRun(line, &t)) {
                runs_.push_back(t);
                if (runs_.size() > kKeepRuns) runs_.erase(runs_.begin());
            }
            line.clear();
        }
        fclose(f);
    }

    static const char* valueOf(const std::string& line, const char* key) {
        const std::string needle = std::string("\"") + key + "\":";
        const size_t p = line.find(needle);
        return p == std::string::npos ? nullptr : line.c_str() + p + needle.size();
    }
    // A number from a line of benchmarks.jsonl. Anything that is not a finite
    // number of a sane size reads as 0, so a damaged line cannot make the
    // state the page reads invalid JSON.
    static double numberOf(const std::string& line, const char* key) {
        const char* v = valueOf(line, key);
        const double d = v ? strtod(v, nullptr) : 0;
        return std::isfinite(d) && d > -1e9 && d < 1e9 ? d : 0;
    }
    static std::string textOf(const std::string& line, const char* key) {
        const char* v = valueOf(line, key);
        if (!v || *v != '"') return "";
        const char* end = strchr(v + 1, '"');
        return end ? std::string(v + 1, end) : "";
    }
    template <typename Fn>
    static void eachOf(const std::string& line, const char* key, Fn fn) {
        const char* v = valueOf(line, key);
        if (!v || *v != '[') return;
        ++v;
        while (*v && *v != ']') {
            char* end = nullptr;
            const double d = strtod(v, &end);
            if (end == v) break;
            if (std::isfinite(d) && d > -1e9 && d < 1e9) fn(d);
            v = end;
            if (*v == ',') ++v;
        }
    }
    static bool parseRun(const std::string& line, TestRun* t) {
        t->version = static_cast<int>(numberOf(line, "v"));      // 0 when written before runs carried one
        const std::string name = textOf(line, "name");
        for (int i = 1; i < kTestCount; ++i) if (name == testName(i)) t->id = i;
        if (t->id == kNoTest) return false;
        const char* guarded = valueOf(line, "guarded");
        t->guarded = guarded && strncmp(guarded, "true", 4) == 0;
        t->target = static_cast<int>(numberOf(line, "target"));
        t->good = static_cast<int>(numberOf(line, "good"));
        t->wrong = static_cast<int>(numberOf(line, "wrong"));
        t->fast = static_cast<int>(numberOf(line, "fast"));
        t->fastest = valueOf(line, "fastest") ? static_cast<float>(numberOf(line, "fastest")) : -1;
        t->caught = static_cast<int>(numberOf(line, "caught"));
        t->eaten = static_cast<int>(numberOf(line, "eaten"));
        t->startedT = 0;
        t->lastT = numberOf(line, "seconds") * 1000.0;
        int i = 0;
        eachOf(line, "goodBy", [&](double d) { if (i < kButtons) t->goodBy[i++] = static_cast<int>(d); });
        i = 0;
        eachOf(line, "wrongBy", [&](double d) { if (i < kButtons) t->wrongBy[i++] = static_cast<int>(d); });
        eachOf(line, "wrongGaps", [&](double d) { if (t->wrongGaps.size() < 500) t->wrongGaps.push_back(static_cast<float>(d)); });
        for (char c : textOf(line, "finishedAt")) if (plain(c) && t->finishedAt.size() < 32) t->finishedAt += c;
        return t->good >= 0 && t->wrong >= 0 && t->caught >= 0 && t->eaten >= 0 && t->target >= 0;
    }

    // ---- json -------------------------------------------------------------

    static void num(std::string& j, const char* key, double v, int decimals = 0) {
        if (!std::isfinite(v)) v = 0;
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
        num(j, "v", t.version);
        boolean(j, "old", t.version != kRunsVersion);
        boolean(j, "active", t.active);
        boolean(j, "guarded", t.guarded);
        num(j, "target", t.target);
        num(j, "good", t.good);
        num(j, "wrong", t.wrong);
        num(j, "fast", t.fast);
        num(j, "fastest", t.fastest, 1);
        num(j, "caught", t.caught);
        num(j, "eaten", t.eaten);
        num(j, "seconds", ((t.active ? now : t.lastT) - t.startedT) / 1000.0, 1);
        arr(j, "goodBy", t.goodBy, kButtons);
        arr(j, "wrongBy", t.wrongBy, kButtons);
        j += "\"wrongGaps\":[";
        for (size_t i = 0; i < t.wrongGaps.size(); ++i) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%s%.1f", i ? "," : "", std::isfinite(t.wrongGaps[i]) ? t.wrongGaps[i] : 0.0f);
            j += buf;
        }
        j += "],\"finishedAt\":\"";
        j += t.finishedAt;
        j += "\"}";
    }

    std::mutex mu_;
    std::wstring dataDir_;
    FILE* log_ = nullptr;
    int   logDay_ = 0;
    unsigned pollCount_ = 0;
    std::string startedAt_;      // wall clock when recording began: t_ms 0
    std::string configLine_;     // the thresholds in force, as written into the recording

    uint64_t seq_ = 0;
    std::deque<Item> events_;
    std::vector<DeviceRow> devices_;
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
    uint32_t testUpSys_[kButtons] = {};                       // Windows' stamp of that release
    bool     testUpSysOk_[kButtons] = {};
    int      keepDays_ = 14;
    double testDown_[kButtons] = {-1, -1, -1, -1, -1};
    double testHold_[kButtons] = {-1, -1, -1, -1, -1};       // how long the click before lasted
    double suspectGap_[kButtons] = {-1, -1, -1, -1, -1};     // a press waiting for its release to be judged
    bool   suspectDropped_[kButtons] = {};                    // and whether the guard dropped it
    double testTick_ = -1;
    double lastNow_ = 0;             // the latest time this store was told
    double stoppedT_ = -1e9;         // when a test was last stopped by hand
    double lastGoodT_ = -1;          // the last press counted as made by a finger: when, which button,
    int    lastGoodB_ = 0;           // whether it was a fast one, and the fastest gap before it
    bool   lastGoodFast_ = false;
    float  fastestBefore_ = -1;
};

}  // namespace fbw
