/*
 * PSCLEAN.PRG - take the Mac litter out of a drive or folder from APJ-OS.
 *
 * A Mac copying onto the share (or onto a floppy image) leaves ._*
 * AppleDouble sidecars, .DS_Store, .smbdeleteXXXX leftovers and the
 * .fseventsd / .Spotlight-V100 / .Trashes / .TemporaryItems folders. They
 * show up on the Atari as files it never asked for. This walks a folder,
 * counts them, asks, and deletes them; nothing else is touched.
 *
 * The same rules as tools/atariclean on the Pi (which also does disk
 * images); this one runs where the drives already are, so it works on
 * C:, on HOSTFS drives and on a floppy in the drive alike.
 *
 * Pick the folder in the file selector (go into it and press OK, or type
 * a drive like S:\); with a command line (run as TTP, or from a shell)
 * PSCLEAN <folder> skips the selector and PSCLEAN -d <folder> skips the
 * question too - for scripts.
 *
 * MiNT only: it walks with Dopendir/Dxreaddir so long names on HOSTFS
 * come back whole (a DTA holds 13 characters).
 */
#include <gem.h>
#include <mint/osbind.h>
#include <mint/mintbind.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* MiNT's XATTR as Dxreaddir fills it (mint/ostruct.h in a full mintlib;
 * spelled out here so the file builds against any header set) */
typedef struct {
    unsigned short mode;
    long           index;
    unsigned short dev, rdev, nlink, uid, gid;
    long           size, blksize, nblocks;
    unsigned short mtime, mdate, atime, adate, ctime, cdate;
    short          attr, reserved2;
    long           reserved3[2];
} MXATTR;

#define PATHMAX     1024
#define NAMEMAX     260
#define MAXDEPTH    24

static long n_files, n_dirs, n_bytes;       /* found */
static long n_gone, n_failed;               /* deleted */
static char first_err[PATHMAX];

/* ---------------------------------------------------------------- rules -- */

static int ieq(const char *a, const char *b)
{
    while (*a && *b) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return 0;
        a++; b++;
    }
    return *a == *b;
}

static int iprefix(const char *s, const char *pfx)
{
    while (*pfx) {
        char x = *s, y = *pfx;
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return 0;
        s++; pfx++;
    }
    return 1;
}

static int is_junk(const char *name, int is_dir)
{
    if (is_dir) {
        if (ieq(name, ".fseventsd") || ieq(name, ".Spotlight-V100") ||
            ieq(name, ".Trashes") || ieq(name, ".TemporaryItems") ||
            ieq(name, ".AppleDouble") || ieq(name, ".AppleDB") ||
            ieq(name, ".AppleDesktop") || ieq(name, "__MACOSX"))
            return 1;
        return iprefix(name, "._") || ieq(name, ".DS_Store");
    }
    return iprefix(name, "._") || ieq(name, ".DS_Store") ||
           iprefix(name, ".smbdelete") || ieq(name, ".apdisk");
}

/* ----------------------------------------------------------------- walk -- */

static void join(char *out, const char *dir, const char *name)
{
    size_t n = strlen(dir);

    strcpy(out, dir);
    if (n && out[n - 1] != '\\' && out[n - 1] != '/')
        strcat(out, "\\");
    strcat(out, name);
}

static long tree_bytes(const char *path, int depth);

/* one directory's entries; cb gets (path, name, is_dir, size) */
typedef void (*entry_fn)(const char *path, const char *name, int is_dir, long size, int depth);

/* the directory is read whole and closed before anything is done to its
 * entries: deleting while a Dreaddir is open can skip the next name */
typedef struct entry {
    struct entry *next;
    long size;
    int is_dir;
    char name[1];
} ENTRY;

static void walk(const char *dir, int depth, entry_fn cb)
{
    long dh;
    char buf[NAMEMAX + 4];
    MXATTR xa;
    long xret;
    char path[PATHMAX];
    ENTRY *head = NULL, *tail = NULL, *e;

    if (depth > MAXDEPTH)
        return;
    dh = Dopendir(dir, 0);
    if (dh < 0)
        return;
    while (Dxreaddir(sizeof buf, dh, buf, &xa, &xret) == 0) {
        const char *name = buf + 4;                 /* after the 32-bit index */

        if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0')))
            continue;
        e = (ENTRY *) malloc(sizeof(ENTRY) + strlen(name));
        if (!e)
            break;
        e->next = NULL;
        e->is_dir = xret == 0 && (xa.mode & 0xF000) == 0x4000;
        e->size = xret == 0 ? xa.size : 0L;
        strcpy(e->name, name);
        if (tail) tail->next = e; else head = e;
        tail = e;
    }
    Dclosedir(dh);
    while (head) {
        e = head;
        head = e->next;
        join(path, dir, e->name);
        cb(path, e->name, e->is_dir, e->size, depth);
        free(e);
    }
}

static void count_cb(const char *path, const char *name, int is_dir, long size, int depth)
{
    if (is_junk(name, is_dir)) {
        if (is_dir) {
            n_dirs++;
            n_bytes += tree_bytes(path, depth + 1);
        } else {
            n_files++;
            n_bytes += size;
        }
    } else if (is_dir)
        walk(path, depth + 1, count_cb);
}

