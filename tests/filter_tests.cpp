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

#include "filter.h"   // found through /I, so the mutation check can swap it

using namespace fbw;

struct Ev {
    double t = 0;
    bool   wheel = false;
    int    button = 0;
    bool   down = false;
    int    delta = 0;
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
    if (e.wheel) snprintf(buf, sizeof(buf), "%g wheel %+d", e.t, e.delta);
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
        std::string what, arg;
        if (!(is >> e.t >> what >> arg)) { *err = "cannot read event: " + item; return false; }
        if (what == "wheel") {
            e.wheel = true;
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
    c->holdGuard = false;
    c->wheelGuard = false;
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
        else if (k == "hold") { c->holdGuard = v > 0; c->holdMs = v; }
        else if (k == "wheel") c->wheelGuard = v != 0;
        else if (k == "reverse") c->reverseMs = v;
        else if (k == "confirm") c->confirmMs = v;
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
// work first, in deadline order, then the event itself.
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
        const Outcome o = e.wheel ? f.wheel(e.delta, e.t) : f.button(e.button, e.down, e.t);
        if (!o.block) out.push_back(e);
        if (o.hasEmit) out.push_back(fromEmit(o.emit, e.t));
    }
    runUntil(std::numeric_limits<double>::max());
    return out;
}

static bool same(const std::vector<Ev>& a, const std::vector<Ev>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::fabs(a[i].t - b[i].t) > 1e-6 || a[i].wheel != b[i].wheel) return false;
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

static void directChecks() {
    // flush sends a held release so no button stays down for applications
    {
        Filter f;
        Config c;
        c.clickGuard = false; c.holdGuard = true; c.holdMs = 25; c.wheelGuard = false;
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
    // busy while a press is hidden, so the hook is not swapped in between
    {
        Filter f;
        Config c;
        c.clickGuard = true; c.holdGuard = false; c.wheelGuard = false;
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
    // tick reports what it settled
    {
        Filter f;
        Config c;
        c.clickGuard = false; c.holdGuard = false; c.wheelGuard = true; c.reverseMs = 150; c.confirmMs = 200;
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
        Config c;
        c.clickGuard = true; c.holdGuard = true; c.holdMs = 25; c.wheelGuard = false;
        f.configure(c);
        f.button(Left, true, 0);
        f.button(Left, false, 50);          // held
        Emit em;
        while (f.flush(&em)) {}
        Config off;
        off.clickGuard = false; off.holdGuard = false; off.wheelGuard = false;
        f.configure(off);
        const Outcome o = f.button(Left, true, 55);
        check(!o.block, "after switching off, the next press passes");
        const Outcome r = f.button(Left, false, 110);
        check(!r.block, "and so does its release");
    }
}

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: filter_tests <cases.txt>\n"); return 2; }
    if (runCaseFile(argv[1]) != 0) return 2;
    directChecks();
    printf("%d checks, %d failed\n", g_run, g_failed);
    return g_failed ? 1 : 0;
}
