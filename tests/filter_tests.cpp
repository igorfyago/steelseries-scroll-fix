// filter_tests.cpp : runs the cases in cases.txt through the filter and checks
// what applications would receive, then a few checks that need direct calls.
//
//   filter_tests.exe <path to cases.txt>
// Exit code 0 when everything passes.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "filter.h"       // both found through /I, so the mutation check can swap either
#include "whichmouse.h"
#include "pairs.h"

using namespace fbw;

struct Ev {
    double t = 0;
    bool   wheel = false;
    int    button = 0;
    bool   down = false;
    int    delta = 0;
    bool   bypass = false;      // injected by another program, or the sideways wheel: never judged
    bool   sideways = false;
    int    guard = 0;           // +1 guard switched on, -1 switched off, 0 an ordinary event
};

static std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static const char* kNames[kButtons] = {"L", "R", "M", "X1", "X2"};

static std::string show(const Ev& e) {
    char buf[64];
    if (e.wheel) snprintf(buf, sizeof(buf), "%g %s %+d", e.t, e.sideways ? "hwheel" : "wheel", e.delta);
    else snprintf(buf, sizeof(buf), "%g %s %s", e.t, kNames[e.button], e.down ? "down" : "up");
    return buf;
}

static bool parseEvents(const std::string& list, std::vector<Ev>* out, std::string* err) {
    std::stringstream ss(list);
    std::string item;
    while (std::getline(ss, item, ',')) {
        item = trim(item);
        if (item.empty()) continue;
        std::stringstream is(item);
        Ev e;
        std::string what, arg, tail;
        if (!(is >> e.t >> what)) { *err = "cannot read event: " + item; return false; }
        if (what == "on" || what == "off") {
            e.guard = what == "on" ? 1 : -1;
            out->push_back(e);
            continue;
        }
        if (!(is >> arg)) { *err = "cannot read event: " + item; return false; }
        if (is >> tail) {
            if (tail != "inj") { *err = "unknown word after event: " + item; return false; }
            e.bypass = true;
        }
        if (what == "wheel" || what == "hwheel") {
            e.wheel = true;
            e.sideways = what == "hwheel";
            if (e.sideways) e.bypass = true;
            e.delta = atoi(arg.c_str());
            if (e.delta == 0) { *err = "wheel needs a signed size: " + item; return false; }
        } else {
            int b = -1;
            for (int i = 0; i < kButtons; ++i) if (what == kNames[i]) b = i;
            if (b < 0 || (arg != "down" && arg != "up")) { *err = "cannot read event: " + item; return false; }
            e.button = b;
            e.down = arg == "down";
        }
        out->push_back(e);
    }
    return true;
}

static bool parseConfig(const std::string& text, Config* c, std::string* err) {
    c->clickGuard = false;
    for (double& h : c->holdMs) h = 0;
    c->wheelGuard = false;
    c->firstMs = 0;
    c->wheelFlip = false;
    c->blipMs = 0;           // cases name it when they want it
    c->turnMs = 0;           // the remembered direction too
    c->wheelResend = true;   // the reference vectors send a confirmed turn's first notch
    std::stringstream ss(text);
    std::string kv;
    while (ss >> kv) {
        const size_t eq = kv.find('=');
        if (eq == std::string::npos) { *err = "bad config item: " + kv; return false; }
        const std::string k = kv.substr(0, eq);
        const double v = atof(kv.substr(eq + 1).c_str());
        if (k == "click") c->clickGuard = v != 0;
        else if (k == "bounce") { for (double& b : c->bounceMs) b = v; }
        else if (k == "bounceL") c->bounceMs[Left] = v;
        else if (k == "bounceR") c->bounceMs[Right] = v;
        else if (k == "bounceM") c->bounceMs[Middle] = v;
        else if (k == "bounceX1") c->bounceMs[X1] = v;
        else if (k == "bounceX2") c->bounceMs[X2] = v;
        else if (k == "hold") { for (double& h : c->holdMs) h = v; }
        else if (k == "holdL") c->holdMs[Left] = v;
        else if (k == "holdR") c->holdMs[Right] = v;
        else if (k == "holdM") c->holdMs[Middle] = v;
        else if (k == "holdX1") c->holdMs[X1] = v;
        else if (k == "holdX2") c->holdMs[X2] = v;
        else if (k == "wheel") c->wheelGuard = v != 0;
        else if (k == "reverse") c->reverseMs = v;
        else if (k == "confirm") c->confirmMs = v;
        else if (k == "turn") c->turnMs = v;
        else if (k == "first") c->firstMs = v;
        else if (k == "flip") c->wheelFlip = v != 0;
        else if (k == "blip") c->blipMs = v;
        else if (k == "resend") c->wheelResend = v != 0;
        else { *err = "unknown config key: " + k; return false; }
    }
    return true;
}

static Ev fromEmit(const Emit& em, double t) {
    Ev e;
    e.t = t;
    if (em.kind == Emit::Wheel) { e.wheel = true; e.delta = em.delta; }
    else { e.button = em.button; e.down = false; }
    return e;
}