static void bytes_cb(const char *path, const char *name, int is_dir, long size, int depth)
{
    if (is_dir)
        n_bytes += tree_bytes(path, depth + 1);
    else
        n_bytes += size;
}

static long tree_bytes(const char *path, int depth)
{
    long before = n_bytes;
    long got;

    n_bytes = 0;
    walk(path, depth, bytes_cb);
    got = n_bytes;
    n_bytes = before;
    return got;
}

/* --------------------------------------------------------------- delete -- */

static void note_fail(const char *path)
{
    n_failed++;
    if (!first_err[0])
        strncpy(first_err, path, sizeof first_err - 1);
}

static int del_file(const char *path)
{
    if (Fdelete(path) == 0)
        return 1;
    (void) Fattrib(path, 1, 0);                 /* read-only off, then again */
    return Fdelete(path) == 0;
}

static void rm_cb(const char *path, const char *name, int is_dir, long size, int depth);

static void rmtree(const char *path, int depth)
{
    walk(path, depth, rm_cb);
    if (Ddelete(path) != 0)
        note_fail(path);
}

/* inside a litter directory: everything goes */
static void rm_cb(const char *path, const char *name, int is_dir, long size, int depth)
{
    if (is_dir)
        rmtree(path, depth + 1);
    else if (!del_file(path))
        note_fail(path);
}

static void delete_cb(const char *path, const char *name, int is_dir, long size, int depth)
{
    if (is_junk(name, is_dir)) {
        long before = n_failed;

        if (is_dir)
            rmtree(path, depth + 1);
        else if (!del_file(path))
            note_fail(path);
        if (n_failed == before)
            n_gone++;
    } else if (is_dir)
        walk(path, depth + 1, delete_cb);
}

/* ------------------------------------------------------------------ ui -- */

static void kb(char *out, long bytes)
{
    if (bytes >= 1024L * 1024L)
        sprintf(out, "%ld.%ld MB", bytes / (1024L * 1024L), (bytes / (1024L * 100L)) % 10L);
    else
        sprintf(out, "%ld KB", (bytes + 1023L) / 1024L);
}

/* the folder from the selector: the path part, or the drive root */
static int pick_folder(char *dir)
{
    char path[PATHMAX], name[NAMEMAX];
    short button = 0;
    char *p;

    strcpy(path, "S:\\*.*");
    name[0] = '\0';
    if (fsel_exinput(path, name, &button, "Clean which folder? (go in, then OK)") == 0 || button == 0)
        return 0;
    p = strrchr(path, '\\');
    if (p)
        p[1] = '\0';
    strcpy(dir, path);
    return 1;
}

int main(int argc, char **argv)
{
    char dir[PATHMAX];
    char msg[300], size[24];
    int quiet = 0, gem;

    dir[0] = '\0';
    if (argc > 1) {
        int i;

        for (i = 1; i < argc; i++) {
            if (strcmp(argv[i], "-d") == 0)
                quiet = 1;
            else
                strncpy(dir, argv[i], sizeof dir - 1);
        }
    }
    gem = appl_init() >= 0;
    if (!dir[0]) {
        if (!gem || !pick_folder(dir)) {
            if (gem) appl_exit();
            return 1;
        }
    }

    graf_mouse(BUSY_BEE, NULL);
    n_files = n_dirs = n_bytes = 0;
    walk(dir, 0, count_cb);
    graf_mouse(ARROW, NULL);

    if (n_files + n_dirs == 0) {
        if (gem)
            form_alert(1, "[1][No Mac litter here:|nothing to do.][ OK ]");
        else
            printf("%s: nothing to do\n", dir);
        if (gem) appl_exit();
        return 0;
    }
    kb(size, n_bytes);
    if (!quiet) {
        int r;

        if (!gem) {
            printf("%s: %ld files, %ld folders of Mac litter, %s. Run with -d to delete.\n",
                   dir, n_files, n_dirs, size);
            return 0;
        }
        sprintf(msg, "[2][Mac litter found:|%ld file%s and %ld folder%s|(%s, ._* .DS_Store .smbdelete|.fseventsd .Trashes ...)][Delete|Cancel]",
                n_files, n_files == 1 ? "" : "s", n_dirs, n_dirs == 1 ? "" : "s", size);
        r = form_alert(2, msg);
        if (r != 1) {
            appl_exit();
            return 0;
        }
    }

    graf_mouse(BUSY_BEE, NULL);
    n_gone = n_failed = 0;
    first_err[0] = '\0';
    walk(dir, 0, delete_cb);
    graf_mouse(ARROW, NULL);

    if (n_failed == 0)
        sprintf(msg, "[1][Removed %ld item%s|(%s).][ OK ]", n_gone, n_gone == 1 ? "" : "s", size);
    else {
        char shortp[40];

        strncpy(shortp, first_err + (strlen(first_err) > 36 ? strlen(first_err) - 36 : 0), 39);
        shortp[39] = '\0';
        sprintf(msg, "[3][Removed %ld, could not remove %ld.|First: ...%s][ OK ]", n_gone, n_failed, shortp);
    }
    if (gem)
        form_alert(1, msg);
    else
        printf("%s: removed %ld, failed %ld\n", dir, n_gone, n_failed);
    if (gem) appl_exit();
    return n_failed ? 1 : 0;
}
