/*
 * JITTEST - a differential correctness check for the PiStorm 68k JIT.
 *
 * The idea: the emulator has two ways to run 68k code - the JIT recompiler
 * and the plain interpreter - and the interpreter is the proven one. So we
 * do not need to know the "right" answer for any instruction: we run the
 * SAME battery of operations twice, once with the JIT on and once with it
 * off, and compare. Any difference is the JIT mistranslating something the
 * interpreter gets right.
 *
 *   1. Build JITTEST.PRG (see the Makefile) and put it on the Atari.
 *   2. Run it with the JIT ON  (normal).  It writes JITTEST.LOG and prints
 *      a single CRC.
 *   3. In PSCTRL set "JIT power" to 0 (cache off = interpreter), or launch
 *      with a .cfg that has the JIT disabled, and run it again. It writes
 *      JITTEST.INT and prints a CRC.
 *   4. Compare the two CRCs. Equal = the JIT agrees with the interpreter on
 *      every operation in the battery. Different = diff JITTEST.LOG against
 *      JITTEST.INT and the first differing line names the instruction,
 *      size, inputs, X-in, and the result/CCR each engine produced.
 *
 * Every test runs one instruction in register form (Dn OP Dn, or the unary
 * Dn form) with a controlled CCR-in, then reads the result and the CCR the
 * instruction left. Register form is deliberate: a mistranslation lives in
 * the instruction handler, not in the addressing mode, so the core ALU/flag
 * logic is what we hammer - with the operand values that sit on every flag
 * boundary (0, +/-1, the signed and unsigned min/max at each size).
 *
 * No memory operands, no divide-by-zero, no privileged ops: nothing here
 * takes an exception, so the two runs differ only where the JIT is wrong.
 */

#include <stdio.h>
#include <gem.h>

typedef unsigned long  u32;
typedef unsigned short u16;
typedef unsigned char  u8;

/* ---- operand ladders: the values that sit on flag boundaries ---------- */

static const u32 vals[] = {
	0x00000000UL, 0x00000001UL, 0x00000002UL,
	0x0000007FUL, 0x00000080UL, 0x000000FFUL,
	0x00007FFFUL, 0x00008000UL, 0x0000FFFFUL,
	0x7FFFFFFFUL, 0x80000000UL, 0xFFFFFFFFUL,
	0x55555555UL, 0xAAAAAAAAUL, 0x12345678UL, 0xDEADBEEFUL
};
#define NV ((int)(sizeof(vals)/sizeof(vals[0])))

/* shift/rotate counts to exercise: 0 and 1 are the special cases for the
 * V and C/X flags, 8/16/31/32 cross the size and the "count>=size" edge */
static const u8 counts[] = { 0, 1, 2, 7, 8, 15, 16, 31, 32 };
#define NC ((int)(sizeof(counts)/sizeof(counts[0])))

/* ---- the running check: a CRC over every (result, ccr) pair ----------- */

static u32 crc;
static FILE *log_fp;

static void fold(const char *tag, char sz, u32 a, u32 b, int xin, u32 res, u32 ccr)
{
	/* only the low 5 bits of CCR are defined (X N Z V C) */
	ccr &= 0x1F;
	crc = (crc * 16777619UL) ^ res;
	crc = (crc * 16777619UL) ^ (ccr | ((u32)xin << 8));
	if (log_fp)
		fprintf(log_fp, "%-6s.%c x%d  a=%08lX b=%08lX -> res=%08lX ccr=%02lX\n",
		        tag, sz, xin, (unsigned long)a, (unsigned long)b,
		        (unsigned long)res, (unsigned long)ccr);
}

/* set the CCR to X-in (bit4) and clear the rest, run BODY, capture result
 * (d0) and the CCR the instruction left. move ccr,Dn / move Dn,ccr are
 * 68010+ user-mode, which the 68040 target has. */
#define RUN(BODY, a_in, b_in, xin, resv, ccrv)                            \
	__asm__ volatile (                                                     \
		"move.l  %2,%%d0\n\t"                                              \
		"move.l  %3,%%d1\n\t"                                              \
		"moveq   #0,%%d2\n\t"                                              \
		"tst.l   %4\n\t"      /* xin != 0 ? set X in the CCR-in */         \
		"beq     0f\n\t"                                                   \
		"moveq   #0x10,%%d2\n"                                             \
		"0:\tmove    %%d2,%%ccr\n\t"                                       \
		BODY "\n\t"                                                        \
		"move.l  %%d0,%0\n\t"                                              \
		"move    %%ccr,%%d2\n\t"                                           \
		"move.l  %%d2,%1\n\t"                                              \
		: "=&d"(resv), "=&d"(ccrv)                                         \
		: "d"(a_in), "d"(b_in), "d"(xin)                                   \
		: "d0","d1","d2","cc")

