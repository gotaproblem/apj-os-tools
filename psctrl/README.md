# PSCTRL — PiSTorm settings

`PSCTRL.ACC` / `PSCTRL.PRG`. Every configurable switch and tunable the
emulator has, in one skinned window, changing the **running** machine
through the PSCTRL NatFeat.

Built on APJSKIN, the same bitmap-skin engine as MP3GEM, so it follows the
desktop's Fluent theme and comes in the same three scales.

```
make                 PSCTRL.ACC and PSCTRL.PRG
```

Install `PSCTRL.ACC` in the root of the boot drive. The `.SKN` files live
in `S:\APJ-OS\NATFEATS\SKINS\` with the rest of the PiSTorm tools, which
is where the engine looks by default. Without a skin it still runs, on the
flat `apjgui` controls, and says which file it wanted and where it looked.

**Why the skins folder needs saying out loud for an accessory.** A `.PRG`
sits with the other tools, so `<progdir>SKINS\` finds the sheets beside
it and nothing has to be configured. An `.ACC` is loaded from the **root
of the boot drive**, so its progdir is `C:\` and it will never see them.
Hence the built-in `S:\APJ-OS\NATFEATS\SKINS\` in the search path, and
the override:

    C:\PSCTRL.INF
    scale=125
    skins=S:\APJ-OS\NATFEATS\SKINS

`skins=` is only needed if the sheets are somewhere other than the two
built-in locations. It is preserved when the 1/2/3 keys rewrite the scale.

## Why an accessory

An `.ACC` is in the drop-down of every application under TOS and XaAES and
can be opened while a game or the ST Box is running. That is the whole
point: `jit_power` is worth moving against a game you are actually
playing, not between runs. The `.PRG` is the same program for the desktop.

## It is self-describing

Nothing about the controls is compiled into this program. At startup it
asks the host for `PS_COUNT` and then `PS_DESCRIBE` for each setting, and
builds its tabs from the answers. Adding a switch to the emulator is one
line in `platforms/atari/psctrl/psctrl_settings.cpp` and **no rebuild of
this**.

A descriptor carries: the `.cfg` key, the title, the tab, the kind
(bool / enum / int / string / action / readout), the apply class, the
range and step, the unit, the enum labels, and the current value. The
window turns a kind into a control and a class into a badge:

| kind | control |
|---|---|
| bool | a checkbox |
| enum, ≤ 4 choices whose labels fit | radios |
| enum, otherwise | a popup with a drop-down list |
| int | a slider with the value beside it |
| string | a field; clicking it lists the host's images, or opens the file selector |
| action | a button |
| readout | a right-aligned value, refreshed twice a second |

The design doc says radios for binaries. A checkbox is used instead
because the Debug tab alone has a dozen booleans and a row of two radios
each is a wall; and the choice count is a ceiling rather than the rule for
enums, because three radios reading *off / real chip / emulated* in a
180 pt column truncate to `rea...` and `emu...`, which is worse than a
popup.

## The badge is not decoration

| badge | meaning |
|---|---|
| **live** | the host re-reads it; the change is in effect before the mouse comes up |
| **defer** | parked, and applied at the next JIT block boundary (anything that reallocates or flushes the translation cache) |
| **boot** | the machine's shape; written to a shadow config, applied by a restart |
| **box** | takes effect the next time the ST Box starts |

`PS_SETINT` enforces the class and answers which one happened; the status
line says so in words. A settings box that shows the same tick for "done"
and "needs a restart" is lying, and this one does not.

## Keys

| key | |
|---|---|
| ← → | previous / next tab |
| ↑ ↓ PgUp PgDn | scroll (the PiSTorm's USB bridge sends wheel clicks as cursor keys) |
| 1 / 2 / 3 | skin scale 100% / 125% / 175%, remembered in `PSCTRL.INF` |
| 0 | back to the screen's own choice |
| B | run the benchmark |
| S | save to the `.cfg` |
| Esc / Q | close |

## The benchmark

On the JIT tab, and it runs as 68k code inside the accessory, so it
measures what programs get on the settings in force **now**. Move
`jit_power`, run it again, compare. About four seconds.

* **Dhrystone 2.1** is the headline: it is the one figure with a published
  Atari reference — 1400 Dhry/s (≈0.8 DMIPS) for a stock 1040 at 8 MHz,
  built with the Vincent Rivière GCC 4.6.4 port (FireBee Dhrystone table).
  "×N vs 8 MHz ST" divides by that.
* `dhry.h`, `dhry_1.c` and `dhry_2.c` are Weicker's published 2.1 source.
  The **only** change is that `main()`'s stdio-and-`times()` harness is
  wrapped in `#ifndef DHRY_EMBEDDED` and a pair of entry points added
  below it; the measurement loop and every `Proc_`/`Func_` body are byte
  for byte the published ones. The loop in `dhry_run()` is a verbatim copy
  of the one in `main()`, which is why it appears twice in the file.
* Its self-check runs too. If any final value is not what the published
  program says it should be, the panel says so instead of printing a
  suspiciously good score — a JIT that mistranslates something shows up
  here.
* The compile flags are fixed in the Makefile (`-O2 -fomit-frame-pointer`)
  and must not be tidied: changing them invalidates every number the tab
  has ever printed.
* The first pass is thrown away. That is the pass in which the JIT
  compiles the benchmark, and counting the compile in the score is how you
  get a number that improves every time you press the button.

Under it, assembly loops that say *where* a change came from:

| figure | loop | tells you |
|---|---|---|
| MIPS mixed | 64 instructions: ALU, a taken and a not-taken branch, a load and a store through ST-RAM, jsr/rts, movem | the number to watch; closest to a game's inner loop, and the one `jit_power` visibly moves |
| MIPS ALU | register-only add/sub/lsl/eor | raw translation quality |
| MIPS ST-RAM | `move.l (a0)+,(a1)+` over a 32 KB ST-RAM buffer | the bus path: stram_cache, stram_direct, TT-RAM placement |
| MFLOPS | an `fmul`/`fadd` chain, only when the host reports an FPU | `compfpu` and the FPU model |
| BogoMIPS | `dbra` and nothing else | absurd on purpose: the JIT folds it away, so read it as a ceiling, never as a speed |

Timing is the 200 Hz counter at `$4BA` through `Supexec` — the only clock a
GEM program can read on plain TOS as well as MiNT, and driven by the MFP
rather than by anything the JIT does. Every loop is calibrated first, so a
machine four times faster still measures for the same 0.4 s rather than
finishing before the clock ticks.

## Files

```
psctrl.c        the shell: NatFeat, the model, the event loop, both builds
psui.h/.c       the window - layout, drawing, hit-testing; no NatFeat calls
psbench.h/.c    the benchmark and its assembly loops
psbench_fpu.c   the MFLOPS loop, its own TU because it needs -m68881
dhry.h dhry_1.c dhry_2.c   Dhrystone 2.1 (see above)
```

`tests/psctrl/run.sh` builds `psui.c` against a stub VDI and checks every
blit on every tab; `skins/preview.py --psctrl <sheet> out.png [tab]`
renders the same window in Python from the `.SKN` alone, as an independent
reference.

## Host side

Needs the settings sub-ops in the emulator — `platforms/atari/psctrl/`
`psctrl_settings.cpp` and `psctrl_tunables.c`, and the deferred-apply hook
in `m68k_run_jit()`. They are in `../hostpatches/` as a numbered series.
Without them the window opens and says which half of PSCTRL is missing:
`PSMON`'s read-only status has shipped for a while, the settings sub-ops
are new.
