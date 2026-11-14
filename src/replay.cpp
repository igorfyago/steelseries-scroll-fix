// replay.cpp : runs a recording through the rules, to see what a set of
// thresholds would have done to events that really happened. The rules are the
// same filter.h the program uses, so nothing here can drift from it.
//
//   replay.exe <events.csv> [more.csv ...] [options]
//
// Defaults are the program's own defaults (src/filter.h).
//   --click 0|1        C1 on or off                      (default 1)
//   --bounce N         C1 window for every button, ms    (default 20)
//   --bounce-left N    --bounce-right N                  (one button)
//   --hold-left N      --hold-right N   C2 hold, ms      (default 0, off)
//   --blip N           press bounce window, ms, 0 is off (default 20)
//   --wheel 0|1        wheel rules on or off             (default 1)
//   --resend 0|1       send corrected wheel ticks        (default 0)
//   --reverse N        reversal window, ms               (default 70)
//   --confirm N        confirm window, ms                (default 200, never below --reverse)
//   --turn N           wait of a later tick against the remembered direction, ms;
//                      0: no direction is remembered     (default 1000, never below --confirm)
//   --first N          first tick wait, ms, 0 is off     (default 40, only with --resend 1)
//   --flip 0|1         turn a misread tick round, or drop it (default 1, only with --resend 1)
//   --list             print every event the rules would drop or hold
//   --sweep            print what each C1 window from 5 to 80 ms would drop
//
// Only what the mouse itself sent is replayed. Events other programs injected
// and events the guard sent during the recording are left out.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "filter.h"
#include "pairs.h"

using namespace fbw;

namespace {

// The same definition the recorder uses for the start of a clicks test (store.h).
constexpr double kTestLeadMs = 350;

struct Ev {
    double      t = 0;
    bool        wheel = false;
    int         button = 0;
    bool        down = false;
    int         delta = 0;
    Stamp       sys;
    std::string test;        // "", "clicks", "up", "down"
    int         run = 0;     // which run of that test, 0 outside tests
    long long   seq = 0;
};

struct Segment { std::vector<Ev> events; };     // one program start: its own clock

struct Out {                                    // what applications would receive
    double t = 0;
    bool   wheel = false;
    int    button = 0;
    bool   down = false;
    int    delta = 0;
    const Ev* from = nullptr;                   // the hardware event it came from, null for a late release
    std::string test;
    int    run = 0;
};

struct Dropped {
    const Ev* ev = nullptr;
    Verdict   verdict = Verdict::Passed;
    double    gap = -1;
    double    holdBefore = -1, holdAfter = -1;  // presses: the hold that ended before it, and its own
};

const char* kNames[kButtons] = {"left", "right", "middle", "side1", "side2"};

std::vector<std::string> split(const std::string& line) {
    std::vector<std::string> out;
    std::stringstream ss(line);
    std::string item;
    while (std::getline(ss, item, ',')) out.push_back(item);
    if (!line.empty() && line.back() == ',') out.push_back("");
    return out;
}

// Run numbers go on across files, so runs from two recordings are never merged.
std::map<std::string, int> g_runCount;

bool readFile(const std::wstring& path, std::vector<Segment>* segments) {
    std::ifstream in(path);       // a wide path: folders in any language open
    if (!in) { printf("cannot open %ls\n", path.c_str()); return false; }
    std::map<std::string, int> col;
    std::string line;
    std::map<std::string, int>& runCount = g_runCount;
    std::string lastTest;
    double leadUntil = -1;
    segments->push_back(Segment{});
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.empty()) continue;
        if (line[0] == '#') {
            if (line.rfind("# started", 0) == 0) {
                if (!segments->back().events.empty()) segments->push_back(Segment{});
                lastTest.clear();
                leadUntil = -1;
            } else if (line.rfind("# test ", 0) == 0 && line.find(" start ") != std::string::npos) {
                const size_t p = line.find("t_ms=");
                if (p != std::string::npos) leadUntil = atof(line.c_str() + p + 5) + kTestLeadMs;
                lastTest.clear();               // a new run begins, even of the same test
            }
            continue;
        }
        if (line.rfind("seq,", 0) == 0) {
            col.clear();
            const std::vector<std::string> names = split(line);
            for (size_t i = 0; i < names.size(); ++i) col[names[i]] = static_cast<int>(i);
            continue;
        }
        const std::vector<std::string> f = split(line);
        auto get = [&](const char* name) -> const char* {
            const auto it = col.find(name);
            return (it == col.end() || it->second >= static_cast<int>(f.size())) ? nullptr : f[static_cast<size_t>(it->second)].c_str();
        };
        const char* kind = get("kind");
        const char* source = get("source");
        const char* t = get("t_ms");
        if (!kind || !source || !t) continue;
        const int k = atoi(kind);
        const char* test = get("test");
        const std::string testName = test ? test : "";
        // Run numbering follows the tag, whatever kind of row carries it.
        if (testName != lastTest) {
            if (!testName.empty()) ++runCount[testName];
            lastTest = testName;
        }
        if (atoi(source) != 0 || (k != 0 && k != 1)) continue;
        // Known to be another mouse: not part of what the rules are about. An
        // event whose mouse was not recognised stays in.
        if (get("device") && get("judged") && atoi(get("device")) != 0 && atoi(get("judged")) == 0) continue;
        Ev e;
        e.t = atof(t);
        e.wheel = k == 1;
        e.button = get("button") ? atoi(get("button")) : 0;
        e.down = get("down") && atoi(get("down")) != 0;
        e.delta = get("delta") ? atoi(get("delta")) : 0;
        e.seq = get("seq") ? atoll(get("seq")) : 0;
        if (const char* sys = get("sys_ms")) {
            e.sys.known = true;
            e.sys.ms = static_cast<uint32_t>(strtoul(sys, nullptr, 10));
        }
        // The click on Start and its bounce are not part of a clicks test.
        if (!testName.empty() && !(testName == "clicks" && leadUntil >= 0 && e.t < leadUntil)) {
            e.test = testName;
            e.run = runCount[testName];
        }
        if (e.wheel && e.delta == 0) continue;
        if (!e.wheel && (e.button < 0 || e.button >= kButtons)) continue;
        segments->back().events.push_back(e);
    }
    if (segments->back().events.empty()) segments->pop_back();
    return true;
}

