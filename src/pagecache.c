/*
 * userspace pagecache management using LD_PRELOAD, fadvise and
 * sync_file_range().
 *
 * Original by
 * Andrew Morton <akpm@linux-foundation.org>
 * March, 2007
 *
 * Andreas Monzner <andreas.monzner@dream-property.net>
 * July, 2011
 *
 */

#include "libpagecache-config.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <pthread.h>

extern void *find_symbol(void *hdn, const char *symbol, void *repl);

enum fd_state {
	FDS_UNKNOWN = 0,	/* We know nothing about this fd */
	FDS_IGNORE,		/* Ignore this file (!S_ISREG?) */
	FDS_ACTIVE,		/* We're managing this file's pagecache */
};

struct fd_status {
	enum fd_state state;
	unsigned int bytes;
};

static struct fd_status fd_status[4096+1];
static pthread_mutex_t realloc_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned int pagecache_flush_interval = 1024 * 1024; // default 1MB

static void initialize_globals_ctor(void) __attribute__ ((constructor));
static void initialize_globals(void);

#define CALL(func, ...) __extension__ \
({ \
	if (!func) \
		initialize_globals(); \
	func(__VA_ARGS__); \
})

static int (*libc_close)(int fd);
static int (*libc_dup2)(int oldfd, int newfd);
static int (*libc_dup3)(int oldfd, int newfd, int flags);
static int (*libc_fclose)(FILE *fp);
static size_t (*libc_fread)(void *ptr, size_t size, size_t nmemb, FILE *stream);
static size_t (*libc_fread_unlocked)(void *ptr, size_t size, size_t nmemb, FILE *stream);
static size_t (*libc_fwrite)(const void *ptr, size_t size, size_t nmemb, FILE *stream);
static size_t (*libc_fwrite_unlocked)(const void *ptr, size_t size, size_t nmemb, FILE *stream);
static ssize_t (*libc_pread)(int fd, void *buf, size_t count, off_t offset);
static ssize_t (*libc_pwrite)(int fd, const void *buf, size_t count, off_t offset);
static ssize_t (*libc_pread64)(int fd, void *buf, size_t count, off64_t offset);
static ssize_t (*libc_pwrite64)(int fd, const void *buf, size_t count, off64_t offset);
static ssize_t (*libc_read)(int fd, void *buf, size_t count);
static ssize_t (*libc_write)(int fd, const void *buf, size_t count);
static ssize_t (*libc_sendfile)(int out_fd, int in_fd, off_t *offset, size_t count);
static void *(*libc_dlsym)(void *hnd, const char *sym);

static struct fd_status *get_fd_status(int fd, int no_debug)
{
	if (fd > 4095) {
		if (!no_debug) {
			fprintf(stderr, "libpagecache broken!!! please report... fd %d requested\n", fd);
			fflush(stderr);
		}
		fd = 4096;
	}
	return &fd_status[fd];
}

/*
 * Work out if we're interested in this fd
 */
static void inspect_fd(int fd, struct fd_status *fds)
{
	struct stat stat_buf;

	if (fstat(fd, &stat_buf))
		return;
	fds->bytes = 0;
	if ((S_ISREG(stat_buf.st_mode) || S_ISBLK(stat_buf.st_mode)) && !(fcntl(fd, F_GETFL) & O_DIRECT)) {
		posix_fadvise(fd, 0, 0, POSIX_FADV_RANDOM);
		fds->state = FDS_ACTIVE;
	}
	else
		fds->state = FDS_IGNORE;
}

static void fd_touched_bytes(int fd, ssize_t count)
{
	struct fd_status *fds;

	if (pagecache_flush_interval == 0)
		return;
	if (fd < 0)
		return;
	if (count <= 0)
		return;

	fds = get_fd_status(fd, 0);

	if (fds->state == FDS_UNKNOWN)
		inspect_fd(fd, fds);

	if (fds->state == FDS_IGNORE)
		return;

	fds->bytes += count;

	if (fds->bytes >= pagecache_flush_interval) {
		posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
		fds->bytes = 0;
	}
}

