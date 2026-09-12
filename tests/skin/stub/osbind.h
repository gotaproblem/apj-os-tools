#ifndef STUB_OSBIND_H
#define STUB_OSBIND_H
long Fopen(const char *p, short mode);
long Fread(short h, long n, void *buf);
long Fseek(long off, short h, short mode);
long Fclose(short h);
long Mxalloc(long n, short mode);
long Mfree(void *p);
short Dgetdrv(void);
#endif