// Plays the input through a fresh filter the way the engine does: time based
// work first, in deadline order, then the event itself. Events from other
// programs and the sideways wheel go straight through, as they do in the hook.
// "off" lets go of what is held and stops hiding anything; "on" puts the
// thresholds back. What the buttons and the wheel were doing is kept across both.
static std::vector<Ev> play(const Config& cfg, const std::vector<Ev>& in) {
    Filter f;
    f.configure(cfg);
    std::vector<Ev> out;
    auto runUntil = [&](double t) {
        for (;;) {
            const double d = f.nextDeadline();
            if (d > t) break;
            Settled s;
            Emit em;
            bool has = false;
            if (!f.tick(d, &s, &em, &has)) break;
            if (has) out.push_back(fromEmit(em, d));
        }
    };
    for (const Ev& e : in) {
        runUntil(e.t);
        if (e.guard < 0) {
            Emit em;
            while (f.flush(&em)) out.push_back(fromEmit(em, e.t));
            Config off = cfg;
            off.clickGuard = false;
            for (double& h : off.holdMs) h = 0;
            off.blipMs = 0;
            off.wheelGuard = false;
            off.firstMs = 0;
            f.configure(off);
            f.forgetHidden();
            continue;
        }
        if (e.guard > 0) { f.configure(cfg); continue; }
        if (e.bypass) { out.push_back(e); continue; }
        const Outcome o = e.wheel ? f.wheel(e.delta, e.t) : f.button(e.button, e.down, e.t);
        if (!o.block) out.push_back(e);
        if (o.hasEmit) out.push_back(fromEmit(o.emit, e.t));
        if (o.hasEmit2) out.push_back(fromEmit(o.emit2, e.t));
    }
    runUntil(std::numeric_limits<double>::max());
    return out;
}

// Wheel ticks delivered at the same instant reach applications in no promised
// order (one passes through the hook, the others are sent by us), so such a
// group is compared as a set: each group is put in order of size first.
static void settleOrder(std::vector<Ev>* v) {
    bool swapped = true;
    while (swapped) {
        swapped = false;
        for (size_t i = 0; i + 1 < v->size(); ++i) {
            Ev& a = (*v)[i];
            Ev& b = (*v)[i + 1];
            if (a.wheel && b.wheel && !a.sideways && !b.sideways && std::fabs(a.t - b.t) < 1e-6 && a.delta > b.delta) {
                const Ev tmp = a;
                a = b;
                b = tmp;
                swapped = true;
            }
        }
    }
}

static bool same(std::vector<Ev> a, std::vector<Ev> b) {
    if (a.size() != b.size()) return false;
    settleOrder(&a);
    settleOrder(&b);
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::fabs(a[i].t - b[i].t) > 1e-6 || a[i].wheel != b[i].wheel || a[i].sideways != b[i].sideways) return false;
        if (a[i].wheel ? a[i].delta != b[i].delta : (a[i].button != b[i].button || a[i].down != b[i].down)) return false;
    }
    return true;
}

static std::string join(const std::vector<Ev>& v) {
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) s += (i ? ", " : "") + show(v[i]);
    return s.empty() ? "(nothing)" : s;
}

static int g_failed = 0, g_run = 0;

static void check(bool ok, const char* name) {
    ++g_run;
    if (!ok) { ++g_failed; printf("FAIL  %s\n", name); }
}

static int runCaseFile(const char* path) {
    std::ifstream in(path);
    if (!in) { printf("cannot open %s\n", path); return 1; }
    std::string line, name, config, input, expect;
    int lineNo = 0, cases = 0;
    auto finish = [&]() {
        if (name.empty()) return;
        ++cases;
        ++g_run;
        Config cfg;
        std::vector<Ev> evIn, evExpect;
        std::string err;
        if (!parseConfig(config, &cfg, &err) || !parseEvents(input, &evIn, &err) || !parseEvents(expect, &evExpect, &err)) {
            ++g_failed;
            printf("FAIL  %s\n      %s\n", name.c_str(), err.c_str());
        } else {
            const std::vector<Ev> got = play(cfg, evIn);
            if (!same(got, evExpect)) {
                ++g_failed;
                printf("FAIL  %s\n      want: %s\n      got:  %s\n", name.c_str(), join(evExpect).c_str(), join(got).c_str());
            }
        }
        name.clear(); config.clear(); input.clear(); expect.clear();
    };
    while (std::getline(in, line)) {
        ++lineNo;
        const std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        const size_t colon = t.find(':');
        if (colon == std::string::npos) { printf("line %d: no key\n", lineNo); return 1; }
        const std::string key = t.substr(0, colon);
        const std::string value = trim(t.substr(colon + 1));
        if (key == "case") { finish(); name = value; }
        else if (key == "config") config = value;
        else if (key == "in") input = value;
        else if (key == "out") expect = value;
        else { printf("line %d: unknown key %s\n", lineNo, key.c_str()); return 1; }
    }
    finish();
    printf("%d cases from file\n", cases);
    return cases == 0 ? 1 : 0;
}

static Config allOff() {
    Config c;
    c.clickGuard = false;
    for (double& h : c.holdMs) h = 0;
    c.wheelGuard = false;
    c.firstMs = 0;
    c.wheelFlip = false;
    c.blipMs = 0;
    c.turnMs = 0;
    c.wheelResend = true;
    return c;
}

static Stamp at(uint32_t ms) {
    Stamp s;
    s.known = true;
    s.ms = ms;
    return s;
}

