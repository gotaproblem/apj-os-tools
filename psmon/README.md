# PSMON — PiSTorm Monitor

`PSMON.ACC` / `PSMON.PRG`. The live JIT engine figures, guest memory, and
the state of the Pi underneath, in one skinned window.

```
make            # both
make PSMON.ACC
```

Install: `PSMON.ACC` in the **root of the boot drive** (that is where GEM
loads accessories from), `*.SKN` where the other tools find them.

## What it shows

The reference is the **taskbar's JIT panel** — TeraDesk `pstask.c`,
`mn_*`. Same six engine figures, same status indices, same arithmetic:

| row | from | note |
|---|---|---|
| Speed | `PS_JIT_EFF_KHZ` | MHz, and the multiple of an 8 MHz ST |
| JIT hit | `PS_JIT_HITRATE_X10` | cycle-weighted, tenths of a percent |
| Idle | `PS_JIT_IDLE_X10` | true STOP-state share of the clock |
| Cache | `PS_STAT_CACHE_USED` / `_TOTAL`, `_FLUSHES_TOTAL` | |
| Compile | `PS_STAT_COMPILES` | ×2: the sampler's window is 500 ms |
| SMC inv | `PS_STAT_SMC_INV` | ×2, same reason |

Then the two memory gauges, which need nothing from the emulator: totals
from the low-memory system variables under `Supexec()`, free space from
`Mxalloc(-1)` (or `Malloc(-1)` on a TOS that has no `Mxalloc`). If TOS did
not validate `ramtop`, the emulator's configured TT-RAM size is believed
instead — "0 free of 128.0 MB" is worth seeing, because it means a TOS
that is not initialising Fast RAM.

**One block the taskbar has no room for**: the Pi itself — board and RAM,
ARM clock and SoC temperature, and the firmware throttle bits. That last
row earns its place. A board that is thermally capped or browning out runs
the JIT slower, and nothing else on the Atari side can tell you that is
what happened; "ok now (has throttled)" explains a figure that was worse
five minutes ago.

**What it no longer shows**: the `u:\proc` task list. The XaAES task
manager does that job properly, and the JIT panel dropped it for the same
reason.

## Absent readings

Every reading can be absent. The host answers `0xFFFFFFFF` for an index it
does not know, and an older emulator does not know all of these, so each
row draws `n/a` rather than a plausible zero.

This is not hypothetical. The **previous** PSMON asked for indices 33..47
— `MHZ_X100`, `HITRATE_X100`, `SPEEDX_X100` and the rest — most of which
the emulator has since retired, and it drew a missing index as `0`. A
stale binary reported a confident `0.00 MHz`.

## Flicker

A monitor is the program most likely to make the rest of the desktop
flicker, so every rule PSCTRL learned on hardware is built in here from
the start:

* the 500 ms poll compares the **rendered lines**, not the numbers — a
  figure that moves without changing its string costs nothing;
* it repaints the **section** whose text changed, not the window;
* the screen lock (`wind_update`) is taken once per change however many
  rectangles it covers;
* `graf_mouse(M_OFF)` repaints whatever is under the pointer — the
  taskbar, if that is where it is resting — so the pointer is hidden only
  when it is actually over what is being drawn.

## PSMON.INF

Next to the `.PRG`; for the `.ACC`, in the root of the boot drive, because
that is where an accessory is loaded from. (`shel_read()` reports the
*running application's* folder, not the accessory's, which is how PSCTRL's
INF ended up beside whatever was in the foreground.)

```
scale=125
skins=C:\GEMSYS\SKINS
```

`1` / `2` / `3` set 100% / 125% / 175% and write the file; `0` follows the
desktop.

## Testing

`sh tests/psmon/run.sh [file.SKN]` builds `apjskin.c` and `psmonui.c`
against a fake VDI and a fake 1920×1080 32-bit screen, and draws four
models: normal, every reading absent, the widest string each row can
produce, and no emulator at all. It checks the usual blit rules, that no
**text** leaves the window or its own section box, that no line formats to
an empty value, and that repainting one section costs under half a full
window — which is the property that keeps the poll quiet.
