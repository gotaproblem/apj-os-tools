# JITTEST - differential correctness check for the PiStorm 68k JIT

## What it is

The emulator can run 68k code two ways: the JIT recompiler and the plain
interpreter. The interpreter is the proven one. JITTEST does not need to
know the right answer for any instruction - it runs the same battery of
~13,000 flag-stressing operations twice, once with the JIT on and once
with it off, and you compare. Any difference is the JIT mistranslating
something the interpreter gets right.

It is the correctness companion to the static coverage map (see
`hostpatches/jit-interpreted-opcodes.csv`): the coverage map says which
instructions the JIT *compiles*, JITTEST says whether it compiles them
*correctly*.

## The battery

Each test runs one instruction in register form (`Dn OP Dn`, or the unary
`Dn` form) with a controlled X-flag input, then reads back the result and
the CCR the instruction left. Register form is deliberate: a mistranslation
lives in the instruction handler, not the addressing mode, so this hammers
the core ALU/flag logic - with the operand values that sit on every flag
boundary (0, +/-1, signed and unsigned min/max at byte/word/long).

Covered: add/sub/cmp/addx/subx, and/or/eor, neg/negx/not/tst/swap/ext,
mulu/muls (word and 68020+ long), divu/divs (word and long, nonzero
divisors, overflow case kept), and the full shift/rotate family incl.
roxl/roxr through the X bit. No memory operands, no divide-by-zero, no
privileged ops - nothing here takes an exception, so the two runs differ
only where the JIT is wrong.

## How to run it

1. `make` and copy `JITTEST.PRG` to the Atari.
2. Run it with the **JIT on** (normal). It writes `JITTEST.LOG` and shows
   a CRC in an alert box.
3. Rename that log (e.g. to `JIT.LOG`), then turn the **JIT off**: in
   PSCTRL set *JIT power* to 0 (cache off = interpreter), or launch with a
   .cfg that disables the JIT. Run again - it writes a fresh `JITTEST.LOG`
   and shows a CRC.
   - Or pass an output name: `JITTEST.PRG -o INT.LOG`.
4. **Equal CRCs** = the JIT agrees with the interpreter on every operation
   in the battery. **Different** = `diff` the two logs; the first differing
   line names the instruction, size, inputs and X-in, and the result/CCR
   each engine produced. That line is the bug.

The CRC folds in the result and the low 5 CCR bits (X N Z V C) of every
test, so a wrong flag is caught as surely as a wrong result.

## Extending it

Add a line to `battery()`. `BIN`/`UN`/`SHIFT` (+ the `X` and `DIV`
variants) take a tag, the instruction mnemonic with size, and a size
letter. Keep to non-faulting, register-form instructions so the two runs
stay comparable.