static void directChecks() {
    // flush sends a held release so no button stays down for applications
    {
        Filter f;
        Config c = allOff();
        c.holdMs[Left] = 25;
        f.configure(c);
        f.button(Left, true, 0);
        const Outcome up = f.button(Left, false, 50);
        check(up.block && up.verdict == Verdict::HeldRelease, "bridge holds the release back");
        check(f.busy(), "busy while a release is held");
        check(f.nextDeadline() == 75, "deadline is release time plus hold");
        Emit em;
        check(f.flush(&em) && em.kind == Emit::ButtonUp && em.button == Left, "flush sends the held release");
        check(!f.flush(&em), "flush has nothing left");
        check(!f.busy(), "not busy after flush");
        check(f.nextDeadline() == std::numeric_limits<double>::infinity(), "no deadline after flush");
    }
    // a second release while one is held does not push the first one back
    {
        Filter f;
        Config c = allOff();
        c.holdMs[Left] = 25;
        f.configure(c);
        f.button(Left, true, 0);
        f.button(Left, false, 50);
        const Outcome again = f.button(Left, false, 60);
        check(again.block && again.verdict == Verdict::ExtraRelease, "a second release while one is held is dropped");
        check(f.nextDeadline() == 75, "and the held release keeps its time");
    }
    // the hold is set per button
    {
        Filter f;
        Config c = allOff();
        c.holdMs[Left] = 25;
        f.configure(c);
        f.button(Right, true, 0);
        const Outcome r = f.button(Right, false, 50);
        check(!r.block && !f.busy(), "a button with no hold set releases at once");
    }
    // busy while a press is hidden
    {
        Filter f;
        Config c = allOff();
        c.clickGuard = true;
        for (double& b : c.bounceMs) b = 40;
        f.configure(c);
        f.button(Left, true, 0);
        f.button(Left, false, 60);
        check(!f.busy(), "idle between clicks");
        const Outcome o = f.button(Left, true, 65);
        check(o.block && o.verdict == Verdict::BouncePress, "bounce press is reported as such");
        check(o.gapMs > 4.99 && o.gapMs < 5.01, "the gap that was judged is reported");
        check(f.busy(), "busy while a press is hidden");
        const Outcome r = f.button(Left, false, 70);
        check(r.block && r.verdict == Verdict::BounceRelease, "its release is reported as such");
        check(!f.busy(), "idle again after the hidden press is released");
    }
    // forgetHidden: the release of a hidden press passes afterwards
    {
        Filter f;
        Config c = allOff();
        c.clickGuard = true;
        f.configure(c);
        f.button(Left, true, 0);
        f.button(Left, false, 60);
        f.button(Left, true, 65);
        check(f.busy(), "a press is hidden");
        f.forgetHidden();
        check(!f.busy(), "nothing is hidden after forgetHidden");
        const Outcome r = f.button(Left, false, 70);
        check(!r.block, "the release of a press that is no longer hidden passes");
    }
    // tick reports what it settled, and an unconfirmed tick ends the stroke
    {
        Filter f;
        Config c = allOff();
        c.wheelGuard = true; c.reverseMs = 150; c.confirmMs = 200;
        f.configure(c);
        f.wheel(120, 0);
        const Outcome h = f.wheel(-120, 30);
        check(h.block && h.verdict == Verdict::WheelHeld, "reversed tick is held");
        check(f.busy(), "busy while a tick is held");
        Settled s = Settled::None;
        Emit em;
        bool has = true;
        check(!f.tick(229, &s, &em, &has), "nothing settles before the window ends");
        check(f.tick(230, &s, &em, &has) && s == Settled::WheelDropped && !has, "held tick is dropped when the window ends");
        check(!f.busy(), "idle after the held tick is dropped");
        const Outcome next = f.wheel(-120, 231);
        check(!next.block, "after an unconfirmed tick the next tick passes, whichever way it goes");
    }
    // out of range button numbers are ignored, not indexed
    {
        Filter f;
        const Outcome a = f.button(-1, true, 0);
        const Outcome b = f.button(kButtons, true, 0);
        check(!a.block && !b.block, "unknown buttons pass untouched");
        const Outcome w = f.wheel(0, 0);
        check(!w.block, "a zero wheel delta passes untouched");
    }
    // switching a guard off in mid flight does not strand anything
    {
        Filter f;
        Config c = allOff();
        c.clickGuard = true;
        c.holdMs[Left] = 25;
        f.configure(c);
        f.button(Left, true, 0);
        f.button(Left, false, 50);          // held
        Emit em;
        while (f.flush(&em)) {}
        f.configure(allOff());
        f.forgetHidden();
        const Outcome o = f.button(Left, true, 55);
        check(!o.block, "after switching off, the next press passes");
        const Outcome r = f.button(Left, false, 110);
        check(!r.block, "and so does its release");
    }
    // coarse clock veto, clicks: the fine clock says 5 ms, Windows says the gap was long
    {
        Config c = allOff();
        c.clickGuard = true;
        for (double& b : c.bounceMs) b = 30;
        {
            Filter f;
            f.configure(c);
            f.button(Left, true, 0, at(1000));
            f.button(Left, false, 60, at(1060));
            const Outcome o = f.button(Left, true, 65, at(1106));     // 46 on the coarse clock = 30 + 16
            check(!o.block && o.vetoed, "a press is not dropped when the coarse clock says the gap was long");
            const Outcome r = f.button(Left, false, 120, at(1160));
            check(!r.block, "and its release passes");
        }
        {
            Config held = c;
            held.holdMs[Left] = 25;
            Filter f;
            f.configure(held);
            f.button(Left, true, 0, at(1000));
            f.button(Left, false, 60, at(1060));
            Settled s = Settled::None;
            Emit em;
            bool has = false;
            f.tick(85, &s, &em, &has);                                 // the held release goes out
            const Outcome p = f.button(Left, true, 88, at(1110));      // 28 ms on the fine clock, 50 on the coarse one
            check(!p.block && p.vetoed, "a vetoed press passes with the hold on as well");
            const Outcome r = f.button(Left, false, 150, at(1170));
            check(r.block && r.verdict == Verdict::HeldRelease, "and it counts as down for applications: its release is held like any other");
        }
        {
            Filter f;
            f.configure(c);
            f.button(Left, true, 0, at(1000));
            f.button(Left, false, 60, at(1060));
            const Outcome o = f.button(Left, true, 65, at(1105));     // one short of the veto
            check(o.block && !o.vetoed, "a coarse gap just under threshold plus one clock step does not veto");
        }
        {
            Filter f;
            f.configure(c);
            f.button(Left, true, 0);
            f.button(Left, false, 60);
            const Outcome o = f.button(Left, true, 65);
            check(o.block && !o.vetoed, "without coarse stamps the fine clock decides alone");
        }
        {
            Filter f;
            f.configure(c);
            f.button(Left, true, 0, at(1000));
            f.button(Left, false, 60);                                 // release carries no stamp
            const Outcome o = f.button(Left, true, 65, at(5000));
            check(o.block, "a stamp on one side only cannot veto");
        }
        {
            Filter f;
            f.configure(c);
            f.button(Left, true, 0, at(0xFFFFFF00u));
            f.button(Left, false, 60, at(0xFFFFFFF0u));
            const Outcome o = f.button(Left, true, 65, at(0x00000040u));   // 80 later, across the wrap
            check(!o.block && o.vetoed, "the veto survives the coarse clock wrapping round");
        }
        {
            Filter f;
            f.configure(c);
            f.button(Left, true, 0, at(1000));
            f.button(Left, false, 60, at(1060));
            const Outcome o = f.button(Left, true, 400, at(1062));    // fine clock says long: no block at all
            check(!o.block && !o.vetoed, "the coarse clock never causes a block");
        }
    }
    // first tick of a stroke: held, then sent as it came when nothing follows
    {
        Filter f;
        Config c = allOff();
        c.wheelGuard = true; c.reverseMs = 70; c.confirmMs = 60; c.firstMs = 40;
        f.configure(c);
        const Outcome first = f.wheel(-120, 0);
        check(first.block && first.verdict == Verdict::WheelFirst, "the first tick of a stroke is held");
        check(f.busy() && f.nextDeadline() == 40, "it waits exactly the first tick time");
        Settled s = Settled::None;
        Emit em;
        bool has = false;
        check(!f.tick(39.9, &s, &em, &has), "nothing is sent before that time is up");
        check(f.tick(40, &s, &em, &has) && s == Settled::WheelLate && has && em.kind == Emit::Wheel && em.delta == -120,
              "when the time is up the first tick is sent as it came");
        check(!f.busy(), "nothing is held after that");
        const Outcome second = f.wheel(-120, 60);
        check(!second.block && !second.hasEmit, "the stroke counts from when the mouse sent the first tick: a tick 60 ms after it goes straight through");
    }
    // a held first tick is sent when the guard lets go, a held reversed tick is not
    {
        Filter f;
        Config c = allOff();
        c.wheelGuard = true; c.reverseMs = 70; c.confirmMs = 60; c.firstMs = 40;
        f.configure(c);
        f.wheel(240, 0);
        Emit em;
        check(f.flush(&em) && em.kind == Emit::Wheel && em.delta == 240, "flush sends a held first tick with its own size");
        check(!f.flush(&em) && !f.busy(), "and then has nothing left");
        f.wheel(120, 10);
        const Outcome rev = f.wheel(-120, 20);
        check(rev.block && rev.verdict == Verdict::WheelHeld, "a tick against the stroke is held as a reversal");
        check(!f.flush(&em) && !f.busy(), "flush forgets a held reversed tick");
    }
    // turning a misread tick round
    {
        Filter f;
        Config c = allOff();
        c.wheelGuard = true; c.reverseMs = 70; c.confirmMs = 60; c.firstMs = 0; c.wheelFlip = true;
        f.configure(c);
        f.wheel(120, 0);
        f.wheel(120, 8);
        f.wheel(-240, 12);                               // misread, twice the size
        const Outcome next = f.wheel(120, 20);
        check(!next.block && next.settled == Settled::WheelFlipped && next.hasEmit && next.emit.delta == 240,
              "a tick shown to be misread is sent turned round, with its own size");
    }
    // the coarse clock veto never delays a tick that goes with the stroke
    {
        Filter f;
        Config c = allOff();
        c.wheelGuard = true; c.reverseMs = 70; c.confirmMs = 60; c.firstMs = 40;
        f.configure(c);
        f.wheel(120, 0, at(1000));
        Settled s = Settled::None;
        Emit em;
        bool has = false;
        f.tick(40, &s, &em, &has);                       // the first tick goes out
        const Outcome same = f.wheel(120, 50, at(1200)); // fine clock: in the stroke. coarse clock: a long pause
        check(!same.block && !same.vetoed, "a tick that goes with the stroke passes at once, whatever the coarse clock says");
        const Outcome against = f.wheel(-120, 60, at(1400));
        check(against.vetoed && against.block && against.verdict == Verdict::WheelFirst,
              "a vetoed reversal is treated as the first tick of a new stroke");
        check(f.tick(100, &s, &em, &has) && s == Settled::WheelLate && has && em.delta == -120, "so it is delayed, never lost");
    }
    // the first two ticks of a stroke disagree: a third decides
    {
        Config c = allOff();
        c.wheelGuard = true; c.reverseMs = 70; c.confirmMs = 50; c.firstMs = 40; c.wheelFlip = true;
        {
            Filter f;
            f.configure(c);
            f.wheel(-120, 0);                              // misread first tick
            const Outcome second = f.wheel(240, 10);
            check(second.block && second.verdict == Verdict::WheelFirst && !second.hasEmit, "a second tick that disagrees with the first waits as well");
            check(f.nextDeadline() == 50, "the pair waits the first tick time, counted from the second tick");
            const Outcome third = f.wheel(120, 18);
            check(!third.block && third.settled == Settled::WheelFlipped && third.hasEmit && third.hasEmit2, "the third tick settles both");
            check(third.emit.delta == 240 && third.emit2.delta == 120, "the one that agrees goes out as it came, the other turned round with its own size");
            check(!f.busy(), "nothing is held after the vote");
        }
        {
            Config drop = c;
            drop.wheelFlip = false;
            Filter f;
            f.configure(drop);
            f.wheel(120, 0);
            f.wheel(-120, 10);                             // misread second tick
            const Outcome third = f.wheel(120, 18);
            check(third.settled == Settled::WheelDropped && third.hasEmit && !third.hasEmit2 && third.emit.delta == 120,
                  "with turning off, the odd one out is dropped and the other is sent");
        }
        {
            Filter f;
            f.configure(c);
            f.wheel(120, 0);
            f.wheel(-120, 10);
            Settled s = Settled::None;
            Emit em;
            bool has = false;
            check(!f.tick(49.9, &s, &em, &has), "a pair waits until its time is up");
            check(f.tick(50, &s, &em, &has) && s == Settled::WheelLate && has && em.delta == 120, "then the first of the two is sent as it came");
            check(f.tick(50, &s, &em, &has) && s == Settled::WheelLate && has && em.delta == -120, "and the second right after it");
            check(!f.tick(50, &s, &em, &has) && !f.busy(), "and nothing is left");
        }
        {
            Filter f;
            f.configure(c);
            f.wheel(120, 0);
            f.wheel(-120, 10);
            Emit em;
            check(f.flush(&em) && em.delta == 120, "flush sends the first of a waiting pair");
            check(f.flush(&em) && em.delta == -120, "and then the second");
            check(!f.flush(&em) && !f.busy(), "and then has nothing left");
        }
    }
    // a tick against the stroke with nothing after it
    {
        Config c = allOff();
        c.wheelGuard = true; c.reverseMs = 70; c.confirmMs = 50; c.firstMs = 0; c.wheelFlip = true;
        Filter f;
        f.configure(c);
        f.wheel(120, 0);
        f.wheel(120, 8);
        f.wheel(-240, 30);
        Settled s = Settled::None;
        Emit em;
        bool has = false;
        check(!f.tick(79.9, &s, &em, &has), "a suspect tick waits the confirm time");
        check(f.tick(80, &s, &em, &has) && s == Settled::WheelFlipped && has && em.kind == Emit::Wheel && em.delta == 240,
              "then it is sent turned round, with its own size");
        const Outcome next = f.wheel(-120, 95);
        check(next.block && next.verdict == Verdict::WheelHeld,
              "the stroke goes on from the time the mouse sent that tick: a tick against it 65 ms later is still judged as part of it");
    }
    // which mouse: a hook event is matched to the raw record of the same event
    {
        const uint16_t kWheelFlag = 0x0400, kLeftDown = 0x0001, kLeftUp = 0x0002;
        uint8_t how = 0;
        {
            MouseMatcher m;
            m.seen(1, kWheelFlag, 100);
            check(m.match(kWheelFlag, 100.5, &how) == 1 && how == kDevExact, "an event is matched to the raw record that carries the same change");
        }
        {
            // What bit on 2026-11-14: the guard sends a tick itself, that leaves a
            // raw record with no device, and the next real tick must not take it.
            MouseMatcher m;
            m.seen(1, kWheelFlag, 100);
            check(m.match(kWheelFlag, 100.1, &how) == 1, "first real tick");
            m.seen(0, kWheelFlag, 101);                 // the guard's own tick
            m.seen(1, kWheelFlag, 106);
            check(m.match(kWheelFlag, 106.1, &how) == 1 && how == kDevExact, "a raw record with no device is never matched to a real event");
            m.seen(0, kWheelFlag, 107);
            m.seen(0, kWheelFlag, 107.1);
            m.seen(1, kWheelFlag, 112);
            m.seen(1, kWheelFlag, 118);
            check(m.match(kWheelFlag, 112.1, &how) == 1 && how == kDevExact, "nor are two of them in a row");
            check(m.match(kWheelFlag, 118.1, &how) == 1 && how == kDevExact, "and the real records stay in step with their events");
        }
        {
            MouseMatcher m;
            m.seen(0, kWheelFlag, 100);
            check(m.match(kWheelFlag, 100.1, &how) == 0 && how == kDevUnknown, "with only device-less records nothing is known");
        }
        {
            MouseMatcher m;
            m.seen(1, kWheelFlag, 100);
            m.seen(2, kWheelFlag, 101);
            check(m.match(kWheelFlag, 101.5, &how) == 1, "the oldest matching record is used first");
            check(m.match(kWheelFlag, 101.6, &how) == 2, "then the next");
        }
        {
            MouseMatcher m;
            m.seen(1, kWheelFlag, 100);
            m.seen(2, kLeftDown, 101);
            check(m.match(kLeftDown, 101.2, &how) == 2 && how == kDevExact, "two mice at once: the click goes to the mouse that clicked");
            check(m.match(kWheelFlag, 101.3, &how) == 1 && how == kDevExact, "and the wheel tick to the mouse that scrolled");
        }
        {
            MouseMatcher m;
            m.seen(3, static_cast<uint16_t>(kLeftDown | kLeftUp), 100);     // one record, two changes
            check(m.match(kLeftDown, 100.1, &how) == 3 && how == kDevExact, "a record with two changes serves the first");
            check(m.match(kLeftUp, 100.2, &how) == 3 && how == kDevExact, "and the second");
            check(m.match(kLeftUp, 100.3, &how) == 3 && how == kDevLastActive, "but each only once: after that it is the mouse that was last active");
        }
        {
            MouseMatcher m;
            m.seen(1, kWheelFlag, 100);
            check(m.match(kWheelFlag, 100 + kRawFreshMs, &how) == 1 && how == kDevExact, "a record is fresh up to the limit");
            m.seen(1, kWheelFlag, 1000);
            check(m.match(kWheelFlag, 1000 + kRawFreshMs + 1, &how) == 1 && how == kDevLastActive, "a stale record is not matched: the last active mouse is used instead");
        }
        {
            MouseMatcher m;
            m.seen(2, 0, 100);                          // movement only
            check(m.match(kLeftDown, 150, &how) == 2 && how == kDevLastActive, "no record for the click: the mouse that just moved");
            check(m.match(kLeftDown, 100 + kLastMouseMs, &how) == 2, "up to the limit");
            check(m.match(kLeftDown, 100 + kLastMouseMs + 1, &how) == 0 && how == kDevUnknown, "and after that nothing is known");
        }
        {
            MouseMatcher m;
            m.seen(1, 0, 100);
            m.seen(0, 0, 200);                          // injected movement
            check(m.match(kLeftDown, 250, &how) == 1 && how == kDevLastActive, "injected movement does not become the last active mouse");
        }
        {
            MouseMatcher m;
            for (int i = 0; i < 100; ++i) m.seen(1, kWheelFlag, 100 + i);          // far more than it keeps
            m.seen(2, kLeftDown, 300);
            check(m.match(kLeftDown, 300.1, &how) == 2 && how == kDevExact, "old records make room for new ones");
        }
    }
    // which pairs of clicks count against the mouse
    {
        check(!buggedPair(30.0, 104.1, 79.9), "the fastest pair recorded from a hand (30 ms apart, long presses) is not a fault");
        check(!buggedPair(49.0, 97.0, 100.0) && !buggedPair(35.0, 105.0, 104.0), "nor are other fast pairs by hand");
        check(buggedPair(5.0, 40.0, 3.0), "a re-fire 5 ms after release is");
        check(buggedPair(19.9, 80.0, 80.0) && !buggedPair(20.0, 80.0, 80.0), "a gap under 20 ms is a fault whatever the presses look like, 20 ms is not");
        check(buggedPair(50.0, 80.0, 15.0) && buggedPair(59.9, 80.0, 15.0) && !buggedPair(60.0, 80.0, 15.0), "a blip press marks a pair up to 60 ms apart, not beyond");
        check(buggedPair(50.0, 15.0, 80.0) && buggedPair(50.0, 19.9, 80.0) && !buggedPair(50.0, 20.0, 80.0), "so does a blip as the first press, up to 20 ms");
        check(buggedPair(25.0, 80.0, 19.9) && !buggedPair(25.0, 80.0, 20.0), "a second press under 20 ms marks the pair, 20 ms does not");
        check(buggedPair(25.0, 3.0, 300.0), "so does a first press under 20 ms (the switch bounced on the way down)");
        check(!buggedPair(60.0, 3.0, 3.0) && !buggedPair(200.0, 80.0, 5.0), "pairs 60 ms or more apart are not looked at");
        check(!buggedPair(25.0, -1, -1) && buggedPair(5.0, -1, -1), "with the press lengths unknown only the gap can speak");
        check(!buggedPair(-1, 3.0, 3.0), "no gap, no pair");
    }
    // taking a hold back, for when the caller lets the event through after all
    {
        Config c = allOff();
        c.holdMs[Left] = 25;
        {
            Filter f;
            f.configure(c);
            f.button(Left, true, 0);
            f.button(Left, false, 50);                      // release held
            check(f.takeRelease(Left) && !f.busy(), "takeRelease hands the held release over");
            check(!f.takeRelease(Left), "but only once");
            const Outcome again = f.button(Left, false, 60);
            check(!again.block, "after that the button counts as up: a further release is not held");
        }
        {
            Filter f;
            f.configure(c);
            f.button(Left, true, 0);
            f.button(Left, false, 50);
            f.cancelRelease(Left);
            Emit em;
            check(!f.busy() && !f.flush(&em), "cancelRelease forgets the held release: nothing is sent for it");
            const Outcome up = f.button(Left, false, 300);
            check(up.block && up.verdict == Verdict::HeldRelease, "and the button still counts as down: its next release is held like any other");
        }
        {
            Filter f;
            f.configure(c);
            f.button(Left, true, 0);
            f.button(Left, false, 50);
            f.takeBackRelease(Left);
            Emit em;
            check(!f.busy() && !f.flush(&em), "takeBackRelease: nothing is left to send");
            const Outcome next = f.button(Left, false, 60);
            check(!next.block, "and the button counts as up");
        }
        {
            Filter f;
            check(!f.takeRelease(-1) && !f.takeRelease(kButtons), "out of range buttons are ignored");
        }
    }
    {
        Config c = allOff();
        c.wheelGuard = true; c.reverseMs = 70; c.confirmMs = 50; c.firstMs = 40; c.wheelFlip = true;
        Emit em;
        {
            Filter f;
            f.configure(c);
            f.wheel(120, 0);                                // first tick held
            f.takeBackTick();
            check(!f.busy() && !f.flush(&em), "takeBackTick: a held first tick is forgotten, not sent");
            const Outcome next = f.wheel(120, 10);
            check(!next.block && !next.hasEmit, "and the stroke goes on from it");
        }
        {
            Filter f;
            f.configure(c);
            f.wheel(120, 0);
            f.wheel(-120, 10);                              // a pair that disagrees, both held
            f.takeBackTick();                               // the caller lets the second one through
            check(f.busy(), "of a pair only the second is taken back");
            check(f.flush(&em) && em.delta == 120 && !f.flush(&em), "the first is still there to be sent");
        }
        {
            Filter f;
            f.configure(c);
            f.wheel(120, 0);
            f.wheel(120, 8);
            f.wheel(-120, 12);                              // a tick against the stroke, held
            f.takeBackTick();
            check(!f.busy(), "a held reversed tick is taken back too");
            const Outcome next = f.wheel(-120, 20);
            check(!next.block && !next.hasEmit, "and since it was let through, the stroke now runs its way");
        }
    }
    // second review: behaviour earlier tests could not tell apart
    {
        Config c = allOff();
        c.wheelGuard = true; c.reverseMs = 70; c.confirmMs = 50; c.firstMs = 40; c.wheelFlip = true;
        Filter f;
        f.configure(c);
        Settled s = Settled::None;
        Emit em;
        bool has = false;
        f.wheel(120, 0);
        f.wheel(120, 8);
        f.wheel(-120, 30);
        check(f.tick(80, &s, &em, &has) && s == Settled::WheelFlipped, "a held tick that times out is turned round");
        const Outcome o = f.wheel(-120, 110);
        check(o.block && o.verdict == Verdict::WheelFirst, "the stroke counts from when the mouse sent that tick: 80 ms later a tick against it starts a new stroke");
    }
    {
        Config c = allOff();
        c.wheelGuard = true; c.firstMs = 40;
        Filter f;
        f.configure(c);
        f.wheel(240, 0);
        const Outcome o = f.wheel(120, 10);
        check(o.hasEmit && o.emit.delta == 240 && !o.block, "a stroke opening +240, +120 sends the +240 with its own size");
    }
    {
        Config c = allOff();
        c.clickGuard = true;
        for (double& b : c.bounceMs) b = 20;
        Filter f;
        f.configure(c);
        f.button(Left, true, 0, at(1000));
        f.button(Left, false, 60, at(1060));
        const Outcome hidden = f.button(Left, true, 65, at(1065));
        check(hidden.block, "a re-press 5 ms after release is hidden");
        const Outcome vetoed = f.button(Left, true, 70, at(1100));
        check(!vetoed.block && vetoed.vetoed, "a second press that the coarse clock puts 40 ms after the release passes");
        const Outcome up = f.button(Left, false, 150, at(1150));
        check(!up.block, "and its release is delivered: nothing of the hidden press is left over");
    }
    {
        Config c = allOff();
        c.wheelGuard = true; c.reverseMs = 150; c.confirmMs = 200;
        Filter f;
        f.configure(c);
        f.wheel(120, 0);
        f.wheel(-120, 50);
        const Outcome o = f.wheel(-120, 90);
        check(o.settled == Settled::WheelConfirmed && o.hasEmit && o.emit.delta == -120, "a real turn is reported as confirmed, and its first notch is sent");
    }
    {
        Config c = allOff();
        c.holdMs[Left] = 25;
        Filter f;
        f.configure(c);
        f.button(Left, true, 0);
        f.button(Left, false, 100);
        const Outcome o = f.button(Left, true, 104);
        check(o.block && o.verdict == Verdict::BridgedPress && o.settled == Settled::ReleaseBridged, "a re-press inside the hold is reported as bridged");
    }
    // keepSane: a confirm window shorter than the reversal window is pulled up
    {
        Config c;
        c.reverseMs = 70; c.confirmMs = 50;
        keepSane(&c);
        check(c.confirmMs == 70, "a confirm window under the reversal window is raised to it");
        c.confirmMs = 90;
        keepSane(&c);
        check(c.confirmMs == 90, "a longer one is left alone");
        c.turnMs = 60;
        keepSane(&c);
        check(c.turnMs == 90, "a turn wait under the confirm window is raised to it");
        c.turnMs = 0;
        keepSane(&c);
        check(c.turnMs == 0, "a turn wait of 0 (no direction remembered) stays 0");
    }
    // the remembered direction: what is held, and how long
    {
        Config c = allOff();
        c.wheelGuard = true; c.reverseMs = 70; c.confirmMs = 200; c.turnMs = 1000; c.firstMs = 40; c.wheelFlip = true;
        Filter f;
        f.configure(c);
        check(f.remembered() == 0, "no direction before the first tick");
        Settled s = Settled::None;
        Emit em;
        bool has = false;
        f.wheel(120, 0);
        f.wheel(120, 10);
        check(f.remembered() == 1, "the direction of the stroke is remembered");
        const Outcome fast = f.wheel(-120, 30);
        check(fast.block && fast.verdict == Verdict::WheelHeld && f.nextDeadline() == 230, "a tick too soon after the stroke waits confirmMs");
        check(f.tick(230, &s, &em, &has) && has && em.delta == 120 && s == Settled::WheelFlipped, "then it is turned round");
        const Outcome slow = f.wheel(-120, 5000);
        check(slow.block && slow.verdict == Verdict::WheelHeld && f.nextDeadline() == 6000, "a tick against it after a long pause waits turnMs");
        const Outcome pair = f.wheel(120, 5100);
        check(pair.block && pair.verdict == Verdict::WheelFirst && f.nextDeadline() == 5300, "the old way next: both wait confirmMs for a third tick");
        f.takeBackTick();
        check(f.busy() && f.nextDeadline() == 6000, "taking back the second of them leaves the first held as before");
        check(f.tick(6000, &s, &em, &has) && !has && s == Settled::WheelDropped, "with nothing more it is dropped, not turned round");
        check(f.remembered() == 1, "and the direction stays");
        const Outcome again = f.wheel(-120, 7000);
        check(again.block && again.verdict == Verdict::WheelFirst, "the next tick the same way counts as its second: the hand turned, this is a first tick of the new way");
        check(f.remembered() == -1, "and the new direction is remembered");
    }
    // the coarse clock: a quick looking turn that was not quick is held as one a hand may have made
    {
        Config c = allOff();
        c.wheelGuard = true; c.reverseMs = 70; c.confirmMs = 200; c.turnMs = 1000;
        Filter f;
        f.configure(c);
        f.wheel(120, 0, at(1000));
        const Outcome o = f.wheel(-120, 30, at(1100));
        check(o.block && o.vetoed && f.nextDeadline() == 1030, "vetoed: held for turnMs, not confirmMs");
    }
    // without sending: nothing is ever sent for the wheel
    {
        Config c = allOff();
        c.wheelGuard = true; c.reverseMs = 70; c.confirmMs = 70; c.firstMs = 40; c.wheelFlip = true; c.wheelResend = false;
        Filter f;
        f.configure(c);
        Settled s = Settled::None;
        Emit em;
        bool has = false;
        const Outcome first = f.wheel(120, 0);
        check(!first.block, "without sending, the first tick of a stroke is not held");
        f.wheel(120, 8);
        const Outcome mis = f.wheel(-120, 12);
        check(mis.block && mis.verdict == Verdict::WheelHeld, "a tick against the stroke is still held");
        const Outcome next = f.wheel(120, 20);
        check(!next.block && !next.hasEmit && next.settled == Settled::WheelDropped, "the misread is dropped and nothing is sent");
        f.wheel(-120, 40);
        const Outcome turn = f.wheel(-120, 50);
        check(turn.settled == Settled::WheelConfirmed && !turn.hasEmit, "a real turn: its held first notch is dropped, nothing is sent");
        f.wheel(120, 60);
        check(f.tick(200, &s, &em, &has) && !has && s == Settled::WheelDropped, "a held tick that times out is dropped, not turned");
    }
    // the press bounce bridge
    {
        Config c = allOff();
        c.clickGuard = true; c.blipMs = 20;
        for (double& b : c.bounceMs) b = 20;
        Filter f;
        f.configure(c);
        f.button(Left, true, 0);
        const Outcome blipUp = f.button(Left, false, 3);
        check(blipUp.block && blipUp.verdict == Verdict::HeldRelease && f.nextDeadline() == 23, "the release of a 3 ms press is held for the bounce window");
        const Outcome re = f.button(Left, true, 7);
        check(re.block && re.verdict == Verdict::BridgedPress, "a re-press inside it continues the same press");
        const Outcome realUp = f.button(Left, false, 600);
        check(!realUp.block, "the release of the real press is not held");
        f.button(Left, true, 1000);
        const Outcome up = f.button(Left, false, 1080);
        check(!up.block, "the release of an 80 ms press passes at once");
        f.button(Left, true, 2000);
        const Outcome edge = f.button(Left, false, 2020);
        check(!edge.block, "a press of exactly 20 ms is a finger's: not held");
    }
    // which mouse: arrival order, literal limits, movement, several waiting records
    {
        const uint16_t kWheelFlag = 0x0400, kLeftDown = 0x0001;
        uint8_t how = 0;
        {
            MouseMatcher m;
            for (int i = 0; i < 31; ++i) { m.seen(3, kLeftDown, i); m.match(kLeftDown, i + 0.1, &how); }
            m.seen(1, kWheelFlag, 100);
            m.seen(2, kWheelFlag, 100);
            check(m.match(kWheelFlag, 100.1, &how) == 1, "records with the same time are matched in the order they arrived, across the wrap");
            check(m.match(kWheelFlag, 100.2, &how) == 2, "and the next one after it");
        }
        {
            MouseMatcher m;
            m.seen(1, kWheelFlag, 100);
            check(m.match(kWheelFlag, 300, &how) == 1 && how == kDevExact, "a record 200 ms old is still matched");
            m.seen(1, kWheelFlag, 1000);
            check(m.match(kWheelFlag, 1200.5, &how) == 1 && how == kDevLastActive, "one 200.5 ms old is not: the last active mouse is used");
        }
        {
            MouseMatcher m;
            m.seen(2, 0, 100);
            check(m.match(kLeftDown, 600, &how) == 2 && how == kDevLastActive, "the mouse that moved 500 ms ago is still the last active one");
            check(m.match(kLeftDown, 601, &how) == 0 && how == kDevUnknown, "501 ms ago it is not");
        }
        {
            MouseMatcher m;
            m.seen(2, kLeftDown, 100);
            for (int i = 0; i < 40; ++i) m.seen(1, 0, 100.5 + i * 0.01);
            check(m.match(kLeftDown, 101, &how) == 2 && how == kDevExact, "40 movement records after a click record leave it in place");
        }
        {
            MouseMatcher m;
            m.seen(1, kLeftDown, 100);
            m.seen(2, kLeftDown, 100.1);
            m.seen(3, kLeftDown, 100.2);
            const uint8_t a = m.match(kLeftDown, 101, &how);
            const uint8_t b = m.match(kLeftDown, 101, &how);
            const uint8_t d = m.match(kLeftDown, 101, &how);
            check(a == 1 && b == 2 && d == 3, "three waiting records are matched in order");
        }
    }
    // coarse clock veto, wheel
    {
        Config c = allOff();
        c.wheelGuard = true; c.reverseMs = 200; c.confirmMs = 150;
        {
            Filter f;
            f.configure(c);
            f.wheel(120, 0, at(1000));
            const Outcome o = f.wheel(-120, 50, at(1216));             // 216 = 200 + 16
            check(!o.block && o.vetoed, "a reversed tick is not held when the coarse clock says the pause was long");
            const Outcome n = f.wheel(-120, 80, at(1246));
            check(!n.block && !n.hasEmit, "and the stroke carries on in the new direction");
        }
        {
            Filter f;
            f.configure(c);
            f.wheel(120, 0, at(1000));
            const Outcome o = f.wheel(-120, 50, at(1215));
            check(o.block && !o.vetoed, "a coarse pause just under the window plus one clock step does not veto");
        }
    }
}

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: filter_tests <cases.txt>\n"); return 2; }
    if (runCaseFile(argv[1]) != 0) return 2;
    directChecks();
    printf("%d checks, %d failed\n", g_run, g_failed);
    return g_failed ? 1 : 0;
}