/* binary op: d0 = d0 <op> d1, for one size */
#define BIN(tag, insn, sz)                                                 \
	do {                                                                   \
		int i,j; u32 r,c;                                                  \
		for (i=0;i<NV;i++) for (j=0;j<NV;j++) {                            \
			RUN(insn " %%d1,%%d0", vals[i], vals[j], 0, r, c);            \
			fold(tag, sz, vals[i], vals[j], 0, r, c);                     \
		}                                                                  \
	} while (0)

/* binary op that also consumes X (addx/subx): test X-in 0 and 1 */
#define BINX(tag, insn, sz)                                                \
	do {                                                                   \
		int i,j,x; u32 r,c;                                                \
		for (x=0;x<2;x++) for (i=0;i<NV;i++) for (j=0;j<NV;j++) {          \
			RUN(insn " %%d1,%%d0", vals[i], vals[j], x, r, c);            \
			fold(tag, sz, vals[i], vals[j], x, r, c);                     \
		}                                                                  \
	} while (0)

/* unary op: d0 = <op> d0 */
#define UN(tag, insn, sz)                                                  \
	do {                                                                   \
		int i; u32 r,c;                                                    \
		for (i=0;i<NV;i++) {                                               \
			RUN(insn " %%d0", vals[i], 0, 0, r, c);                       \
			fold(tag, sz, vals[i], 0, 0, r, c);                          \
		}                                                                  \
	} while (0)

/* unary op consuming X (negx): X-in 0 and 1 */
#define UNX(tag, insn, sz)                                                 \
	do {                                                                   \
		int i,x; u32 r,c;                                                  \
		for (x=0;x<2;x++) for (i=0;i<NV;i++) {                            \
			RUN(insn " %%d0", vals[i], 0, x, r, c);                       \
			fold(tag, sz, vals[i], 0, x, r, c);                          \
		}                                                                  \
	} while (0)

/* shift/rotate by a register count in d1: d0 = d0 <op> d1 */
#define SHIFT(tag, insn, sz)                                              \
	do {                                                                  \
		int i,k; u32 r,c;                                                 \
		for (i=0;i<NV;i++) for (k=0;k<NC;k++) {                           \
			RUN(insn " %%d1,%%d0", vals[i], counts[k], 0, r, c);         \
			fold(tag, sz, vals[i], counts[k], 0, r, c);                 \
		}                                                                 \
	} while (0)

/* division: d0 = d0 </ d1, skipping a zero divisor (that would trap) but
 * KEEPING the overflow case (quotient too big) - the classic flag-bug spot.
 * Word form uses d0.l / d1.w; long form (68020+) is 32/32. */
#define DIV(tag, insn, sz)                                               \
	do {                                                                 \
		int i,j; u32 r,c;                                                \
		for (i=0;i<NV;i++) for (j=0;j<NV;j++) {                          \
			if ((sz=='w' ? (vals[j] & 0xFFFF) : vals[j]) == 0) continue;\
			RUN(insn " %%d1,%%d0", vals[i], vals[j], 0, r, c);          \
			fold(tag, sz, vals[i], vals[j], 0, r, c);                  \
		}                                                                \
	} while (0)

/* rotate-through-X by a register count: X-in matters */
#define SHIFTX(tag, insn, sz)                                            \
	do {                                                                 \
		int i,k,x; u32 r,c;                                              \
		for (x=0;x<2;x++) for (i=0;i<NV;i++) for (k=0;k<NC;k++) {        \
			RUN(insn " %%d1,%%d0", vals[i], counts[k], x, r, c);        \
			fold(tag, sz, vals[i], counts[k], x, r, c);               \
		}                                                                \
	} while (0)

