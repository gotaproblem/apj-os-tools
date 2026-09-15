# CoreMark in PSCTRL — what was and was not changed

CoreMark is EEMBC's. It is here because it is the one figure on the
Benchmark tab that can be compared with **other people's Atari machines**
— there are published Falcon, TT and CT60 scores — and that comparison is
only legitimate if the rules are followed. They were.

## The files

| file | whose | state |
|---|---|---|
| `core_main.c` `core_list_join.c` `core_matrix.c` `core_state.c` `core_util.c` | EEMBC | **byte for byte upstream**, each matching its published md5 |
| `coremark.h` | EEMBC | byte for byte upstream |
| `coremark.md5` `LICENSE.md` `README.md` | EEMBC | upstream |
| `core_portme.c` `core_portme.h` | ours | the porting layer, which the rules exist to allow |
| `cm_shared.c` `cm_shared.h` | ours | the capture buffer and the clock |

`coremark.md5` fails for `coremark.h`. **That is stale upstream** — a
freshly cloned `eembc/coremark` fails the same check on the same file.
The five `.c` files, which are what the rules protect, all pass, and
`tests/coremark/run.sh` verifies that on every run.

## What the run rules allow, and what we used

Quoting the upstream README: what may change is *the iteration count, the
toolchain and build options, memory acquisition, the seed source,
`core_portme.c`, `core_portme.h` and `core_portme.mak`.* Prohibited is
*"changing of source file other then `core_portme*`"*.

* **Iterations: 0** — CoreMark then sizes its own run to land between 10
  and 100 seconds. The 10 second minimum is a rule, not a suggestion: a
  shorter run is not reportable, and the tab says so when it happens.
* **`main` renamed** with `-Dmain=cm_run_000` / `-Dmain=cm_run_020`. A
  build option, not a source change — this runs inside an accessory that
  has a `main` of its own.
* **Seeds: `SEED_VOLATILE`, `PERFORMANCE_RUN`** — 0, 0, 0x66, 2000-byte
  buffer. The reportable configuration.
* **Memory: `MEM_STACK`**, so the compliance line ends `/ STACK`.
* **Clock: the 200 Hz counter at `$4BA`** through `Supexec`. Coarse, but
  it cannot be out by more than 5 ms in ten seconds — one part in two
  thousand, well inside the run-to-run spread of a JIT.
* **`ee_printf` is ours** and captures instead of printing; there is no
  console in a desk accessory.

## Two builds, on purpose

CoreMark is compiled **twice**, `-m68000` and `-m68020-60`, and the
Benchmark tab lets you pick. Same benchmark, same machine, different
instruction set — which is the only way to see what the ISA is actually
worth as opposed to what the clock is worth. The 020 build is not offered
on a CPU that cannot execute it.

The two copies coexist without touching a line of EEMBC's code: each set
is combined with `ld -r`, then `objcopy -G <entry>` makes every other
defined symbol local. Undefined references (libgcc, `memset`) are left
alone, which is why it works. `cm_shared.c` is compiled once and holds
the one capture buffer and the one clock both variants report into.

## Reading a score

The tab shows the compliance line CoreMark itself printed, verbatim:

```
CoreMark 1.0 : 12.40 / GCC4.6.4 -O2 -m68020-60 / STACK
```

That is the form the rules require and the form to quote. Two things
invalidate it and the tab says both out loud:

* **"FAILED ITS OWN CHECKS"** — CoreMark validates four CRCs against
  known values. A failure means the score is *wrong*, not slow. Do not
  quote it; something in the JIT or the build is miscompiling.
* **"under the 10 s minimum"** — real, but not reportable.

The number PSCTRL displays is computed from the integer tick and
iteration counts rather than read out of CoreMark's own printed float, so
soft-float rounding cannot move it.

## Licence

Two, both in `LICENSE.md`:

* **Apache 2.0** for the code.
* **the CoreMark® Acceptable Use Agreement** — a trademark licence from
  EEMBC governing the name and how scores are reported. Keeping to the
  run rules above, and quoting the compliance line as printed, is what it
  asks for. `LICENSE.md` ships with the source here; do not remove it.

## Testing

`sh tests/coremark/run.sh` builds this port on the build machine and runs
it. The score there is meaningless; the **CRCs** are the point. They only
come out right if the port's data types are exactly the widths the rules
specify — `long` is 32 bits on m68k and 64 on the build host, and using
it for `ee_u32` changed the list and state CRCs and turned the run into
"Errors detected". That is precisely the class of mistake worth catching
before it reaches the Atari.
