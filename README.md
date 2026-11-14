# SteelSeries scroll fix

**fly-by-wire** is a small Windows tray program for a worn mouse:

- the **scroll wheel jumps the wrong way** now and then (you scroll down, the page twitches up)
- a **click fires twice** (the switch re-closes by itself a few milliseconds after you let go)

It sits between the mouse and every program, catches those two faults before anything sees them, and lets everything else through untouched. No driver, no install, no admin rights.

Built for a SteelSeries Rival 500 whose wheel encoder wore out. It works with any mouse.

## What it does to the wheel

A worn encoder misreads a step now and then. On the mouse this was built on, 84 of 1270 ticks (6.6%) came out the wrong way. Always a single tick, never two in a row, mostly in the middle of a stroke, 3 to 15 ms after the tick before. No hand turns a wheel round that fast.

- A tick against the stroke that comes within **70 ms** of the last one is held back.
- If the next tick agrees with it, you really turned the wheel round: both go through.
- If the stroke goes on the old way, the held tick was a misread step. It goes through **turned the right way round**, so you keep every notch. (A stroke with a wrong tick in it has as many ticks as one without: the wrong tick takes the place of a right one.)
- The first tick of a new stroke has nothing to be checked against, so it waits up to **40 ms** for the second. If the first two disagree, a third decides.

On that mouse, the same scroll tests before and after:

| Test | Ticks the mouse sent | Wrong way | Wrong way that reached programs |
|---|---|---|---|
| Scroll up, guard off | 634 | 51 | 51 |
| Scroll down, guard off | 636 | 33 | 33 |
| Scroll up, guard on | 280 | 18 | **0** |
| Scroll down, guard on | 218 | 12 | **0** |

## What it does to clicks

A press that comes less than **20 ms** after the release of the same button is the switch bouncing, not a finger. It is dropped, together with its release.

Double clicks by hand are never touched: the fastest double click measured from a hand left 30 ms between release and press, and games need them. The click test on the page counts a pair as bugged only by what a finger cannot do (a gap under 10 ms, or a press shorter than 20 ms), so your own fast clicking is never counted against the mouse.

An optional release delay per button (off by default) bridges a switch that opens for an instant in the middle of a drag.

## Use it

1. Download `fly-by-wire.exe` from [Releases](../../releases) and run it. It is not code-signed, so Windows SmartScreen may ask once: More info, Run anyway.
2. A tray icon appears (a filled disc while the guard is on, a ring while it is off) and the page opens: <http://127.0.0.1:8479>.
3. Pick your mouse in the list at the top of the page. Until you do, every mouse is guarded.
4. Run the three tests on the page if you want numbers: clicks, scroll up, scroll down. Runs are listed with the guard on or off, so you see before and after.
5. To have it start at login: right-click the tray icon, **Start with Windows**.

To switch the guard off: the Guard box on the page, the tray menu, **Ctrl+Alt+Shift+F11** from anywhere, or `fly-by-wire.exe --off` (from Win+R, for when the mouse is the problem). `--on` and `--quit` work the same way.

## What it does not touch

- **Input from other programs.** Macros from SteelSeries GG, Logitech G HUB, remote desktop tools: injected input is recorded but never judged.
- **Other mice.** Only the mouse you pick is judged. Raw input names the device of every click and tick, and the program looks that up for each event.
- **Anything in doubt.** It fails open: if the hook was late, the mouse could not be told, or the guard was just switched off, events pass as they came. The worst case is the mouse as it is without the program, never a button stuck down.

## Privacy

Everything stays on your PC. It records button and wheel timing (no cursor positions, no keys, no window titles) to `%LOCALAPPDATA%\fly-by-wire`, so the tests and the page have something to show. The page is served to 127.0.0.1 only and refuses requests from web sites. There is no other network use and no telemetry.

## Limits

- Games that read the mouse through raw input or DirectInput see it before any program like this can. The fix does not reach them. (Age of Empires II DE imports no raw input function, so it should be covered. Not yet tried in a match.)
- Windows that run as administrator are not covered unless fly-by-wire runs as administrator too.
- Settings were fitted on one mouse. If yours is worse, the Rules tab has every value, and `replay.exe` runs a recording through the real rules with other values before you change anything.

## Build from source

Needs Windows 10 or 11 and the free [Build Tools for Visual Studio](https://visualstudio.microsoft.com/downloads/) (2019 or newer) with "Desktop development with C++". No other dependencies.

```powershell
.\build.ps1
```

That runs the rule tests first (155 cases), then builds `build\fly-by-wire.exe`, `build\replay.exe` and the test tools.

Checks:

| Check | What it proves |
|---|---|
| `tests\cases.txt` via `build.ps1 -Only tests` | the rules give the right output for 155 event sequences, 84 of them from an independent reference model |
| `tests\mutation_check.ps1` | the tests can fail: 69 deliberately broken copies of the rules must each be caught |
| `build\selftest.exe` | the real hook on the real desktop does what the rules say (takes over the mouse for 20 seconds) |
| `tests\verify_recorder.ps1` | the recorder counts a scripted faulty mouse exactly (takes over the mouse for 40 seconds) |

## How it is put together

| File | What is in it |
|---|---|
| `src/filter.h` | the rules, plain C++ with no Windows in it, so they can be tested with made-up events |
| `src/engine.h` | the low level mouse hook, raw input, the thread that owns the rules |
| `src/whichmouse.h` | matching each event to the mouse that sent it |
| `src/store.h` | counts, guided tests, the recording |
| `src/main.cpp` | tray, settings, the local page server |
| `web/index.html` | the page |

Not affiliated with or endorsed by SteelSeries.

## License

MIT, see [LICENSE](LICENSE).
