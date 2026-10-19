# Tick Saver (Geode mod for Geometry Dash 2.2)

Experimental tool to study input buffering / tick saving at 240 ticks/s.
F6 cycles the max shift: off, 1, 2, 4, 8, 16 ticks (rebindable in the mod's settings).

## Get the .geode file WITHOUT building anything locally
1. Create a free GitHub account, make a new repository, upload everything in this folder
   (including the hidden `.github` folder), commit to `main`.
2. Open the repo's **Actions** tab -> "Build Mod". It runs automatically on push (or press "Run workflow").
3. When it finishes (about 5-10 min), open the run and download the **Windows** artifact (or Android64, macOS, ...).
4. Unzip it and put `yourname.tick-saver.geode` into `Geometry Dash/geode/mods/`, or just double-click it.
   Requires Geode installed (geode-sdk.org).

Before pushing, edit `mod.json`: change `id` (`yourname.tick-saver`) and `developer`.

## Design decisions
* **Half ticks:** none. A tick = one `processCommands(dt)` call. An input belongs to the tick whose
  `processCommands` consumed it (`handleButton` fires inside it). Snapshots are taken at the START of a
  tick and store that tick's dt.
* **Shift order:** +1, -1, +2, -2 ... +-N applied to the most recent logged input.
* **Survive test:** ~100 ms (24 ticks at 240 TPS) beyond the death tick.
* **Ring depth:** 2N+4 snapshots. A shift is only tried if a snapshot at or before the
  earlier of (original, shifted) input tick still exists, so only recent inputs can be saved.
* **Anti-abuse:** if a save was used in an attempt, completing it is forced into test mode (nothing is submitted).
  The anticheat spike is never swallowed.
* **Cost:** one checkpoint per tick, plus up to 2N trial replays on each death. Expect a small hitch at N=16.

## If CI fails to compile
This was written against the Geode docs but not compiled. The most likely spots:
* `m_queuedButtons`, `m_anticheatSpike`, `m_isTestMode`, `m_isDead` field names (check the Geode bindings for your GD version)
* keybind setting default format in `mod.json` and the `listenForKeybindSettingPresses` callback signature
* `mod.json` `"geode"` must match the SDK version CI uses
Paste the red error lines from the Actions log and it's a quick fix.
