/*
 * mp3play.c - PiStorm host MP3 player front-end (TOS .ttp)
 *
 * Calls the "MP3PLAY" NatFeat exposed by the emulator, which spawns ffmpeg on
 * the Pi to decode the file and mixes the audio into the HDMI output alongside
 * ST/STE sound. The file lives on the host, reached through a mounted HOSTFS
 * drive - so give a FULL path with the drive letter, e.g.
 *
 *     MP3PLAY U:\MM_MP4\SONG.MP3
 *     MP3PLAY STOP
 *
 * Build (in this folder):   make
 * Toolchain:                m68k-atari-mint-gcc
 */

#include <stdio.h>
#include <string.h>
#include <osbind.h>

/* ---- NatFeat call stubs (ARAnyM/PiStorm ABI) -----------------------------
 * 0x7300 = NF_GETID (name ptr at 4(sp) -> id in d0)
 * 0x7301 = NF_CALL  (id at 4(sp), args after -> result in d0)
 * Emitted naked so the opcode traps on the caller's stack frame. On real
 * hardware these are harmless moveq's, so nf_id() returns 0 = "not present". */
__asm__(
"   .text\n"
"   .globl _nf_id\n"
"_nf_id:\n"
"   .word 0x7300\n"
"   rts\n"
"   .globl _nf_call\n"
"_nf_call:\n"
"   .word 0x7301\n"
"   rts\n"
);
extern long nf_id(const char *name);
extern long nf_call(long id, ...);

/* MP3PLAY subids - must match nf_mp3_ops in atari_natfeat.cpp */
#define NF_MP3_PLAY   0
#define NF_MP3_STOP   1
#define NF_MP3_STATUS 2

int main(int argc, char **argv)
{
    long id = nf_id("MP3PLAY");
    if (!id) {
        fputs("MP3PLAY NatFeat not available (not running under PiStorm?)\r\n",
              stderr);
        return 1;
    }

    if (argc < 2 ||
        strcasecmp(argv[1], "stop") == 0 || strcasecmp(argv[1], "off") == 0) {
        nf_call(id | NF_MP3_STOP);
        fputs("MP3 stopped.\r\n", stdout);
        return 0;
    }

    if (strcasecmp(argv[1], "status") == 0) {
        long playing = nf_call(id | NF_MP3_STATUS);
        printf("MP3 %s.\r\n", playing ? "playing" : "idle");
        return 0;
    }

    /* Rebuild the path from ALL remaining args. A dragged file arrives via the
     * GEMDOS command tail, which the startup code splits on spaces - so
     * "U:\A DIR\MY SONG.MP3" lands in argv[1..n] as separate words. Re-join
     * them with single spaces to recover the real filename. */
    char path[512];
    path[0] = '\0';
    for (int i = 1; i < argc; i++) {
        if (i > 1)
            strncat(path, " ", sizeof(path) - strlen(path) - 1);
        strncat(path, argv[i], sizeof(path) - strlen(path) - 1);
    }
    if (path[0] == '\0') {
        fputs("Give a path, e.g. U:\\MUSIC\\SONG.MP3  or  /u/music/song.mp3\r\n",
              stderr);
        return 1;
    }

    /* Desktops quote dragged paths containing spaces: "C:\DIR\MY FILE.MP3".
     * Strip the surrounding double quotes before looking at the drive letter. */
    {
        size_t len = strlen(path);
        if (path[0] == '"') {
            memmove(path, path + 1, len);        /* shifts the NUL too */
            len--;
            if (len > 0 && path[len - 1] == '"')
                path[len - 1] = '\0';
        }
    }

    /* Some desktops drag-drop just the FILENAME and set the CWD to its folder.
     * If there's no drive letter and it isn't a MiNT /x/ path, prepend the
     * current drive:\path so the host gets an absolute path. */
    if (path[0] != '/' && path[1] != ':') {
        char full[512], cwd[256];
        int drv = Dgetdrv();                     /* 0 = A */
        cwd[0] = '\0';
        Dgetpath(cwd, 0);                        /* current path on current drive */
        snprintf(full, sizeof(full), "%c:%s\\%s",
                 'A' + drv, cwd, path);
        strncpy(path, full, sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
    }

    long rc = nf_call(id | NF_MP3_PLAY, path);
    if (rc != 0) {
        fprintf(stderr, "Could not play '%s' (rc=%ld).\r\n"
                "Note: the file must be on the host-shared drive (HOSTFS);\r\n"
                "IDE/floppy drives live inside disk images the host can't open.\r\n",
                path, rc);
        return 1;
    }
    printf("Playing %s\r\n", path);
    return 0;
}
