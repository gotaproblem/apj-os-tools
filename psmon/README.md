# PSMON — PiStorm Monitor

CPU/JIT, memory and task monitor for PiStorm Atari JIT. One source, two
binaries with deliberately different jobs:

- **`PSMON.PRG`** — for **FreeMiNT**. CPU/JIT figures, memory, and a live task
  list.
- **`PSMON.ACC`** — for **GEM desktops, primarily EmuTOS**. CPU/JIT figures and
  memory only. No task list, and nothing that calls a GEMDOS function EmuTOS
  does not have.

## Build

```
make              # PSMON.PRG and PSMON.ACC
make clean
```

Same toolchain assumptions as `vidgem` / `mp3gem`: `MINTBIN=/opt/cross-mint/bin`,
`CROSS=m68k-atari-mint-`, `-O2 -Wall -m68000`, `-lgem`. Override on the command
line if yours lives elsewhere.

## Install

- `PSMON.PRG` — run from the Desktop under FreeMiNT.
- `PSMON.ACC` — copy to the boot drive root and reboot; appears in the Desk menu.

## The three halves, and what each needs

**Memory — needs nothing.** Totals come from the low-memory system variables
(`phystop`, `ramtop`, `ramvalid`) read under `Supexec`, free space from GEMDOS.
Works today against any TOS and any emulator.

**CPU/JIT — needs the `PSCTRL` NatFeat in the emulator.** That host side is
applied: `platforms/atari/psctrl.{h,cpp}` plus counters in
`compemu_support_arm.cpp`, cycle accumulators in `newcpu.cpp`, the 50 Hz
sampler call in `platform_atari_fdd.c`, and the feature itself in
`atari_natfeat.cpp`. Rebuild the emulator and Speed, JIT hit rate, cache usage
and the compile/flush/invalidate counters go live. Run against an emulator
without it, the app says so in the window and still shows everything else.

**Task list — needs FreeMiNT, and is in the PRG only.** Enumerated from
`u:\proc` with `Dopendir`/`Dxreaddir`, showing name, PID, memory and run state,
sorted by memory. Under EmuTOS `Dopendir` answers `EINVFN`, the app notices
once and stops asking, and the window says there is no task list.

## Things worth knowing

**The task list reads no kernel structures.** Everything comes from the proc
filesystem's own `XATTR`: `size` is the process's memory use, and `attr` is the
kernel's `p_attr[]` run-queue encoding —

| attr | state | attr | state |
|------|-------|------|-------|
| 0x00 | run | 0x22 | zombie |
| 0x01 | ready | 0x02 | tsr |
| 0x20 | wait | 0x24 | stop |
| 0x21 | io / select | | |

`Fcntl(PPROCADDR)` would give more, but it hands back a raw pointer into the
kernel's `PROC` struct whose layout changes between kernel versions. Not worth
it for a monitor.

**Speed is MIPS, not MHz, and that is deliberate.** `execute_normal()` charges
every instruction a flat `4 * CYCLE_UNIT` — the `adjust_cycles()` call above it
is commented out — and `compile_block()` inherits that same total. So the
emulator's cycle counter is *exactly* four times an instruction count, and
dividing it back out gives instructions retired per second with no modelling
assumption at all. Presenting it as "MHz" or "×ST" would mean asserting a
cycles-per-instruction figure the emulator does not model: a real 68000
averages nearer 8–10 once effective-address calculation and memory access are
counted, so those figures overstate real hardware by roughly 2–2.5×. They are
still readable at PSCTRL indices 33 and 34 if you want them; they are not on
screen. For an honest speed multiplier, benchmark — `cdev/coremark` has
`CM_68000.tos` built, and `jit_glue.cpp` already records a hardware baseline of
540→734 from `PISTORM_PISSOFF` tuning.

**No per-process CPU%.** The proc filesystem's timestamps are the process
*start* time, not accumulated CPU. Per-task CPU would mean parsing
`u:\kern\<pid>\stat`, which only exists if the kern filesystem is mounted.

**`Mxalloc` is detected, not version-sniffed.** GEMDOS answers `EINVFN` (-32)
for calls it does not implement, so the app tries `Mxalloc(-1, 0)` and falls
back to `Malloc(-1)`. More reliable than reading `os_version`, and
`Mxalloc(-1, …)` allocates nothing so the probe is free.

On TOS 1.x that means "free" is the **largest free block**, not the total —
`Malloc` offers nothing better, and walking the free list would mean allocating
everything and handing it back, which is far too intrusive for a monitor. The
window marks it with an asterisk rather than quietly showing a number that
means something other than it appears to.

**Redraws are skipped when nothing changed**, by comparing a signature of the
whole display against the previous sample. Under cooperative GEM, AES time is
scarce and a VDI redraw is not cheap.

**The task list scrolls.** The window is clamped to the desktop at open time,
and on a 640×400 ST screen that bites, so the PRG carries a real GEM vertical
slider with arrows — `WM_ARROWED` for line and page steps, `WM_VSLID` for the
thumb, with proportional `WF_VSLSIZE`. It replaces an earlier "... n more"
line, which was also a bug: it was drawn over the last task row in the same
pass and, being shorter than the row beneath it, left the tail of the previous
text on screen. Every field is blank-padded now, so a short state or name can't
leave a fragment of a longer one behind either. The ACC has no list and keeps
the plain window.

**The accessory stalls inside non-AES programs, by design.** Under single-TOS
GEM an accessory only runs when the foreground application calls the AES; in a
game that never does, the display freezes. Because the host samples on its own
wall-clock tick rather than at read time, the first redraw after a stall shows
*current* values rather than a stale delta — no garbage frame. Under MiNT it is
preemptive and behaves normally.

**`nf_probe` will not crash a real ST.** `$7300` is an illegal instruction on a
68000, so the probe installs a temporary vector-4 handler, and if the opcode
traps it steps the stacked PC past it and reports "unsupported". Worth lifting
into `vidgem.c` and `mp3gem.c`, which still describe the opcode as "harmless on
real HW".

## Not done

- **No `PS_SETINT`, so the app is read-only.** No JIT on/off, no manual flush.
  A NatFeat call runs *inside* a translated block, so anything reaching
  `flush_icache_hard()` would free the code the call is about to return into.
  Control needs the deferred-apply hook in `m68k_run_jit()` first —
  `PSCTRL-DESIGN.md` §4 in the emulator tree.
- **`WAIT` reads 0% and the row is marked with an asterisk.** The host stubs
  `wait_x100` to 0 and reports `IDLE_VALID == 0`, so this is the wait-loop
  heuristic rather than true `regs.stopped` accounting. `TODO-STOP-IDLE.md`
  explains why that distinction currently matters.
- **No graph/history.** `PS_STAT_EPOCH` plus the host-side ring buffer were
  specced so that can be added later with no API change.
- **No kill or renice.** Deliberate: this is a monitor.

## Status

Compiles clean in both configurations (`-Wall -Wextra`) against the real
gemlib and MiNTLib headers, and the inline `__asm__` block assembles as m68k
with the expected encodings. **Not yet run on hardware** — treat the first
launch as a test.