static void fd_pre_close(int fd, int no_debug)
{
	struct fd_status *fds;

	if (pagecache_flush_interval == 0)
		return;
	if (fd < 0)
		return;

	fds = 	get_fd_status(fd, no_debug);

	if (fds->state == FDS_ACTIVE) {
		sync_file_range(fd, 0, LONG_LONG_MAX,
			SYNC_FILE_RANGE_WRITE | SYNC_FILE_RANGE_WAIT_AFTER);
		posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
	}

	fds->state = FDS_UNKNOWN;
}

static void file_touched_bytes(size_t size, size_t nmemb, FILE *stream)
{
	/* does not prevent possible overflow of size * nmemb */
	if (stream != NULL)
		fd_touched_bytes(fileno(stream), size * nmemb);
}

static void file_pre_close(FILE *fp)
{
	if (fp != NULL)
		fd_pre_close(fileno(fp), 1);
}

/*
 * syscall interface
 */

ssize_t pagecache_write(int fd, const void *buf, size_t count)
{
	ssize_t ret = CALL(libc_write, fd, buf, count);
	fd_touched_bytes(fd, ret);
	return ret;
}

ssize_t pagecache_pwrite(int fd, const void *buf, size_t count, off_t offset)
{
	ssize_t ret = CALL(libc_pwrite, fd, buf, count, offset);
	fd_touched_bytes(fd, ret);
	return ret;
}

ssize_t pagecache_pwrite64(int fd, const void *buf, size_t count, off64_t offset)
{
	ssize_t ret = CALL(libc_pwrite64, fd, buf, count, offset);
	fd_touched_bytes(fd, ret);
	return ret;
}

size_t pagecache_fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream)
{
	size_t ret = CALL(libc_fwrite, ptr, size, nmemb, stream);
	file_touched_bytes(size, ret, stream);
	return ret;
}

size_t pagecache_fwrite_unlocked(const void *ptr, size_t size, size_t nmemb, FILE *stream)
{
	size_t ret = CALL(libc_fwrite_unlocked, ptr, size, nmemb, stream);
	file_touched_bytes(size, ret, stream);
	return ret;
}

ssize_t pagecache_read(int fd, void *buf, size_t count)
{
	ssize_t ret = CALL(libc_read, fd, buf, count);
	fd_touched_bytes(fd, ret);
	return ret;
}

ssize_t pagecache_pread(int fd, void *buf, size_t count, off_t offset)
{
	ssize_t ret = CALL(libc_pread, fd, buf, count, offset);
	fd_touched_bytes(fd, ret);
	return ret;
}

ssize_t pagecache_pread64(int fd, void *buf, size_t count, off64_t offset)
{
	ssize_t ret = CALL(libc_pread64, fd, buf, count, offset);
	fd_touched_bytes(fd, ret);
	return ret;
}

size_t pagecache_fread(void *ptr, size_t size, size_t nmemb, FILE *stream)
{
	size_t ret = CALL(libc_fread, ptr, size, nmemb, stream);
	file_touched_bytes(size, ret, stream);
	return ret;
}

size_t pagecache_fread_unlocked(void *ptr, size_t size, size_t nmemb, FILE *stream)
{
	size_t ret = CALL(libc_fread_unlocked, ptr, size, nmemb, stream);
	file_touched_bytes(size, ret, stream);
	return ret;
}

ssize_t pagecache_sendfile(int out_fd, int in_fd, off_t *offset, size_t count)
{
	ssize_t processed = 0;

	while (processed < count) {
		ssize_t ret;
		size_t rest = count - processed;
		if (rest > pagecache_flush_interval)
			rest = pagecache_flush_interval;
		ret = CALL(libc_sendfile, out_fd, in_fd, offset, rest);
		if (ret <= 0) { // error, EOF or interrupted syscall
			if (processed == 0)
				processed = ret;
			break;
		}

		processed += ret;
		fd_touched_bytes(in_fd, ret);
		fd_touched_bytes(out_fd, ret);
	}

	return processed;
}

int pagecache_fclose(FILE *fp)
{
	file_pre_close(fp);
	return CALL(libc_fclose, fp);
}

int pagecache_close(int fd)
{
	fd_pre_close(fd, 1);
	return CALL(libc_close, fd);
}

