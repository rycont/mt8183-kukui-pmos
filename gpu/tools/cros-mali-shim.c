/*
 * glibc symbols the ChromeOS Mali blob needs that gcompat does not provide.
 *
 * These are the _FORTIFY_SOURCE wrappers: each takes an extra "size of the
 * destination" argument that glibc uses for a bounds check. musl has no
 * fortify layer, so forward to the plain function and ignore the extra
 * argument -- the blob is already compiled and its buffers are its own.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <poll.h>
#include <limits.h>

void *__memcpy_chk(void *d, const void *s, size_t n, size_t dlen)
{ (void)dlen; return memcpy(d, s, n); }

void *__memmove_chk(void *d, const void *s, size_t n, size_t dlen)
{ (void)dlen; return memmove(d, s, n); }

void *__memset_chk(void *d, int c, size_t n, size_t dlen)
{ (void)dlen; return memset(d, c, n); }

char *__strcpy_chk(char *d, const char *s, size_t dlen)
{ (void)dlen; return strcpy(d, s); }

char *__strncpy_chk(char *d, const char *s, size_t n, size_t dlen)
{ (void)dlen; return strncpy(d, s, n); }

char *__strncat_chk(char *d, const char *s, size_t n, size_t dlen)
{ (void)dlen; return strncat(d, s, n); }

int __vsnprintf_chk(char *s, size_t n, int flag, size_t slen, const char *fmt, va_list ap)
{ (void)flag; (void)slen; return vsnprintf(s, n, fmt, ap); }

int __vfprintf_chk(FILE *f, int flag, const char *fmt, va_list ap)
{ (void)flag; return vfprintf(f, fmt, ap); }

int __vprintf_chk(int flag, const char *fmt, va_list ap)
{ (void)flag; return vprintf(fmt, ap); }

size_t __fread_chk(void *p, size_t plen, size_t size, size_t n, FILE *f)
{ (void)plen; return fread(p, size, n, f); }

ssize_t __read_chk(int fd, void *buf, size_t n, size_t buflen)
{ (void)buflen; return read(fd, buf, n); }

ssize_t __readlink_chk(const char *path, char *buf, size_t n, size_t buflen)
{ (void)buflen; return readlink(path, buf, n); }

char *__realpath_chk(const char *path, char *resolved, size_t rlen)
{ (void)rlen; return realpath(path, resolved); }

int __poll_chk(struct pollfd *fds, nfds_t n, int timeout, size_t fdslen)
{ (void)fdslen; return poll(fds, n, timeout); }

int __ppoll_chk(struct pollfd *fds, nfds_t n, const struct timespec *tmo,
                const sigset_t *mask, size_t fdslen)
{ (void)fdslen; return ppoll(fds, n, tmo, mask); }

/*
 * C23 renamed the string-to-number functions in glibc 2.38 so that they reject
 * the "0b" binary prefix the old ones accept. musl only has the classic names;
 * nothing here parses binary literals, so forwarding is safe.
 */
#include <stdarg.h>

long __isoc23_strtol(const char *s, char **end, int base)
{ return strtol(s, end, base); }

long long __isoc23_strtoll(const char *s, char **end, int base)
{ return strtoll(s, end, base); }

unsigned long __isoc23_strtoul(const char *s, char **end, int base)
{ return strtoul(s, end, base); }

unsigned long long __isoc23_strtoull(const char *s, char **end, int base)
{ return strtoull(s, end, base); }

long __isoc23_strtol_l(const char *s, char **end, int base, void *loc)
{ (void)loc; return strtol(s, end, base); }

unsigned long __isoc23_strtoul_l(const char *s, char **end, int base, void *loc)
{ (void)loc; return strtoul(s, end, base); }

int __isoc23_sscanf(const char *s, const char *fmt, ...)
{
	va_list ap;
	int r;
	va_start(ap, fmt);
	r = vsscanf(s, fmt, ap);
	va_end(ap);
	return r;
}

/* glibc's fortified open() with the mode argument omitted. */
#include <fcntl.h>
int __open64_2(const char *path, int flags)
{ return open(path, flags); }

/* glibc's C++ runtime registers fork handlers through this internal alias. */
int __register_atfork(void (*prepare)(void), void (*parent)(void),
		      void (*child)(void), void *dso_handle)
{
	extern int pthread_atfork(void (*)(void), void (*)(void), void (*)(void));
	(void)dso_handle;
	return pthread_atfork(prepare, parent, child);
}

/*
 * The *64 names are glibc's large-file variants. musl's off_t is always 64-bit,
 * so each is just the plain function under a different name.
 */
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/mman.h>
#include <sys/resource.h>

int fcntl64(int fd, int cmd, ...)
{
	va_list ap;
	void *arg;
	va_start(ap, cmd);
	arg = va_arg(ap, void *);
	va_end(ap);
	return fcntl(fd, cmd, arg);
}

FILE *fopen64(const char *path, const char *mode) { return fopen(path, mode); }
int fstat64(int fd, struct stat *st) { return fstat(fd, st); }
int stat64(const char *path, struct stat *st) { return stat(path, st); }
int lstat64(const char *path, struct stat *st) { return lstat(path, st); }
int fstatfs64(int fd, struct statfs *b) { return fstatfs(fd, b); }
off_t lseek64(int fd, off_t off, int whence) { return lseek(fd, off, whence); }
void *mmap64(void *a, size_t l, int p, int f, int fd, off_t o) { return mmap(a, l, p, f, fd, o); }
ssize_t pread64(int fd, void *buf, size_t n, off_t off) { return pread(fd, buf, n, off); }
int getrlimit64(int r, struct rlimit *l) { return getrlimit(r, l); }
int setrlimit64(int r, const struct rlimit *l) { return setrlimit(r, l); }

int open64(const char *path, int flags, ...)
{
	va_list ap;
	int mode;
	va_start(ap, flags);
	mode = va_arg(ap, int);
	va_end(ap);
	return open(path, flags, mode);
}

/* Memory statistics; the blob only logs these. */
struct mallinfo2 {
	size_t arena, ordblks, smblks, hblks, hblkhd, usmblks;
	size_t fsmblks, uordblks, fordblks, keepcost;
};

struct mallinfo2 mallinfo2(void)
{
	struct mallinfo2 mi;
	memset(&mi, 0, sizeof(mi));
	return mi;
}
