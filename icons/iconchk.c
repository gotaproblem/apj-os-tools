/*
 * iconchk - check a TeraDesk cicons.rsc against an apjicons-NN.bin the
 * way XaAES's render_apj does: FNV-1a over each colour icon's mono
 * mask+data, then look that hash up in the bin. Prints, per icon:
 *   name  WxH  HASH  IN BIN / MISSING
 * so we can see exactly which icons XaAES can and cannot replace, and
 * whether the two files agree at all.
 *
 * Pure GEMDOS/stdio, no AES - parses the .rsc itself (same layout the
 * Python tooling uses). Build on the Mac:
 *   m68k-atari-mint-gcc -O2 -o ICONCHK.PRG iconchk.c
 * Run from a console (or double-click; output also goes to ICONCHK.TXT
 * beside the program's working dir and to U:\RAM\ICONCHK.TXT):
 *   ICONCHK.PRG  [cicons.rsc]  [apjicons-32.bin]
 * Defaults: CICONS.RSC and APJICONS-32.BIN in the current directory.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned long  u32;

static u8 *slurp(const char *path, long *len)
{
	FILE *f = fopen(path, "rb");
	u8 *b;
	long n;

	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	b = malloc(n);
	if (!b) { fclose(f); return NULL; }
	if (fread(b, 1, n, f) != (size_t) n) { free(b); fclose(f); return NULL; }
	fclose(f);
	*len = n;
	return b;
}

/* big-endian readers (work whatever the host, so this also runs on a PC) */
static u16 be16(const u8 *p) { return (u16)((p[0] << 8) | p[1]); }
static u32 be32(const u8 *p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | (p[2] << 8) | p[3]; }

static u32 fnv1a(const u8 *a, long na, const u8 *b, long nb)
{
	u32 h = 0x811C9DC5UL;
	long i;
	for (i = 0; i < na; i++) h = ((h ^ a[i]) * 0x01000193UL) & 0xFFFFFFFFUL;
	for (i = 0; i < nb; i++) h = ((h ^ b[i]) * 0x01000193UL) & 0xFFFFFFFFUL;
	return h;
}

/* --- the bin: "APJI" ver16 count16 size16 0; then count*{hash32 w16 h16 off32} --- */
static u32 *bin_hashes(const u8 *d, long len, int *pcount)
{
	int count, i;
	u32 *h;

	if (len < 12 || d[0] != 'A' || d[1] != 'P' || d[2] != 'J' || d[3] != 'I') return NULL;
	count = be16(d + 6);
	if (12 + (long) count * 12 > len) return NULL;
	h = malloc((long) count * sizeof(u32));
	for (i = 0; i < count; i++)
		h[i] = be32(d + 12 + i * 12);
	*pcount = count;
	return h;
}

int main(int argc, char **argv)
{
	const char *rscname = argc > 1 ? argv[1] : "CICONS.RSC";
	const char *binname = argc > 2 ? argv[2] : "APJICONS-32.BIN";
	long rlen, blen;
	u8 *R = slurp(rscname, &rlen);
	u8 *B = slurp(binname, &blen);
	u32 *bh = NULL;
	int bcount = 0, nicons = 0, found = 0, missing = 0;
	long p, rssize, cicon_off;
	FILE *out[3];
	int oi, no;

	no = 0;
	out[no++] = stdout;
	{ FILE *f = fopen("ICONCHK.TXT", "w");        if (f) out[no++] = f; }
	{ FILE *f = fopen("U:\\RAM\\ICONCHK.TXT", "w"); if (f) out[no++] = f; }

	#define P(...) do { for (oi = 0; oi < no; oi++) fprintf(out[oi], __VA_ARGS__); } while (0)

	if (!R) { P("cannot open %s\r\n", rscname); return 1; }
	if (!B) { P("cannot open %s\r\n", binname); return 1; }

	bh = bin_hashes(B, blen, &bcount);
	if (!bh) { P("%s is not a valid apjicons bin\r\n", binname); return 1; }

	P("cicons.rsc = %s  (%ld bytes)\r\n", rscname, rlen);
	P("bin        = %s  (%ld bytes, %d entries)\r\n\r\n", binname, blen, bcount);

	/* RSC header: 18 words; rssize is word[17] (byte offset 34). */
	rssize = be16(R + 34);
	/* extension table at rssize: filesize, cicon_table_off, palette_off, 0 */
	cicon_off = be32(R + rssize + 4);

	/* the cicon table is a run of longs (cicons-per-icon), terminated by -1;
	 * count them so we know how many icons follow */
	p = cicon_off;
	{
		int ncnt = 0;
		long q = p;
		while (be32(R + q) != 0xFFFFFFFFUL) { ncnt++; q += 4; }
		p = q + 4;          /* p now at the first ICONBLK */
		/* walk the icons */
		{
			int k;
			P("%-14s %-7s %-9s %s\r\n", "NAME", "SIZE", "HASH", "RESULT");
			P("-------------- ------- --------- ------\r\n");
			for (k = 0; k < ncnt; k++)
			{
				const u8 *ib = R + p;
				int w = be16(ib + 12 + 5 * 2);   /* wicon = short[5] after 3 longs */
				int h = be16(ib + 12 + 6 * 2);   /* hicon = short[6] */
				long n = (long) (((w + 15) >> 4) * 2) * h;
				long ncic = (long)(unsigned long) be32(ib + 34); /* ncic long follows the ICONBLK */
				const u8 *mono = ib + 38;        /* after ICONBLK(34) + ncic(4) */
				const u8 *mask = mono + n;
				char name[16];
				u32 hash;
				long c;
				int inbin, j;

				memcpy(name, mask + n, 12); name[12] = 0;
				hash = fnv1a(mask, n, mono, n);

				inbin = 0;
				for (j = 0; j < bcount; j++) if (bh[j] == hash) { inbin = 1; break; }
				if (inbin) found++; else missing++;
				nicons++;

				P("%-14s %2dx%-4d %08lX %s\r\n", name, w, h, (unsigned long) hash,
					inbin ? "in bin" : "*** MISSING ***");

				/* advance past ICONBLK(34)+ncic(4)+mono(n)+mask(n)+name(12),
				 * then each cicon: planes(2)+5 longs(20)+col(n*planes)+colmask(n)
				 * +(if sel_data ptr) sel(n*planes)+selmask(n) */
				p = (long) (ib - R) + 38 + n + n + 12;
				for (c = 0; c < ncic; c++)
				{
					int planes = be16(R + p);
					u32 seldata = be32(R + p + 2 + 8); /* 3rd of the 5 longs after planes */

					p += 2 + 20;
					p += (long) n * planes + n;
					if (seldata) p += (long) n * planes + n;
				}
			}
		}
	}

	P("\r\n%d icons: %d in bin, %d MISSING\r\n", nicons, found, missing);
	P("(if some are MISSING, cicons.rsc and this bin were built from different icon art)\r\n");

	for (oi = 1; oi < no; oi++) fclose(out[oi]);
	P("\r\npress Return\r\n");
	getchar();
	return 0;
}