int pagecache_dup2(int oldfd, int newfd)
{
	if ((oldfd >= 0) && (newfd != oldfd))
		fd_pre_close(newfd, 0);
	return CALL(libc_dup2, oldfd, newfd);
}

int pagecache_dup3(int oldfd, int newfd, int flags)
{
	if ((oldfd >= 0) && (newfd != oldfd))
		fd_pre_close(newfd, 0);
	return CALL(libc_dup3, oldfd, newfd, flags);
}

static void initialize_globals_ctor(void)
{
	initialize_globals();
}

static void initialize_globals(void)
{
	if (libc_close)
		return;
	char *e = getenv("PAGECACHE_FLUSH_INTERVAL");
	if (e != NULL)
		pagecache_flush_interval = strtoul(e, NULL, 10);

	fd_status[4096].state = FDS_IGNORE;

	libc_close = find_symbol(NULL, "close", pagecache_close);
	libc_dup2 = find_symbol(NULL, "dup2", pagecache_dup2);
	libc_dup3 = find_symbol(NULL, "dup3", pagecache_dup3);
	libc_fclose = find_symbol(NULL, "fclose", pagecache_fclose);
	libc_fread = find_symbol(NULL, "fread", pagecache_fread);
	libc_fread_unlocked = find_symbol(NULL, "fread_unlocked", pagecache_fread_unlocked);
	libc_fwrite = find_symbol(NULL, "fwrite", pagecache_fwrite);
	libc_fwrite_unlocked = find_symbol(NULL, "fwrite_unlocked", pagecache_fwrite_unlocked);
	libc_pread = find_symbol(NULL, "pread", pagecache_pread);
	libc_pwrite = find_symbol(NULL, "pwrite", pagecache_pwrite);
	libc_pread64 = find_symbol(NULL, "pread64", pagecache_pread64);
	libc_pwrite64 = find_symbol(NULL, "pwrite64", pagecache_pwrite64);
	libc_read = find_symbol(NULL, "read", pagecache_read);
	libc_write = find_symbol(NULL, "write", pagecache_write);
	libc_sendfile = find_symbol(NULL, "sendfile", pagecache_sendfile);
}

/* When compiling with -O2, glibc uses optimized inline macros,
   which we need to undefine at this point to avoid a compiler error. */
#undef fread_unlocked
#undef fwrite_unlocked

ssize_t write(int fd, const void *buf, size_t count) __attribute__ ((weak, alias ("pagecache_write")));
ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset) __attribute__ ((weak, alias ("pagecache_pwrite")));
ssize_t pwrite64(int fd, const void *buf, size_t count, off64_t offset) __attribute__ ((weak, alias ("pagecache_pwrite64")));
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream) __attribute__ ((weak, alias ("pagecache_fwrite")));
size_t fwrite_unlocked(const void *ptr, size_t size, size_t nmemb, FILE *stream) __attribute__ ((weak, alias ("pagecache_fwrite_unlocked")));
ssize_t read(int fd, void *buf, size_t count) __attribute__ ((weak, alias ("pagecache_read")));
ssize_t pread(int fd, void *buf, size_t count, off_t offset) __attribute__ ((weak, alias ("pagecache_pread")));
ssize_t pread64(int fd, void *buf, size_t count, off64_t offset) __attribute__ ((weak, alias ("pagecache_pread64")));
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream) __attribute__ ((weak, alias ("pagecache_fread")));
size_t fread_unlocked(void *ptr, size_t size, size_t nmemb, FILE *stream) __attribute__ ((weak, alias ("pagecache_fread_unlocked")));
ssize_t sendfile(int out_fd, int in_fd, off_t *offset, size_t count) __attribute__ ((weak, alias ("pagecache_sendfile")));
int fclose(FILE *fp) __attribute__ ((weak, alias ("pagecache_fclose")));
int close(int fd) __attribute__ ((weak, alias ("pagecache_close")));
int dup2(int oldfd, int newfd) __attribute__ ((weak, alias ("pagecache_dup2")));
int dup3(int oldfd, int newfd, int flags) __attribute__ ((weak, alias ("pagecache_dup3")));