Out fromEmit(const Emit& em, double t) {
    Out o;
    o.t = t;
    if (em.kind == Emit::Wheel) { o.wheel = true; o.delta = em.delta; }
    else { o.button = em.button; o.down = false; }
    return o;
}

// Plays one segment the way the hook does: what ran out of time first, then the event.
void play(const Config& cfg, const Segment& seg, std::vector<Out>* out, std::vector<Dropped>* dropped) {
    Filter f;
    f.configure(cfg);
    // What is being held, so a late delivery keeps its test tag and a dropped
    // or turned tick can be listed.
    const Ev* heldRelease[kButtons] = {};
    std::vector<const Ev*> heldTicks;       // oldest first, at most two
    auto tagged = [&](const Emit& em, double t, const Ev* src) {
        Out o = fromEmit(em, t);
        if (src) { o.test = src->test; o.run = src->run; }
        return o;
    };
    auto runUntil = [&](double t) {
        for (;;) {
            const double d = f.nextDeadline();
            if (d > t) break;
            Settled s = Settled::None;
            Emit em;
            bool has = false;
            if (!f.tick(d, &s, &em, &has)) break;
            if (has && em.kind == Emit::ButtonUp) {
                out->push_back(tagged(em, d, heldRelease[em.button]));
                heldRelease[em.button] = nullptr;
                continue;
            }
            // Which held tick this was about. With two held, a tick sent as it
            // came is the one its own way, a turned one the other, and a
            // dropped one the one against the direction the rules remember.
            size_t which = 0;
            if (heldTicks.size() > 1) {
                int want = 0;
                if (has) want = (em.delta > 0 ? 1 : -1) * (s == Settled::WheelFlipped ? -1 : 1);
                else if (s == Settled::WheelDropped) want = -f.remembered();
                for (size_t i = 0; i < heldTicks.size() && want != 0; ++i) {
                    if ((heldTicks[i]->delta > 0 ? 1 : -1) == want) { which = i; break; }
                }
            }
            const Ev* src = heldTicks.empty() ? nullptr : heldTicks[which];
            if (has) out->push_back(tagged(em, d, src));
            if (src && s == Settled::WheelFlipped) dropped->push_back(Dropped{src, Verdict::WheelFirst, -1, -1, -1});
            if (src && s == Settled::WheelDropped) dropped->push_back(Dropped{src, Verdict::WheelHeld, -1, -1, -1});
            if (src) heldTicks.erase(heldTicks.begin() + static_cast<std::ptrdiff_t>(which));
            // A held tick the filter let go of without sending it went nowhere
            // (two that disagreed with nothing to decide between them).
            while (heldTicks.size() > static_cast<size_t>(f.wheelHeld())) {
                dropped->push_back(Dropped{heldTicks.front(), Verdict::WheelHeld, -1, -1, -1});
                heldTicks.erase(heldTicks.begin());
            }
        }
    };
    for (const Ev& e : seg.events) {
        runUntil(e.t);
        const Outcome o = e.wheel ? f.wheel(e.delta, e.t, e.sys) : f.button(e.button, e.down, e.t, e.sys);
        if (e.wheel && o.settled != Settled::None) {
            // This tick settled what was held. The held tick that goes against
            // it is the one that was dropped or turned round.
            if (o.settled == Settled::WheelFlipped || o.settled == Settled::WheelDropped) {
                for (const Ev* h : heldTicks) {
                    if ((h->delta > 0) != (e.delta > 0)) {
                        dropped->push_back(Dropped{h, o.settled == Settled::WheelFlipped ? Verdict::WheelFirst : Verdict::WheelHeld, -1, -1, -1});
                    }
                }
            }
            // A real turn whose held first notch could not be sent: that notch is lost.
            if (o.settled == Settled::WheelTurnLost)
                for (const Ev* h : heldTicks) dropped->push_back(Dropped{h, Verdict::WheelHeld, -1, -1, -1});
            heldTicks.clear();
        }
        if (!o.block) {
            Out d;
            d.t = e.t; d.wheel = e.wheel; d.button = e.button; d.down = e.down; d.delta = e.delta;
            d.from = &e; d.test = e.test; d.run = e.run;
            out->push_back(d);
        } else if (o.verdict == Verdict::HeldRelease) {
            heldRelease[e.button] = &e;
        } else if (o.verdict == Verdict::WheelHeld || o.verdict == Verdict::WheelFirst) {
            heldTicks.push_back(&e);
        } else {
            dropped->push_back(Dropped{&e, o.verdict, o.gapMs, -1, -1});
        }
        if (o.settled == Settled::ReleaseBridged) heldRelease[e.button] = nullptr;
        if (o.hasEmit) out->push_back(tagged(o.emit, e.t, &e));
        if (o.hasEmit2) out->push_back(tagged(o.emit2, e.t, &e));
    }
    runUntil(std::numeric_limits<double>::max());
}

