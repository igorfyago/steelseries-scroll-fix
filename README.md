# SteelSeries scroll fix

**fly-by-wire** is a small Windows tray program for a worn mouse:

- the **scroll wheel jumps the wrong way** now and then (you scroll down, the page twitches up)
- a **click fires twice**, or a press is cut short (the switch bounces when it closes or opens)

It sits between the mouse and every program, catches those faults before programs see them, and lets everything else through untouched. No driver, no install, no admin rights. Built for a SteelSeries Rival 500 whose wheel encoder wore out. It works with any mouse.

## What it does to the wheel

A worn encoder misreads a step now and then. On the mouse this was built on, 3 to 7% of the ticks came out the wrong way when the wheel was turned fast, and 8 to 12% when it was turned slowly, notch by notch. Every misread was a single tick: 204 of 204 in the scroll tests, never two in a row. Turned slowly, they come at the same spacing as the right ticks, so no time window can tell them from a turn of your hand. The next tick can.

So the guard remembers which way you scroll, however long ago the last tick was. A tick the other way is held until the next tick says what it was:

- **The next tick goes the same new way:** you turned the wheel. Both go through, and the new direction is the one remembered.
- **The next tick goes the old way:** the held tick was a misread.
- **Nothing follows within a second:** the held tick is dropped and the direction stays. One that came less than **70 ms** after the last tick is a misread for certain (no hand turns a wheel that fast) and is settled after 0.2 s.

The price: a single notch back and forth on its own is lost, and a turn of the wheel needs two notches the new way. What happens to a misread depends on one switch, **Send corrected ticks**:

| | Off (the default) | On |
|---|---|---|
| A misread tick | dropped | turned the right way round, so you keep every notch |
| A turn of the wheel | its first notch is lost | its first notch arrives with the second |
| The first tick after a pause | passes at once | waits up to 40 ms for the second; if the first two disagree, a third decides |
| Programs that read raw input (some games) | see the mouse exactly as it is | also get the ticks this program sends, so some ticks twice |

Why the second column exists: a stroke with a wrong tick in it has as many ticks as a stroke without one. The wrong tick takes the place of a right one, so turning it round is the exact repair.

Why it is off by default: Windows hands every tick to programs that read raw input before any program like this can hold it back (measured: a tick blocked by the hook still reaches raw input). Sending ticks is safe for browsers, Windows itself and most applications, and for games that read ordinary mouse messages (Age of Empires II DE imports no raw input function). For a game that reads raw input it adds ticks. If your games are not of that kind, switch it on.

All of one day's scroll tests on that mouse (14 runs, 4100 ticks, fast and slow), run back through the rules with `replay.exe`:

| | Wrong ticks reaching programs |
|---|---|
| No guard | 317 |
| Guard, default | 20, a notch lost for each misread |
| Guard, Send corrected ticks | 25, every notch kept |

All that is left comes from one run, stopped early, where the wheel went the other way 5 to 8 ticks at a time. Nothing that goes by single misreads can tell that from a turn of the hand. The other 13 runs: 0 wrong ticks in both modes.

## What it does to clicks

- A press that comes less than **20 ms** after the release of the same button is the switch bouncing open, not a finger. It is dropped, together with its release.
- A press that lasts less than **20 ms** is the switch bouncing on the way down (the shortest press measured from a hand was 40 ms). Its release is held for 20 ms, and a re-press inside that time continues the same press. Without this, a drag that starts with such a bounce is cut to a click of a few milliseconds.
- Releases of real presses are never held, so normal clicks are never delayed. Double clicks by hand are never touched: the fastest measured from a hand left 30 ms between release and press, and games need them.
- An optional release delay per button (off by default) bridges a switch that opens for an instant in the middle of a drag.

## Use it

1. Download `fly-by-wire.exe` from [Releases](../../releases) and run it. It is not code-signed, so Windows SmartScreen may ask once: More info, Run anyway.
2. A tray icon appears (a filled disc while the guard is on, a ring while it is off) and the page opens: <http://127.0.0.1:8479> (the next free port if that one is taken).
3. Pick your mouse in the list at the top of the page. Until you do, every mouse is guarded.
4. On the Rules tab, switch on **Send corrected ticks** if your programs allow it (see above).
5. Run the three tests on the page if you want numbers: clicks, scroll up, scroll down. Runs are listed with the guard on or off, so you see before and after.
6. To have it start at login: right-click the tray icon, **Start with Windows**.