static void battery(void)
{
	/* --- add / sub / cmp, all three sizes --- */
	BIN ("add",  "add.b",  'b'); BIN ("add",  "add.w",  'w'); BIN ("add",  "add.l",  'l');
	BIN ("sub",  "sub.b",  'b'); BIN ("sub",  "sub.w",  'w'); BIN ("sub",  "sub.l",  'l');
	BIN ("cmp",  "cmp.b",  'b'); BIN ("cmp",  "cmp.w",  'w'); BIN ("cmp",  "cmp.l",  'l');
	BINX("addx", "addx.b", 'b'); BINX("addx", "addx.w", 'w'); BINX("addx", "addx.l", 'l');
	BINX("subx", "subx.b", 'b'); BINX("subx", "subx.w", 'w'); BINX("subx", "subx.l", 'l');

	/* --- logic --- */
	BIN ("and",  "and.b",  'b'); BIN ("and",  "and.w",  'w'); BIN ("and",  "and.l",  'l');
	BIN ("or",   "or.b",   'b'); BIN ("or",   "or.w",   'w'); BIN ("or",   "or.l",   'l');
	BIN ("eor",  "eor.b",  'b'); BIN ("eor",  "eor.w",  'w'); BIN ("eor",  "eor.l",  'l');

	/* --- unary --- */
	UN  ("neg",  "neg.b",  'b'); UN  ("neg",  "neg.w",  'w'); UN  ("neg",  "neg.l",  'l');
	UNX ("negx", "negx.b", 'b'); UNX ("negx", "negx.w", 'w'); UNX ("negx", "negx.l", 'l');
	UN  ("not",  "not.b",  'b'); UN  ("not",  "not.w",  'w'); UN  ("not",  "not.l",  'l');
	UN  ("tst",  "tst.b",  'b'); UN  ("tst",  "tst.w",  'w'); UN  ("tst",  "tst.l",  'l');
	UN  ("swap", "swap",   'l');
	UN  ("ext",  "ext.w",  'w'); UN  ("ext",  "ext.l",  'l');

	/* --- multiply (word source -> long) --- */
	BIN ("mulu", "mulu.w", 'w'); BIN ("muls", "muls.w", 'w');
	/* --- 32x32 multiply, 68020+ --- */
	BIN ("mulul","mulu.l", 'l'); BIN ("mulsl","muls.l", 'l');

	/* --- divide (nonzero divisor; the overflow case is kept) --- */
	DIV ("divu", "divu.w", 'w'); DIV ("divs", "divs.w", 'w');
	DIV ("divul","divu.l", 'l'); DIV ("divsl","divs.l", 'l');

	/* --- shifts / rotates by a register count --- */
	SHIFT ("asl",  "asl.b",  'b'); SHIFT ("asl",  "asl.w",  'w'); SHIFT ("asl",  "asl.l",  'l');
	SHIFT ("asr",  "asr.b",  'b'); SHIFT ("asr",  "asr.w",  'w'); SHIFT ("asr",  "asr.l",  'l');
	SHIFT ("lsl",  "lsl.b",  'b'); SHIFT ("lsl",  "lsl.w",  'w'); SHIFT ("lsl",  "lsl.l",  'l');
	SHIFT ("lsr",  "lsr.b",  'b'); SHIFT ("lsr",  "lsr.w",  'w'); SHIFT ("lsr",  "lsr.l",  'l');
	SHIFT ("rol",  "rol.b",  'b'); SHIFT ("rol",  "rol.w",  'w'); SHIFT ("rol",  "rol.l",  'l');
	SHIFT ("ror",  "ror.b",  'b'); SHIFT ("ror",  "ror.w",  'w'); SHIFT ("ror",  "ror.l",  'l');
	SHIFTX("roxl", "roxl.b", 'b'); SHIFTX("roxl", "roxl.w", 'w'); SHIFTX("roxl", "roxl.l", 'l');
	SHIFTX("roxr", "roxr.b", 'b'); SHIFTX("roxr", "roxr.w", 'w'); SHIFTX("roxr", "roxr.l", 'l');
}

int main(int argc, char **argv)
{
	const char *name = "JITTEST.LOG";
	char msg[160];
	int i;

	for (i = 1; i < argc; i++)
		if (argv[i][0] == '-' && argv[i][1] == 'o' && i + 1 < argc)
			name = argv[++i];

	crc = 2166136261UL;
	log_fp = fopen(name, "w");
	battery();
	if (log_fp) {
		fprintf(log_fp, "CRC=%08lX\n", (unsigned long)crc);
		fclose(log_fp);
	}

	/* Surface the CRC through GEM so it is visible when launched from the
	 * desktop, where stdout goes nowhere. The log file holds the detail. */
	if (appl_init() >= 0) {
		sprintf(msg, "[1][JITTEST battery|CRC = %08lX|log: %s|"
		             "|Run with JIT on, then jit_power=0,|and diff the "
		             "two logs.][ OK ]",
		        (unsigned long)crc, name);
		form_alert(1, msg);
		appl_exit();
	}
	printf("JITTEST battery CRC = %08lX\n", (unsigned long)crc);
	return 0;
}