// For each dropped press: how long the button had been held before the gap,
// and how long the dropped press itself lasted. Short after: the switch
// re-closed for an instant. Long after: a real hold was cut in two.
void describe(const Segment& seg, std::vector<Dropped>* dropped) {
    for (Dropped& d : *dropped) {
        if (!d.ev || d.ev->wheel || !d.ev->down) continue;
        const Ev* base = seg.events.data();
        const size_t i = static_cast<size_t>(d.ev - base);
        if (i >= seg.events.size()) continue;
        double lastDown = -1, lastUp = -1;
        for (size_t j = i; j-- > 0;) {
            const Ev& p = seg.events[j];
            if (p.wheel || p.button != d.ev->button) continue;
            if (!p.down && lastUp < 0) lastUp = p.t;
            else if (p.down && lastUp >= 0) { lastDown = p.t; break; }
        }
        if (lastDown >= 0 && lastUp >= 0) d.holdBefore = lastUp - lastDown;
        for (size_t j = i + 1; j < seg.events.size(); ++j) {
            const Ev& n = seg.events[j];
            if (n.wheel || n.button != d.ev->button) continue;
            if (!n.down) d.holdAfter = n.t - d.ev->t;
            break;
        }
    }
}

struct RunCount { int good = 0, wrong = 0; };

// Counts a guided test the way the recorder does, on a stream of delivered
// events. A press that comes soon after a release is judged when it is let go,
// because how long it lasted is part of the verdict (pairs.h).
template <typename Stream>
std::map<std::string, RunCount> countTests(const Stream& stream) {
    struct Button { std::string run; double lastUp = -1, lastDown = -1, lastHold = -1, suspect = -1; };
    std::map<std::string, RunCount> runs;
    std::map<std::string, Button> buttons;
    auto settle = [&](Button& b, double hold) {
        if (b.suspect < 0) return;
        if (buggedPair(b.suspect, b.lastHold, hold)) ++runs[b.run].wrong; else ++runs[b.run].good;
        b.suspect = -1;
    };
    for (const auto& e : stream) {
        if (e.test.empty()) continue;
        char key[64];
        snprintf(key, sizeof(key), "%s #%d", e.test.c_str(), e.run);
        RunCount& r = runs[key];
        if (e.test == "clicks") {
            if (e.wheel) continue;
            char bkey[80];
            snprintf(bkey, sizeof(bkey), "%s/%d", key, e.button);
            Button& b = buttons[bkey];
            b.run = key;
            if (e.down) {
                const double gap = b.lastUp >= 0 ? e.t - b.lastUp : -1;
                settle(b, -1);
                if (gap >= 0 && gap < kSuspectGapMs) b.suspect = gap; else ++r.good;
                b.lastDown = e.t;
            } else {
                const double hold = b.lastDown >= 0 ? e.t - b.lastDown : -1;
                settle(b, hold);
                b.lastHold = hold;
                b.lastUp = e.t;
            }
        } else if (e.wheel) {
            const int want = e.test == "up" ? 1 : -1;
            if ((e.delta > 0 ? 1 : -1) == want) ++r.good; else ++r.wrong;
        }
    }
    for (auto& kv : buttons) settle(kv.second, -1);     // still held down when the recording ended
    return runs;
}