To switch the guard off: the Guard box on the page, the tray menu, or **Ctrl+Alt+Shift+F11** from anywhere (it only switches off). From the Run box or a command line, give the full path: `"C:\path\to\fly-by-wire.exe" --off`. `--on` and `--quit` work the same way. Starting the exe again opens the page of the copy that runs.

## What it does not touch

- **Input from other programs.** Anything a program injects (remote desktop tools, AutoHotkey, macro software that plays its macros that way) is recorded but never judged. Macros that a mouse or its driver plays by itself look exactly like the mouse, so the same rules apply to them: a click again within 20 ms is dropped, and a single wheel tick against the last direction waits for the next one.
- **Other mice.** With a mouse picked, only that mouse is judged. Raw input names the device of every click and tick, and the program looks that up for each event; an event whose mouse cannot be told passes untouched. With "every mouse" everything is judged.
- **Late events.** If the program itself was kept waiting so long that Windows delivered an event without it, that event passes as it came.
- **Releases of real presses.** With the default settings the only release ever held back is that of a press shorter than 20 ms, for 20 ms. Switching the guard off, or quitting, sends anything still held.

## Privacy

Everything stays on your PC. While it runs it records the timing of every button press and wheel tick (no cursor positions, no keys, no window titles) to `%LOCALAPPDATA%\fly-by-wire`, so the tests, the page and the replay tool have data to work with. Recordings older than 14 days are deleted; move a file into the `keep` folder there to keep it. The page is served to 127.0.0.1 only and refuses requests from web sites. There is no other network use and no telemetry.

## Limits

- Programs that read raw input see every tick and click as the mouse sent it. Nothing that runs above the mouse driver can change that.
- Windows that run as administrator are not covered unless fly-by-wire runs as administrator too.
- With the buttons swapped in Windows settings (left handed), the release delay and the press bounce bridge stay off.
- Settings were fitted on one mouse. If yours is worse, the Rules tab has every value, and `replay.exe` runs a recording through the real rules with other values before you change anything.

## Build from source

Needs 64-bit Windows 10 or 11 and the free [Build Tools for Visual Studio](https://visualstudio.microsoft.com/downloads/) (2019 or newer) with "Desktop development with C++". No other dependencies.

```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1
```

That runs the rule tests first, then builds `build\fly-by-wire.exe`, `build\replay.exe` and the test tools.

Checks:

| Check | What it proves |
|---|---|
| `tests\cases.txt` via `build.ps1 -Only tests` | the rules give the right output for 185 event sequences; 84 of them come from an independent reference model of the click rules and of the wheel rule without the remembered direction (four differ on purpose, with the reason written next to them) |
| `tests\mutation_check.ps1` | the tests can fail: every deliberately broken copy of the rules, the mouse matching and the click pair definition must be caught |
| `build\selftest.exe` | the real hook on the real desktop does what the rules say (takes over the mouse for 30 seconds) |
| `tests\verify_recorder.ps1` | the recorder counts a scripted faulty mouse exactly (takes over the mouse for a minute) |

## How it is put together

| File | What is in it |
|---|---|
| `src/filter.h` | the rules, plain C++ with no Windows in it, so they can be tested with made-up events |
| `src/engine.h` | the low level mouse hook, raw input, the thread that owns the rules |
| `src/whichmouse.h` | matching each event to the mouse that sent it |
| `src/pairs.h` | when a quick pair of clicks counts against the mouse in the click test |
| `src/store.h` | counts, guided tests, the recording |
| `src/main.cpp` | tray, settings, the local page server |
| `src/replay.cpp` | runs a recording through the rules with other values |
| `web/index.html` | the page |

Not affiliated with or endorsed by SteelSeries.

## License

MIT, see [LICENSE](LICENSE).
