// pairs.h : when a quick pair of clicks counts against the mouse. Shared by the
// recorder's click test and by the replay tool, so the two cannot disagree.
//
// Not by the gap alone. Clicking fast, the hand this was measured on left as
// little as 30 ms between release and press, and double clicks by hand are
// needed in games, so they must never be counted as the fault. What a finger
// does not do is hold the button for under 20 ms (the shortest of 600 clicks
// was 40) or press again within 10 ms of letting go. A switch that fires by
// itself does both. So a pair is bugged when its gap is under
// kImpossibleGapMs, or when one of its two presses lasted under kBlipMs. Only
// pairs closer than kSuspectGapMs are looked at.
#pragma once

namespace fbw {

constexpr double kSuspectGapMs = 60;
constexpr double kImpossibleGapMs = 10;
constexpr double kBlipMs = 20;
// Presses up to this long after a release are listed as double clicks by hand.
constexpr double kByHandMs = 500;

// gapMs: release of the first click to press of the second. The holds are how
// long each of the two presses lasted, -1 when not known.
inline bool buggedPair(double gapMs, double holdBeforeMs, double holdAfterMs) {
    if (gapMs < 0 || gapMs >= kSuspectGapMs) return false;
    if (gapMs < kImpossibleGapMs) return true;
    return (holdBeforeMs >= 0 && holdBeforeMs < kBlipMs) || (holdAfterMs >= 0 && holdAfterMs < kBlipMs);
}

}  // namespace fbw