double arg(const std::vector<std::wstring>& args, size_t* i) { return *i + 1 < args.size() ? _wtof(args[++*i].c_str()) : 0; }

}  // namespace

int wmain(int argc, wchar_t** argv) {
    Config cfg;
    bool list = false, sweep = false;
    const std::vector<std::wstring> args(argv, argv + argc);
    std::vector<std::wstring> files;
    for (size_t i = 1; i < args.size(); ++i) {
        const std::wstring& a = args[i];
        if (a == L"--click") cfg.clickGuard = arg(args, &i) != 0;
        else if (a == L"--bounce") { const double v = arg(args, &i); for (double& b : cfg.bounceMs) b = v; }
        else if (a == L"--bounce-left") cfg.bounceMs[Left] = arg(args, &i);
        else if (a == L"--bounce-right") cfg.bounceMs[Right] = arg(args, &i);
        else if (a == L"--hold-left") cfg.holdMs[Left] = arg(args, &i);
        else if (a == L"--hold-right") cfg.holdMs[Right] = arg(args, &i);
        else if (a == L"--blip") cfg.blipMs = arg(args, &i);
        else if (a == L"--wheel") cfg.wheelGuard = arg(args, &i) != 0;
        else if (a == L"--resend") cfg.wheelResend = arg(args, &i) != 0;
        else if (a == L"--reverse") cfg.reverseMs = arg(args, &i);
        else if (a == L"--confirm") cfg.confirmMs = arg(args, &i);
        else if (a == L"--turn") cfg.turnMs = arg(args, &i);
        else if (a == L"--first") cfg.firstMs = arg(args, &i);
        else if (a == L"--flip") cfg.wheelFlip = arg(args, &i) != 0;
        else if (a == L"--list") list = true;
        else if (a == L"--sweep") sweep = true;
        else if (a.rfind(L"--", 0) == 0) { printf("unknown option %ls\n", a.c_str()); return 2; }
        else files.push_back(a);
    }
    if (files.empty()) {
        printf("usage: replay <events.csv> [more.csv ...] [--bounce N] [--blip N] [--resend 0|1] [--reverse N] [--confirm N] [--turn N] [--list] [--sweep]\n");
        return 2;
    }
    keepSane(&cfg);     // as the program does with every setting it is given

    std::vector<Segment> segments;
    for (const std::wstring& path : files) if (!readFile(path, &segments)) return 2;

    size_t presses[kButtons] = {}, ticks = 0, events = 0;
    for (const Segment& s : segments) {
        for (const Ev& e : s.events) {
            ++events;
            if (e.wheel) ++ticks; else if (e.down) ++presses[e.button];
        }
    }
    printf("%zu events from the mouse in %zu recording session(s): presses left %zu, right %zu, middle %zu, side %zu; wheel ticks %zu\n",
           events, segments.size(), presses[Left], presses[Right], presses[Middle], presses[X1] + presses[X2], ticks);
    printf("rules: C1 %s (left %.0f, right %.0f ms), hold left %.0f right %.0f ms, press bounce %.0f ms, wheel %s (reverse %.0f, confirm %.0f, turn %.0f, %s)\n\n",
           cfg.clickGuard ? "on" : "off", cfg.bounceMs[Left], cfg.bounceMs[Right], cfg.holdMs[Left], cfg.holdMs[Right], cfg.blipMs,
           cfg.wheelGuard ? "on" : "off", cfg.reverseMs, cfg.confirmMs, cfg.turnMs,
           !cfg.wheelResend ? "sends nothing: misread ticks dropped" : cfg.wheelFlip ? "misread ticks turned round, first tick waits" : "misread ticks dropped, first tick waits");

    std::vector<Out> delivered;
    std::vector<Dropped> dropped;
    std::map<std::string, RunCount> before, after;
    for (const Segment& s : segments) {
        std::vector<Out> out;
        std::vector<Dropped> gone;
        play(cfg, s, &out, &gone);
        describe(s, &gone);
        for (const auto& kv : countTests(s.events)) { before[kv.first].good += kv.second.good; before[kv.first].wrong += kv.second.wrong; }
        for (const auto& kv : countTests(out)) { after[kv.first].good += kv.second.good; after[kv.first].wrong += kv.second.wrong; }
        delivered.insert(delivered.end(), out.begin(), out.end());
        dropped.insert(dropped.end(), gone.begin(), gone.end());
    }

    int droppedPress[kButtons] = {}, cutHolds = 0, droppedTicks = 0, turnedTicks = 0;
    for (const Dropped& d : dropped) {
        if (d.verdict == Verdict::BouncePress) {
            ++droppedPress[d.ev->button];
            if (d.holdAfter >= 60) ++cutHolds;
        } else if (d.verdict == Verdict::WheelHeld) {
            ++droppedTicks;
        } else if (d.verdict == Verdict::WheelFirst) {
            ++turnedTicks;
        }
    }
    size_t deliveredUp = 0, deliveredDown = 0, sentUp = 0, sentDown = 0;
    for (const Out& o : delivered) if (o.wheel) { if (o.delta > 0) ++deliveredUp; else ++deliveredDown; }
    for (const Segment& s : segments) for (const Ev& e : s.events) if (e.wheel) { if (e.delta > 0) ++sentUp; else ++sentDown; }
    printf("would drop: presses left %d, right %d, middle %d, side %d; wheel ticks dropped %d, turned round %d\n", droppedPress[Left],
           droppedPress[Right], droppedPress[Middle], droppedPress[X1] + droppedPress[X2], droppedTicks, turnedTicks);
    printf("wheel ticks: the mouse sent %zu up and %zu down, applications would get %zu up and %zu down\n", sentUp, sentDown,
           deliveredUp, deliveredDown);
    printf("dropped presses that stayed down 60 ms or longer (a real hold cut in two, or a real click wrongly dropped): %d\n\n", cutHolds);

    if (!before.empty()) {
        printf("%-14s %18s %18s\n", "guided test", "the mouse sent", "with these rules");
        printf("%-14s %9s %8s %9s %8s\n", "", "counted", "wrong", "counted", "wrong");
        for (const auto& kv : before) {
            const RunCount a = after.count(kv.first) ? after[kv.first] : RunCount{};
            printf("%-14s %9d %8d %9d %8d\n", kv.first.c_str(), kv.second.good, kv.second.wrong, a.good, a.wrong);
        }
        printf("\n");
    }

    if (list) {
        printf("%-8s %-12s %-22s %10s %12s %12s  %s\n", "seq", "t, s", "what", "gap, ms", "held before", "held itself", "test");
        for (const Dropped& d : dropped) {
            if (d.verdict == Verdict::BounceRelease) continue;      // goes with its press
            char what[40];
            if (d.ev->wheel) snprintf(what, sizeof(what), "wheel %s%s", d.ev->delta > 0 ? "up" : "down",
                                      d.verdict == Verdict::WheelFirst ? ", turned" : "");
            else snprintf(what, sizeof(what), "%s %s", kNames[d.ev->button], d.ev->down ? "press" : "release");
            printf("%-8lld %-12.3f %-22s %10.1f %12.1f %12.1f  %s\n", d.ev->seq, d.ev->t / 1000.0, what, d.gap, d.holdBefore,
                   d.holdAfter, d.ev->test.c_str());
        }
        printf("\n");
    }

    if (sweep) {
        printf("C1 window, ms   presses dropped (left / right)   of those, down 60 ms or longer\n");
        for (int w = 5; w <= 80; w += 5) {
            Config c = cfg;
            c.clickGuard = true;
            for (double& b : c.bounceMs) b = w;
            int left = 0, right = 0, cut = 0;
            for (const Segment& s : segments) {
                std::vector<Out> out;
                std::vector<Dropped> gone;
                play(c, s, &out, &gone);
                describe(s, &gone);
                for (const Dropped& d : gone) {
                    if (d.verdict != Verdict::BouncePress) continue;
                    if (d.ev->button == Left) ++left; else if (d.ev->button == Right) ++right;
                    if (d.holdAfter >= 60) ++cut;
                }
            }
            printf("%13d   %15d / %-12d   %d\n", w, left, right, cut);
        }
    }
    return 0;
}
