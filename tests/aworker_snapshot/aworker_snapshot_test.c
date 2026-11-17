/* Tests of the A-worker configuration snapshots (sealed memfd, S2-2 step 4) */
/*
 * SPDX-License-Identifier: AGPL-3.0+
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/* S-1: a snapshot is a sealed memfd with the expected name, flags, seals and contents, and nothing can change it.
 * S-2: the memfd flags and the EINVAL-only retry without MFD_NOEXEC_SEAL; never a snapshot with fewer seals.
 * S-7: the size limits, and what a worker accepts and refuses.
 * S-8: the worker closes the snapshot descriptor after reading it, on success and on every refusal.
 * S-9: the worker reads with pread() from offset 0, whatever the file offset, and leaves it unchanged.
 * S-10: what this kernel and C library actually do (to stderr, not part of the expected output; checked).
 * S-3 to S-6 (S2-2 step 5): the fallback (D-4) -- its WARNING, the directories it may and may not use, how its file is
 * made, and what it does with directories left over from earlier runs.
 *
 * The same source is built twice (see Makefile.am): against libaworker's module (memfd_create() of the C library) and
 * against the module compiled with its system-call path. Failures are injected with -Wl,--wrap: memfd_create() or
 * syscall(), fcntl() and pread(); for the fallback also open(), openat() and mkdirat(), which also redirect /dev/shm
 * and /tmp and record what the module did. Unarmed, the wrappers only pass the call on. Both programs have the same
 * expected output. */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <osmocom/core/application.h>
#include <osmocom/core/logging.h>
#include <osmocom/core/talloc.h>
#include <osmocom/core/utils.h>

#include <osmocom/bsc/aworker_cfg_view.h>
#include <osmocom/bsc/aworker_log.h>
#include <osmocom/bsc/aworker_snapshot.h>

#ifndef MFD_NOEXEC_SEAL
#define MFD_NOEXEC_SEAL 0x0008U
#endif
#ifndef F_SEAL_EXEC
#define F_SEAL_EXEC 0x0020
#endif
#ifndef F_SEAL_FUTURE_WRITE
#define F_SEAL_FUTURE_WRITE 0x0010
#endif
#ifndef O_TMPFILE
#define O_TMPFILE (020000000 | O_DIRECTORY)
#endif

#define FOUR_SEALS (F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL)

static void *ctx;
static char g_dir[256];
static unsigned int failures;

#define CHECK(exp) do { \
		if (!(exp)) { \
			printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #exp); \
			failures++; \
		} \
	} while (0)

/***********************************************************************
 * Fault injection (link-time wrappers)
 ***********************************************************************/

static struct {
	bool armed;
	int memfd_errno[4];		/* errno for the n-th memfd creation while armed; 0: the real call */
	unsigned int memfd_calls;
	unsigned int memfd_flags[4];	/* the flags of each call */
	int add_seals_errno;		/* F_ADD_SEALS fails with it */
	int get_seals_drop;		/* these seals are removed from what F_GET_SEALS reports */
	long pread_eof_at;		/* > 0: pread() acts as if the file ended at this offset */
} g_fault;

static void fault_reset(void)
{
	memset(&g_fault, 0, sizeof(g_fault));
}

/* The file system side of the fallback (D-4): where /dev/shm and /tmp lead, injected failures, and what the module
 * did for the last snapshot. Not cleared by fault_reset(). */
static struct {
	const char *shm, *tmp;		/* if set: open("/dev/shm") / open("/tmp") opens this instead */
	bool no_tmpfile;		/* openat(O_TMPFILE) fails (EOPNOTSUPP) */
	bool no_named;			/* openat(O_CREAT) fails (EACCES) */
	bool no_named_reopen;		/* openat(..., "snapshot", O_RDONLY) fails (EACCES) */
	int proc_reopen;		/* 1: open("/proc/self/fd/N") fails (ENOENT); 2: it opens other_file instead */
	const char *other_file;
	int mkdir_eexist;		/* the next n mkdirat() calls fail with EEXIST */
	bool fake_uid;			/* fstat() of the inode fake_dev/fake_ino reports owner fake_uid_value */
	dev_t fake_dev;
	ino_t fake_ino;
	uid_t fake_uid_value;
	/* observed */
	unsigned int mkdirat_calls;
	mode_t mkdirat_mode;
	char mkdirat_name[128];
	bool file_seen;			/* a file was created (O_TMPFILE or O_CREAT) */
	int file_flags;			/* the flags it was created with */
	mode_t dir_mode;		/* mode and owner of the directory it was created in, at that time */
	uid_t dir_uid;
	dev_t file_dev;			/* the file created */
	ino_t file_ino;
} g_fs;

static void fs_observed_reset(void)
{
	g_fs.mkdirat_calls = 0;
	g_fs.mkdirat_mode = 0;
	g_fs.mkdirat_name[0] = '\0';
	g_fs.file_seen = false;
	g_fs.file_flags = 0;
	g_fs.dir_mode = 0;
	g_fs.dir_uid = (uid_t)-1;
	g_fs.file_dev = 0;
	g_fs.file_ino = 0;
}

static const char *fs_redirect(const char *path)
{
	if (g_fs.shm && !strcmp(path, "/dev/shm"))
		return g_fs.shm;
	if (g_fs.tmp && !strcmp(path, "/tmp"))
		return g_fs.tmp;
	if (g_fs.proc_reopen == 2 && !strncmp(path, "/proc/self/fd/", 14))
		return g_fs.other_file;
	return path;
}

static bool fs_open_fails(const char *path)
{
	if (g_fs.proc_reopen == 1 && !strncmp(path, "/proc/self/fd/", 14)) {
		errno = ENOENT;
		return true;
	}
	return false;
}

static bool creating(int flags)
{
	return (flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE;
}

int __real_open(const char *path, int flags, ...);
int __wrap_open(const char *path, int flags, ...);
int __wrap_open(const char *path, int flags, ...)
{
	mode_t mode = 0;
	va_list ap;

	if (creating(flags)) {
		va_start(ap, flags);
		mode = (mode_t)va_arg(ap, int);
		va_end(ap);
	}
	if (fs_open_fails(path))
		return -1;
	return __real_open(fs_redirect(path), flags, mode);
}

/* With _FORTIFY_SOURCE, an open() whose flags are not a constant is compiled as __open_2(). */
int __real___open_2(const char *path, int flags);
int __wrap___open_2(const char *path, int flags);
int __wrap___open_2(const char *path, int flags)
{
	if (fs_open_fails(path))
		return -1;
	return __real___open_2(fs_redirect(path), flags);
}

static int fs_openat(int dirfd, const char *path, int flags, mode_t mode, bool chk);
int __real_openat(int dirfd, const char *path, int flags, ...);
int __real___openat_2(int dirfd, const char *path, int flags);
int __wrap_openat(int dirfd, const char *path, int flags, ...);
int __wrap_openat(int dirfd, const char *path, int flags, ...)
{
	mode_t mode = 0;
	va_list ap;

	if (creating(flags)) {
		va_start(ap, flags);
		mode = (mode_t)va_arg(ap, int);
		va_end(ap);
	}
	return fs_openat(dirfd, path, flags, mode, false);
}

int __wrap___openat_2(int dirfd, const char *path, int flags);
int __wrap___openat_2(int dirfd, const char *path, int flags)
{
	return fs_openat(dirfd, path, flags, 0, true);
}

static int fs_openat(int dirfd, const char *path, int flags, mode_t mode, bool chk)
{
	struct stat st;
	int fd;

	if (g_fs.no_named_reopen && !creating(flags) && !strcmp(path, "snapshot")) {
		errno = EACCES;
		return -1;
	}
	if (creating(flags)) {
		if ((flags & O_TMPFILE) == O_TMPFILE && g_fs.no_tmpfile) {
			errno = EOPNOTSUPP;
			return -1;
		}
		if ((flags & O_CREAT) && g_fs.no_named) {
			errno = EACCES;
			return -1;
		}
		if (fstat(dirfd, &st) == 0) {
			g_fs.dir_mode = st.st_mode & 07777;
			g_fs.dir_uid = st.st_uid;
		}
		g_fs.file_flags = flags;
		g_fs.file_seen = true;
	}
	fd = chk ? __real___openat_2(dirfd, path, flags) : __real_openat(dirfd, path, flags, mode);
	if (creating(flags) && fd >= 0 && fstat(fd, &st) == 0) {
		g_fs.file_dev = st.st_dev;
		g_fs.file_ino = st.st_ino;
	}
	return fd;
}

/* Another owner for one inode (files of other users cannot be made without root). */
static void fake_owner(const char *path, int fd, uid_t uid)
{
	struct stat st;

	OSMO_ASSERT(path ? stat(path, &st) == 0 : fstat(fd, &st) == 0);
	g_fs.fake_dev = st.st_dev;
	g_fs.fake_ino = st.st_ino;
	g_fs.fake_uid_value = uid;
	g_fs.fake_uid = true;
}

int __real_fstat(int fd, struct stat *st);
int __wrap_fstat(int fd, struct stat *st);
int __wrap_fstat(int fd, struct stat *st)
{
	int rc = __real_fstat(fd, st);

	if (rc == 0 && g_fs.fake_uid && st->st_dev == g_fs.fake_dev && st->st_ino == g_fs.fake_ino)
		st->st_uid = g_fs.fake_uid_value;
	return rc;
}

int __real_mkdirat(int dirfd, const char *path, mode_t mode);
int __wrap_mkdirat(int dirfd, const char *path, mode_t mode);
int __wrap_mkdirat(int dirfd, const char *path, mode_t mode)
{
	g_fs.mkdirat_calls++;
	g_fs.mkdirat_mode = mode;
	osmo_strlcpy(g_fs.mkdirat_name, path, sizeof(g_fs.mkdirat_name));
	if (g_fs.mkdir_eexist > 0) {
		g_fs.mkdir_eexist--;
		errno = EEXIST;
		return -1;
	}
	return __real_mkdirat(dirfd, path, mode);
}

/* What the module logs (A-worker category), kept for the checks. */
static char g_log[65536];
static size_t g_log_len;

static void log_capture(struct log_target *target, unsigned int level, const char *string)
{
	size_t l = strlen(string);

	if (g_log_len + l + 1 < sizeof(g_log)) {
		memcpy(g_log + g_log_len, string, l);
		g_log_len += l;
		g_log[g_log_len] = '\0';
	}
}

static void log_clear(void)
{
	g_log_len = 0;
	g_log[0] = '\0';
}

static unsigned int log_count(const char *needle)
{
	unsigned int n = 0;
	const char *p = g_log;

	while ((p = strstr(p, needle))) {
		n++;
		p += strlen(needle);
	}
	return n;
}

/* \returns -1 (errno set) if this memfd creation is to fail, 0 to do the real call. */
static int fault_memfd(unsigned int flags)
{
	unsigned int n;

	if (!g_fault.armed)
		return 0;
	n = g_fault.memfd_calls++;
	if (n < ARRAY_SIZE(g_fault.memfd_flags))
		g_fault.memfd_flags[n] = flags;
	if (n < ARRAY_SIZE(g_fault.memfd_errno) && g_fault.memfd_errno[n]) {
		errno = g_fault.memfd_errno[n];
		return -1;
	}
	return 0;
}

#ifdef AWORKER_SNAPSHOT_USE_SYSCALL
long __real_syscall(long n, ...);
long __wrap_syscall(long n, ...);
long __wrap_syscall(long n, ...)
{
	va_list ap;
	long a[6];
	unsigned int i;

	va_start(ap, n);
	for (i = 0; i < ARRAY_SIZE(a); i++)
		a[i] = va_arg(ap, long);
	va_end(ap);
	if (n == SYS_memfd_create && fault_memfd((unsigned int)a[1]) < 0)
		return -1;
	return __real_syscall(n, a[0], a[1], a[2], a[3], a[4], a[5]);
}
#else
int __real_memfd_create(const char *name, unsigned int flags);
int __wrap_memfd_create(const char *name, unsigned int flags);
int __wrap_memfd_create(const char *name, unsigned int flags)
{
	if (fault_memfd(flags) < 0)
		return -1;
	return __real_memfd_create(name, flags);
}
#endif

/* pread() as the file would answer if it ended at g_fault.pread_eof_at. */
static size_t fault_pread_count(size_t count, off_t offset)
{
	if (!g_fault.armed || g_fault.pread_eof_at <= 0)
		return count;
	if (offset >= g_fault.pread_eof_at)
		return 0;
	if (offset + (off_t)count > g_fault.pread_eof_at)
		return (size_t)(g_fault.pread_eof_at - offset);
	return count;
}

ssize_t __real_pread(int fd, void *buf, size_t count, off_t offset);
ssize_t __wrap_pread(int fd, void *buf, size_t count, off_t offset);
ssize_t __wrap_pread(int fd, void *buf, size_t count, off_t offset)
{
	count = fault_pread_count(count, offset);
	return count ? __real_pread(fd, buf, count, offset) : 0;
}

/* With _FORTIFY_SOURCE (optimised builds), a pread() into a buffer of known size is compiled as __pread_chk(). */
ssize_t __real___pread_chk(int fd, void *buf, size_t count, off_t offset, size_t buflen);
ssize_t __wrap___pread_chk(int fd, void *buf, size_t count, off_t offset, size_t buflen);
ssize_t __wrap___pread_chk(int fd, void *buf, size_t count, off_t offset, size_t buflen)
{
	count = fault_pread_count(count, offset);
	return count ? __real___pread_chk(fd, buf, count, offset, buflen) : 0;
}

int __real_fcntl(int fd, int cmd, ...);
int __wrap_fcntl(int fd, int cmd, ...);
int __wrap_fcntl(int fd, int cmd, ...)
{
	va_list ap;
	long arg;
	int rc;

	va_start(ap, cmd);
	arg = va_arg(ap, long);
	va_end(ap);
	if (g_fault.armed && cmd == F_ADD_SEALS && g_fault.add_seals_errno) {
		errno = g_fault.add_seals_errno;
		return -1;
	}
	rc = __real_fcntl(fd, cmd, arg);
	if (g_fault.armed && cmd == F_GET_SEALS && rc >= 0)
		rc &= ~g_fault.get_seals_drop;
	return rc;
}

/***********************************************************************
 * Helpers
 ***********************************************************************/

static const char *errname(int rc)
{
	switch (rc) {
	case 0: return "ok";
	case -EBADF: return "-EBADF";
	case -EINVAL: return "-EINVAL";
	case -EPERM: return "-EPERM";
	case -EFBIG: return "-EFBIG";
	case -EIO: return "-EIO";
	case -ESTALE: return "-ESTALE";
	case -EMFILE: return "-EMFILE";
	case -ENOSYS: return "-ENOSYS";
	default: return "other";
	}
}

/* A snapshot a case needs: failing to make one is a test failure (reported, not an abort), and the rest of that
 * case is skipped. */
static bool need(int rc, const char *what)
{
	if (rc == 0)
		return true;
	printf("  FAIL %s: %s; the rest of this case is skipped\n", what, errname(rc));
	failures++;
	return false;
}

static unsigned int count_fds(void)
{
	unsigned int n = 0;
	struct dirent *de;
	DIR *d = opendir("/proc/self/fd");

	OSMO_ASSERT(d);
	while ((de = readdir(d)))
		n += de->d_name[0] != '.';
	closedir(d);
	return n - 1;	/* the directory's own descriptor */
}

static bool fd_open(int fd)
{
	return fcntl(fd, F_GETFD) >= 0;
}

/* A memfd made here, not by the module: contents, then the given seals (0: none). */
static int own_memfd(const char *text, size_t len, int seals)
{
	int fd = memfd_create("test", MFD_CLOEXEC | MFD_ALLOW_SEALING);

	OSMO_ASSERT(fd >= 0);
	OSMO_ASSERT(len == 0 || pwrite(fd, text, len, 0) == (ssize_t)len);
	if (seals)
		OSMO_ASSERT(fcntl(fd, F_ADD_SEALS, seals) == 0);
	return fd;
}

static char *big_text(size_t len)
{
	char *t = talloc_size(ctx, len + 1);
	size_t i;

	OSMO_ASSERT(t);
	for (i = 0; i < len; i++)
		t[i] = (i % 64 == 63) ? '\n' : '#';
	t[len] = '\0';
	return t;
}

static const char *view = "a-worker-pool\n workers 1\n worker 0 cs7-instance 0\ncs7 instance 0\n"
			  " point-code 0.23.3\n asp asp-x 2905 0 m3ua\n  remote-ip 10.0.0.10\n";

/* Read fd as worker with IPC socket ipc; print the outcome (the reason goes to stderr: its wording can depend on the
 * file system); check the expected result, and that fd is closed afterwards unless it is not a snapshot descriptor
 * (0, 1, 2 or ipc), which must stay open (S-8). \returns the read's result. */
static int worker_read(const char *label, int fd, int ipc, const char *expect, size_t expect_len, int expect_rc)
{
	char why[256], *text = NULL;
	size_t len = 0;
	bool kept = fd >= 0 && (fd <= 2 || fd == ipc);
	int rc = aworker_snapshot_read(ctx, fd, ipc, &text, &len, why, sizeof(why));

	printf("    %s: %s\n", label, errname(rc));
	if (rc)
		fprintf(stderr, "%s: %s\n", label, why);
	CHECK(rc == expect_rc);
	CHECK(rc == 0 || why[0]);
	if (rc == 0) {
		CHECK(expect && len == expect_len && !memcmp(text, expect, len) && text[len] == '\0');
	} else {
		CHECK(!text && !len);
	}
	if (fd >= 0) {
		if (kept)
			CHECK(fd_open(fd));
		else
			CHECK(!fd_open(fd) && errno == EBADF);
	}
	talloc_free(text);
	return rc;
}

/***********************************************************************
 * S-1
 ***********************************************************************/

static void test_s1(void)
{
	struct aworker_snapshot *s;
	char link[128], p[64], buf[512], c = 'X';
	struct stat st;
	ssize_t l;
	void *map;
	int rc, seals, fd2;
	size_t len = strlen(view);

	printf("S-1: a snapshot is a sealed memfd\n");
	rc = aworker_snapshot_create(ctx, 3, view, len, &s);
	printf("  create (worker 3): %s\n", errname(rc));
	if (!need(rc, "S-1 create"))
		return;
	snprintf(p, sizeof(p), "/proc/self/fd/%d", s->fd);
	l = readlink(p, link, sizeof(link) - 1);
	OSMO_ASSERT(l > 0);
	link[l] = '\0';
	seals = fcntl(s->fd, F_GET_SEALS);
	OSMO_ASSERT(fstat(s->fd, &st) == 0);
	printf("  descriptor above 2: %s; close-on-exec: %s; name: %s\n", s->fd > 2 ? "yes" : "no",
	       fcntl(s->fd, F_GETFD) & FD_CLOEXEC ? "yes" : "no", link);
	printf("  seals shrink, grow, write, seal: %s\n", (seals & FOUR_SEALS) == FOUR_SEALS ? "all four" : "MISSING");
	printf("  regular file: %s; link count: %lu; size: %s; contents: %s\n", S_ISREG(st.st_mode) ? "yes" : "no",
	       (unsigned long)st.st_nlink, (size_t)st.st_size == len ? "the view's" : "WRONG",
	       pread(s->fd, buf, sizeof(buf), 0) == (ssize_t)len && !memcmp(buf, view, len) ? "the view" : "WRONG");
	CHECK(s->fd > 2 && (fcntl(s->fd, F_GETFD) & FD_CLOEXEC));
	CHECK(!strcmp(link, "/memfd:bsc-aworker-3 (deleted)"));
	CHECK((seals & FOUR_SEALS) == FOUR_SEALS);
	CHECK(S_ISREG(st.st_mode) && st.st_nlink == 0 && (size_t)st.st_size == len);
	CHECK(s->idx == 3 && s->len == len && !memcmp(s->text, view, len));
	printf("  verify: %s\n", errname(aworker_snapshot_verify(s)));
	CHECK(aworker_snapshot_verify(s) == 0);

	/* Nothing can change it: each attempt fails with EPERM, the contents stay. */
	printf("  write protection:\n");
#define EXPECT_EPERM(label, call) do { \
		int _r; \
		errno = 0; \
		_r = (call); \
		printf("    %s: %s\n", label, _r < 0 && errno == EPERM ? "EPERM" : "ALLOWED"); \
		CHECK(_r < 0 && errno == EPERM); \
	} while (0)
	EXPECT_EPERM("write()", (int)write(s->fd, &c, 1));
	EXPECT_EPERM("pwrite() at offset 0", (int)pwrite(s->fd, &c, 1, 0));
	EXPECT_EPERM("pwrite() past the end", (int)pwrite(s->fd, &c, 1, (off_t)len));
	EXPECT_EPERM("ftruncate() shorter", ftruncate(s->fd, (off_t)len - 1));
	EXPECT_EPERM("ftruncate() longer", ftruncate(s->fd, (off_t)len + 1));
	EXPECT_EPERM("fallocate() past the end", fallocate(s->fd, 0, (off_t)len, 4096));
	EXPECT_EPERM("fallocate(PUNCH_HOLE)", fallocate(s->fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, 0, 1));
	EXPECT_EPERM("adding a seal (seals cannot be removed; F_SEAL_SEAL forbids any change)",
		     fcntl(s->fd, F_ADD_SEALS, F_SEAL_FUTURE_WRITE));
	errno = 0;
	map = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, s->fd, 0);
	printf("    mmap(PROT_WRITE, MAP_SHARED): %s\n", map == MAP_FAILED && errno == EPERM ? "EPERM" : "ALLOWED");
	CHECK(map == MAP_FAILED && errno == EPERM);
	if (map != MAP_FAILED)
		munmap(map, len);
	fd2 = open(p, O_RDWR);
	printf("    reopened read-write through %s: %s\n", "/proc/self/fd/N", fd2 >= 0 ? "opened" : "refused");
	if (fd2 >= 0) {
		EXPECT_EPERM("  write() on it", (int)write(fd2, &c, 1));
		close(fd2);
	}
	CHECK(pread(s->fd, buf, sizeof(buf), 0) == (ssize_t)len && !memcmp(buf, view, len));
	printf("  contents afterwards: %s\n", pread(s->fd, buf, sizeof(buf), 0) == (ssize_t)len
	       && !memcmp(buf, view, len) ? "unchanged" : "CHANGED");

	/* verify() compares with the parent's copy. */
	s->text[0] ^= 1;
	printf("  verify with the parent's copy changed: %s\n", errname(aworker_snapshot_verify(s)));
	CHECK(aworker_snapshot_verify(s) == -ESTALE);
	s->text[0] ^= 1;
	s->len--;
	printf("  verify with another size: %s\n", errname(aworker_snapshot_verify(s)));
	CHECK(aworker_snapshot_verify(s) == -ESTALE);
	s->len++;
	g_fault.armed = true;
	g_fault.get_seals_drop = F_SEAL_WRITE;
	printf("  verify with a seal missing: %s\n", errname(aworker_snapshot_verify(s)));
	CHECK(aworker_snapshot_verify(s) == -EPERM);
	fault_reset();
	fd2 = s->fd;
	aworker_snapshot_close(s);
	CHECK(!fd_open(fd2));
	aworker_snapshot_close(NULL);
}

/***********************************************************************
 * S-2
 ***********************************************************************/

static void s2_case(const char *label, int e0, int e1, int add_errno, int get_drop, int expect_rc,
		    unsigned int expect_calls)
{
	struct aworker_snapshot *s = NULL;
	unsigned int before = count_fds(), i;
	int rc;

	fault_reset();
	g_fault.armed = true;
	g_fault.memfd_errno[0] = e0;
	g_fault.memfd_errno[1] = e1;
	g_fault.add_seals_errno = add_errno;
	g_fault.get_seals_drop = get_drop;
	rc = aworker_snapshot_create(ctx, 0, view, strlen(view), &s);
	g_fault.armed = false;
	printf("  %s: %s; memfd_create() calls: %u (", label, errname(rc), g_fault.memfd_calls);
	for (i = 0; i < g_fault.memfd_calls && i < ARRAY_SIZE(g_fault.memfd_flags); i++)
		printf("%s%s", i ? ", " : "", g_fault.memfd_flags[i] & MFD_NOEXEC_SEAL ? "with NOEXEC_SEAL"
										: "without NOEXEC_SEAL");
	printf(")");
	if (s)
		printf("; seals: %s; noexec: %s", (fcntl(s->fd, F_GET_SEALS) & FOUR_SEALS) == FOUR_SEALS ? "all four"
										   : "MISSING", s->noexec ? "yes" : "no");
	printf("\n");
	CHECK(rc == expect_rc);
	CHECK(g_fault.memfd_calls == expect_calls);
	CHECK((rc == 0) == (s != NULL));
	if (s)
		CHECK((fcntl(s->fd, F_GET_SEALS) & FOUR_SEALS) == FOUR_SEALS);
	aworker_snapshot_close(s);
	/* Nothing left open, whatever happened. */
	CHECK(count_fds() == before);
	fault_reset();
}

static char d_none[300];

static void test_s2_cases(void);

static void test_s2(void)
{
	/* Here a memfd that cannot be made means no snapshot: no fallback directory is usable (S-3 to S-6 test it). */
	unsetenv("RUNTIME_DIRECTORY");
	g_fs.shm = d_none;
	g_fs.tmp = d_none;
	test_s2_cases();
	/* Restored however the cases ended. */
	g_fs.shm = NULL;
	g_fs.tmp = NULL;
	fault_reset();
}

static void test_s2_cases(void)
{
	printf("S-2: memfd flags, the EINVAL-only retry, never fewer seals\n");
	s2_case("first try", 0, 0, 0, 0, 0, 1);
	g_fault.armed = true;
	{
		struct aworker_snapshot *s;
		if (!need(aworker_snapshot_create(ctx, 0, view, strlen(view), &s), "S-2 create")) {
			fault_reset();
			return;
		}
		printf("  flags of the first try: CLOEXEC %s, ALLOW_SEALING %s, NOEXEC_SEAL %s\n",
		       g_fault.memfd_flags[0] & MFD_CLOEXEC ? "yes" : "no",
		       g_fault.memfd_flags[0] & MFD_ALLOW_SEALING ? "yes" : "no",
		       g_fault.memfd_flags[0] & MFD_NOEXEC_SEAL ? "yes" : "no");
		CHECK(g_fault.memfd_flags[0] == (MFD_CLOEXEC | MFD_ALLOW_SEALING | MFD_NOEXEC_SEAL));
		aworker_snapshot_close(s);
	}
	fault_reset();
	s2_case("NOEXEC_SEAL refused with EINVAL", EINVAL, 0, 0, 0, 0, 2);
	s2_case("EINVAL twice", EINVAL, EINVAL, 0, 0, -EINVAL, 2);
	s2_case("EMFILE (no retry)", EMFILE, 0, 0, 0, -EMFILE, 1);
	s2_case("ENOSYS (no retry; every fallback directory refused here)", ENOSYS, 0, 0, 0, -ENOSYS, 1);
	s2_case("F_ADD_SEALS fails", 0, 0, EPERM, 0, -EPERM, 1);
	s2_case("F_SEAL_SHRINK missing afterwards", 0, 0, 0, F_SEAL_SHRINK, -EPERM, 1);
	s2_case("F_SEAL_GROW missing afterwards", 0, 0, 0, F_SEAL_GROW, -EPERM, 1);
	s2_case("F_SEAL_WRITE missing afterwards", 0, 0, 0, F_SEAL_WRITE, -EPERM, 1);
	s2_case("F_SEAL_SEAL missing afterwards", 0, 0, 0, F_SEAL_SEAL, -EPERM, 1);
	{
		/* With the retry: the snapshot works without the exec seal, but never without one of the four. */
		struct aworker_snapshot *s;
		fault_reset();
		g_fault.armed = true;
		g_fault.memfd_errno[0] = EINVAL;
		if (!need(aworker_snapshot_create(ctx, 0, view, strlen(view), &s), "S-2 create after the retry")) {
			fault_reset();
			return;
		}
		g_fault.armed = false;
		printf("  after the retry (no exec seal: %s), a worker reads it:\n", s->noexec ? "NO" : "yes");
		CHECK(!s->noexec);
		worker_read("read", dup(s->fd), -1, view, strlen(view), 0);
		aworker_snapshot_close(s);
		fault_reset();
	}
}

/***********************************************************************
 * S-7, S-8
 ***********************************************************************/

static void test_s7_s8(void)
{
	struct aworker_snapshot *s = NULL;
	char *t, path[300];
	int fd, ipc[2], pp[2], i, closed, rc;
	static const struct { const char *name; int seals; } missing[] = {
		{ "F_SEAL_SHRINK", F_SEAL_SHRINK }, { "F_SEAL_GROW", F_SEAL_GROW },
		{ "F_SEAL_WRITE", F_SEAL_WRITE }, { "F_SEAL_SEAL", F_SEAL_SEAL },
	};

	printf("S-7: size limits\n");
	printf("  create, 0 octets: %s\n", errname(aworker_snapshot_create(ctx, 0, "", 0, &s)));
	CHECK(!s);
	t = big_text(AWORKER_CFG_SNAPSHOT_MAX + 1);
	printf("  create, AWORKER_CFG_SNAPSHOT_MAX + 1 octets: %s\n",
	       errname(aworker_snapshot_create(ctx, 0, t, AWORKER_CFG_SNAPSHOT_MAX + 1, &s)));
	CHECK(!s);
	rc = aworker_snapshot_create(ctx, 0, t, AWORKER_CFG_SNAPSHOT_MAX, &s);
	printf("  create, AWORKER_CFG_SNAPSHOT_MAX octets: %s\n", errname(rc));
	if (need(rc, "S-7 create, AWORKER_CFG_SNAPSHOT_MAX octets")) {
		worker_read("read, AWORKER_CFG_SNAPSHOT_MAX octets", dup(s->fd), -1, t, AWORKER_CFG_SNAPSHOT_MAX, 0);
		aworker_snapshot_close(s);
	}
	worker_read("read, AWORKER_CFG_SNAPSHOT_MAX + 1 octets (sealed elsewhere)",
		    own_memfd(t, AWORKER_CFG_SNAPSHOT_MAX + 1, FOUR_SEALS), -1, NULL, 0, -EFBIG);
	worker_read("read, 0 octets (sealed elsewhere)", own_memfd("", 0, FOUR_SEALS), -1, NULL, 0, -EINVAL);
	talloc_free(t);

	printf("S-7: what a worker accepts (S-8: the descriptor is closed afterwards, except descriptors 0, 1, 2 and"
	       " the IPC socket, which are not snapshots)\n");
	if (!need(aworker_snapshot_create(ctx, 0, view, strlen(view), &s), "S-7 create"))
		return;
	OSMO_ASSERT(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, ipc) == 0);
	worker_read("the snapshot", dup(s->fd), ipc[0], view, strlen(view), 0);
	worker_read("a memfd with all four seals made elsewhere", own_memfd(view, strlen(view), FOUR_SEALS), ipc[0],
		    view, strlen(view), 0);
	worker_read("a memfd without seals", own_memfd(view, strlen(view), 0), ipc[0], NULL, 0, -EPERM);
	for (i = 0; i < (int)ARRAY_SIZE(missing); i++) {
		char label[64];
		snprintf(label, sizeof(label), "a memfd without %s", missing[i].name);
		worker_read(label, own_memfd(view, strlen(view), FOUR_SEALS & ~missing[i].seals), ipc[0], NULL, 0, -EPERM);
	}
	snprintf(path, sizeof(path), "%s/view.cfg", g_dir);
	fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0400);
	OSMO_ASSERT(fd >= 0 && write(fd, view, strlen(view)) == (ssize_t)strlen(view));
	close(fd);
	worker_read("a linked file, read-only", open(path, O_RDONLY | O_CLOEXEC), ipc[0], NULL, 0, -EPERM);
	OSMO_ASSERT(chmod(path, 0600) == 0);
	worker_read("a linked file, writable", open(path, O_RDWR | O_CLOEXEC), ipc[0], NULL, 0, -EPERM);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	OSMO_ASSERT(fd >= 0 && unlink(path) == 0);
	worker_read("an unlinked file on a file system", fd, ipc[0], NULL, 0, -EPERM);
	OSMO_ASSERT(pipe2(pp, O_CLOEXEC) == 0);
	OSMO_ASSERT(write(pp[1], view, strlen(view)) == (ssize_t)strlen(view));
	worker_read("a pipe", pp[0], ipc[0], NULL, 0, -EINVAL);
	close(pp[1]);
	fd = dup(s->fd);
	close(fd);
	worker_read("a closed descriptor", fd, ipc[0], NULL, 0, -EBADF);
	/* A snapshot that ends before its size (cannot happen with the seals; the check is there all the same). */
	g_fault.armed = true;
	g_fault.pread_eof_at = 10;
	worker_read("a snapshot that cannot be read completely", dup(s->fd), ipc[0], NULL, 0, -EIO);
	printf("    verify of a snapshot that cannot be read completely: %s\n", errname(aworker_snapshot_verify(s)));
	CHECK(aworker_snapshot_verify(s) == -ESTALE);
	fault_reset();
	worker_read("no descriptor (-1)", -1, ipc[0], NULL, 0, -EBADF);
	closed = count_fds();
	for (i = 0; i <= 2; i++) {
		char label[64];
		snprintf(label, sizeof(label), "descriptor %d (left open)", i);
		worker_read(label, i, ipc[0], NULL, 0, -EINVAL);
	}
	worker_read("the IPC socket (left open)", ipc[0], ipc[0], NULL, 0, -EINVAL);
	CHECK(count_fds() == closed);
	close(ipc[0]);
	close(ipc[1]);
	aworker_snapshot_close(s);
}

/***********************************************************************
 * S-9
 ***********************************************************************/

static void s9_case(const char *label, int whence_off)
{
	struct aworker_snapshot *s;
	int fd, keep;
	off_t before, after;
	size_t len = strlen(view);

	if (!need(aworker_snapshot_create(ctx, 0, view, len, &s), "S-9 create"))
		return;
	fd = dup(s->fd);
	keep = dup(s->fd);	/* shares the file offset with fd */
	before = lseek(keep, whence_off < 0 ? 0 : whence_off, whence_off < 0 ? SEEK_END : SEEK_SET);
	worker_read(label, fd, -1, view, len, 0);
	after = lseek(keep, 0, SEEK_CUR);
	printf("      file offset before: %lld, after: %lld\n", (long long)before, (long long)after);
	CHECK(before == after);
	close(keep);
	aworker_snapshot_close(s);
}

static void test_s9(void)
{
	printf("S-9: pread() from offset 0, the file offset untouched\n");
	s9_case("offset at the end", -1);
	s9_case("offset in the middle", 17);
	s9_case("offset at 0", 0);
}

/***********************************************************************
 * S-10
 ***********************************************************************/

static void test_s10(void)
{
	struct aworker_snapshot *s;
	struct utsname u;
	struct stat st;
	char line[64] = "";
	FILE *f;
	int seals;

	printf("S-10: this environment (details on stderr)\n");
	fault_reset();
	g_fault.armed = true;
	if (!need(aworker_snapshot_create(ctx, 0, view, strlen(view), &s), "S-10 create")) {
		fault_reset();
		return;
	}
	g_fault.armed = false;
	seals = fcntl(s->fd, F_GET_SEALS);
	OSMO_ASSERT(fstat(s->fd, &st) == 0);
	uname(&u);
	f = fopen("/proc/sys/vm/memfd_noexec", "r");
	if (f) {
		if (!fgets(line, sizeof(line), f))
			line[0] = '\0';
		fclose(f);
	}
	line[strcspn(line, "\n")] = '\0';
	fprintf(stderr, "S-10: kernel %s; vm.memfd_noexec %s; path: %s; memfd_create() calls %u; MFD_NOEXEC_SEAL %s;"
		" seals 0x%x (exec seal: %s); mode %04o\n", u.release, line[0] ? line : "(none)",
#ifdef AWORKER_SNAPSHOT_USE_SYSCALL
		"syscall(SYS_memfd_create)",
#else
		"memfd_create()",
#endif
		g_fault.memfd_calls, s->noexec ? "accepted" : "refused (EINVAL), retried without", seals,
		seals & F_SEAL_EXEC ? "yes" : "no", (unsigned int)(st.st_mode & 07777));
	/* With MFD_NOEXEC_SEAL the kernel adds the exec seal and leaves no exec bit. */
	if (s->noexec)
		CHECK((seals & F_SEAL_EXEC) && !(st.st_mode & 0111));
	CHECK((seals & FOUR_SEALS) == FOUR_SEALS);
	printf("  checked\n");
	aworker_snapshot_close(s);
	fault_reset();
}

/***********************************************************************
 * S-3 to S-6: the fallback (D-4)
 ***********************************************************************/

/* Directories the tests use as candidates; d_none does not exist. */
static char d_ok[300], d_left[300], d_link[300], d_file[300], d_w777[300], d_w770[300], d_sticky[300], d_cwd[300];

static void fs_setup(void)
{
	int fd;

	snprintf(d_none, sizeof(d_none), "%s/none", g_dir);
	snprintf(d_ok, sizeof(d_ok), "%s/ok", g_dir);
	snprintf(d_left, sizeof(d_left), "%s/left", g_dir);
	snprintf(d_link, sizeof(d_link), "%s/link", g_dir);
	snprintf(d_file, sizeof(d_file), "%s/file", g_dir);
	snprintf(d_w777, sizeof(d_w777), "%s/w777", g_dir);
	snprintf(d_w770, sizeof(d_w770), "%s/w770", g_dir);
	snprintf(d_sticky, sizeof(d_sticky), "%s/sticky", g_dir);
	snprintf(d_cwd, sizeof(d_cwd), "%s/cwd", g_dir);
	OSMO_ASSERT(mkdir(d_ok, 0700) == 0 && chmod(d_ok, 0700) == 0);
	OSMO_ASSERT(mkdir(d_left, 0700) == 0 && chmod(d_left, 0700) == 0);
	OSMO_ASSERT(symlink(d_ok, d_link) == 0);
	fd = open(d_file, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	OSMO_ASSERT(fd >= 0);
	close(fd);
	OSMO_ASSERT(mkdir(d_w777, 0700) == 0 && chmod(d_w777, 0777) == 0);
	OSMO_ASSERT(mkdir(d_w770, 0700) == 0 && chmod(d_w770, 0770) == 0);
	OSMO_ASSERT(mkdir(d_sticky, 0700) == 0 && chmod(d_sticky, 01777) == 0);
	OSMO_ASSERT(mkdir(d_cwd, 0700) == 0);
}

/* Names in dir starting with the private directories' prefix. */
static unsigned int count_prefixed(const char *dir)
{
	unsigned int n = 0;
	struct dirent *de;
	DIR *d = opendir(dir);

	if (!d)
		return 0;
	while ((de = readdir(d)))
		n += !strncmp(de->d_name, "bsc-aworker-snap.", 17);
	closedir(d);
	return n;
}

/* The candidate the last fallback used, from its WARNING ("... under <dir> instead."), as a label. */
static const char *used_label(char *buf, size_t len)
{
	const char *p = strstr(g_log, "private directory under "), *e;

	if (!p)
		return "none";
	p += strlen("private directory under ");
	e = strstr(p, " instead.");
	if (!e)
		return "?";
	snprintf(buf, len, "%.*s", (int)(e - p), p);
	if (!strcmp(buf, d_ok))
		return "the test's directory 'ok'";
	if (!strcmp(buf, d_left))
		return "the test's directory 'left'";
	if (!strcmp(buf, d_w777))
		return "the test's directory 'w777'";
	if (!strcmp(buf, d_w770))
		return "the test's directory 'w770'";
	if (!strcmp(buf, d_sticky))
		return "the test's directory 'sticky'";
	if (!strcmp(buf, "/dev/shm"))
		return "/dev/shm";
	if (!strcmp(buf, "/tmp"))
		return "/tmp";
	return "an unexpected directory";
}

/* With the memfd failing (ENOSYS), $RUNTIME_DIRECTORY as given (NULL: unset), /dev/shm and /tmp leading where given
 * (NULL: the real ones): make worker 5's snapshot. Expect it in the candidate labelled expect (NULL: no snapshot,
 * the memfd's error returned). Checks that nothing is left open or behind. \returns the snapshot, or NULL. */
static struct aworker_snapshot *fallback_try(const char *label, const char *runtime, const char *shm, const char *tmp,
					     const char *expect)
{
	struct aworker_snapshot *s = NULL;
	unsigned int before = count_fds(), ok_before = count_prefixed(d_ok);
	char buf[PATH_MAX];
	const char *used;
	int rc;

	if (runtime)
		setenv("RUNTIME_DIRECTORY", runtime, 1);
	else
		unsetenv("RUNTIME_DIRECTORY");
	g_fs.shm = shm;
	g_fs.tmp = tmp;
	fs_observed_reset();
	log_clear();
	fault_reset();
	g_fault.armed = true;
	g_fault.memfd_errno[0] = ENOSYS;
	rc = aworker_snapshot_create(ctx, 5, view, strlen(view), &s);
	g_fault.armed = false;
	used = rc ? "none" : used_label(buf, sizeof(buf));
	printf("  %s: %s%s%s\n", label, errname(rc), rc ? "" : ", in ", rc ? "" : used);
	if (expect) {
		CHECK(rc == 0 && s && s->fallback && !strcmp(used, expect));
		CHECK(log_count("WARNING: worker 5: no sealed memfd") == 1);
	} else {
		CHECK(rc == -ENOSYS && !s);
		CHECK(strstr(g_log, "no fallback directory could be used"));
		CHECK(log_count("WARNING: worker 5:") == 0);
		CHECK(count_fds() == before);
	}
	if (s)
		CHECK(count_fds() == before + 1);
	/* No private directory is left behind in the test's directory. */
	CHECK(count_prefixed(d_ok) == ok_before);
	g_fs.shm = NULL;
	g_fs.tmp = NULL;
	unsetenv("RUNTIME_DIRECTORY");
	fault_reset();
	return s;
}

/* reason was logged for candidate path; print it under label. */
static void expect_refused_dir(const char *label, const char *path, const char *reason)
{
	char needle[PATH_MAX + 256];

	snprintf(needle, sizeof(needle), "not using %s: %s", path, reason);
	printf("    %s refused: %s\n", label, strstr(g_log, needle) ? reason : "NOT AS EXPECTED");
	CHECK(strstr(g_log, needle));
}

/* For the cases that need the fallback's file: a sealed memfd there is a failure (already reported by fallback_try()),
 * and the rest of the case is skipped, which closes it. */
static bool fallback_only(struct aworker_snapshot *s)
{
	if (s->fallback)
		return true;
	printf("  FAIL not the fallback's file; the rest of this case is skipped\n");
	failures++;
	aworker_snapshot_close(s);
	return false;
}

/* Candidate path refused for its owner (uid as fstat() reports it); print it under label. */
static void expect_refused_owner(const char *label, const char *path, unsigned int owner)
{
	char needle[PATH_MAX + 256];

	snprintf(needle, sizeof(needle), "not using %s: it is owned by uid %u, not by uid %u, and it is not a sticky"
		 " directory owned by root", path, owner, (unsigned int)geteuid());
	printf("    %s refused: %s\n", label, strstr(g_log, needle) ? "not this user's, and not a sticky directory owned"
		 " by root" : "NOT AS EXPECTED");
	CHECK(strstr(g_log, needle));
}

/* An accepted candidate of the test: its private directory was removed again. */
static void expect_clean(const char *dir)
{
	CHECK(count_prefixed(dir) == 0);
}

static void close_snap(struct aworker_snapshot *s)
{
	unsigned int before = count_fds();

	if (!s)
		return;
	aworker_snapshot_close(s);
	CHECK(count_fds() == before - 1);
}

static void test_s3_s4(void)
{
	struct aworker_snapshot *s;
	char expect[1024], rel[] = "relative/dir", list[700];
	const char *tmpdir = getenv("TMPDIR");
	char *saved_tmpdir = tmpdir ? talloc_strdup(ctx, tmpdir) : NULL;

	printf("S-3: the fallback's WARNING\n");
	log_clear();
	if (need(aworker_snapshot_create(ctx, 5, view, strlen(view), &s), "S-3 create")) {
		printf("  sealed memfd available: fallback %s, WARNING %s\n", s->fallback ? "USED" : "not used",
		       strstr(g_log, "WARNING") ? "LOGGED" : "not logged");
		CHECK(!s->fallback && !strstr(g_log, "WARNING"));
		aworker_snapshot_close(s);
	}
	s = fallback_try("no memfd, $RUNTIME_DIRECTORY usable", d_ok, NULL, NULL, "the test's directory 'ok'");
	if (s) {
		snprintf(expect, sizeof(expect), "WARNING: worker 5: no sealed memfd for the configuration snapshot (%s);"
			 " using an unlinked, read-only file made in a private directory under %s instead. This is"
			 " WEAKER than a sealed memfd: another process of the same user (uid %u) can reopen it through"
			 " /proc/%d/fd/%d, make it writable and change it, or read it, and root can do anything. It is"
			 " compared with the configuration before every worker start.\n", strerror(ENOSYS), d_ok,
			 (unsigned int)geteuid(), (int)getpid(), s->fd);
		printf("  WARNING, word for word as specified: %s\n", strstr(g_log, expect) ? "yes" : "NO");
		CHECK(strstr(g_log, expect));
		close_snap(s);
	}

	printf("S-4: the candidate directories, in order\n");
	close_snap(fallback_try("$RUNTIME_DIRECTORY, /dev/shm and /tmp usable", d_ok, NULL, NULL,
				"the test's directory 'ok'"));
	close_snap(fallback_try("$RUNTIME_DIRECTORY not set", NULL, NULL, NULL, "/dev/shm"));
	close_snap(fallback_try("$RUNTIME_DIRECTORY not set, /dev/shm missing", NULL, d_none, NULL, "/tmp"));
	snprintf(list, sizeof(list), "%s:/somewhere/else", d_ok);
	close_snap(fallback_try("$RUNTIME_DIRECTORY with two entries: the first one", list, d_none, d_none,
				"the test's directory 'ok'"));
	setenv("TMPDIR", d_ok, 1);
	close_snap(fallback_try("$TMPDIR usable, every candidate unusable: $TMPDIR is not used", NULL, d_none, d_none,
				NULL));
	if (saved_tmpdir)
		setenv("TMPDIR", saved_tmpdir, 1);
	else
		unsetenv("TMPDIR");
	talloc_free(saved_tmpdir);

	close_snap(fallback_try("$RUNTIME_DIRECTORY a symbolic link to a usable directory", d_link, d_none, d_none, NULL));
	expect_refused_dir("the symbolic link", d_link, "it is a symbolic link");
	close_snap(fallback_try("$RUNTIME_DIRECTORY a file", d_file, d_none, d_none, NULL));
	expect_refused_dir("the file", d_file, "it is not a directory");
	/* Ownership (D-4): a directory of this user whatever its mode; otherwise only a sticky directory of root.
	 * Owners other than this user are what fstat() reports (fake_owner(): files of other users cannot be made
	 * without root). */
	close_snap(fallback_try("$RUNTIME_DIRECTORY mode 0777, ours", d_w777, d_none, d_none,
				"the test's directory 'w777'"));
	expect_clean(d_w777);
	close_snap(fallback_try("$RUNTIME_DIRECTORY mode 0770, ours", d_w770, d_none, d_none,
				"the test's directory 'w770'"));
	expect_clean(d_w770);
	close_snap(fallback_try("$RUNTIME_DIRECTORY mode 1777 (sticky), ours", d_sticky, d_none, d_none,
				"the test's directory 'sticky'"));
	expect_clean(d_sticky);
	fake_owner(d_ok, -1, 0);
	close_snap(fallback_try("$RUNTIME_DIRECTORY mode 0700, root's, not sticky", d_ok, d_none, d_none, NULL));
	expect_refused_owner("root's directory that is not sticky", d_ok, 0);
	fake_owner(d_w777, -1, 0);
	close_snap(fallback_try("$RUNTIME_DIRECTORY mode 0777, root's, not sticky", d_w777, d_none, d_none, NULL));
	expect_refused_owner("root's writable directory that is not sticky", d_w777, 0);
	fake_owner(d_sticky, -1, 0);
	close_snap(fallback_try("$RUNTIME_DIRECTORY mode 1777 (sticky), root's", d_sticky, d_none, d_none,
				"the test's directory 'sticky'"));
	expect_clean(d_sticky);
	fake_owner(d_sticky, -1, 4242);
	close_snap(fallback_try("$RUNTIME_DIRECTORY mode 1777 (sticky), another user's", d_sticky, d_none, d_none,
				NULL));
	expect_refused_owner("another user's sticky directory", d_sticky, 4242);
	fake_owner(d_w777, -1, 4242);
	close_snap(fallback_try("$RUNTIME_DIRECTORY mode 0777, another user's, not sticky", d_w777, d_none, d_none,
				NULL));
	expect_refused_owner("another user's writable directory", d_w777, 4242);
	fake_owner(d_ok, -1, 4242);
	close_snap(fallback_try("$RUNTIME_DIRECTORY mode 0700, another user's", d_ok, d_none, d_none, NULL));
	expect_refused_owner("another user's directory", d_ok, 4242);
	g_fs.fake_uid = false;
	close_snap(fallback_try("$RUNTIME_DIRECTORY relative", rel, d_none, d_none, NULL));
	printf("    the relative path refused: %s\n", strstr(g_log, "not using $RUNTIME_DIRECTORY 'relative/dir': not"
								" an absolute path") ? "not an absolute path" : "NOT AS EXPECTED");
	CHECK(strstr(g_log, "not using $RUNTIME_DIRECTORY 'relative/dir': not an absolute path"));
	close_snap(fallback_try("$RUNTIME_DIRECTORY empty", "", d_none, d_none, NULL));
	CHECK(strstr(g_log, "not using $RUNTIME_DIRECTORY '': not an absolute path"));
	fake_owner(d_w777, -1, 4242);
	close_snap(fallback_try("every candidate refused: a symbolic link, a file, another user's writable directory",
				d_link, d_file, d_w777, NULL));
	g_fs.fake_uid = false;
	expect_refused_dir("$RUNTIME_DIRECTORY", d_link, "it is a symbolic link");
	/* The module knows these two by their names only (the test redirects what they open). */
	expect_refused_dir("/dev/shm", "/dev/shm", "it is not a directory");
	expect_refused_owner("/tmp", "/tmp", 4242);
}

/* The fallback's snapshot s (worker 5, the view) as S-5 requires it. */
static void check_fallback_file(struct aworker_snapshot *s, bool named)
{
	char buf[512], c = 'X';
	struct stat st;
	int fl, n;
	size_t len = strlen(view);
	bool name_ok;

	OSMO_ASSERT(fstat(s->fd, &st) == 0);
	fl = fcntl(s->fd, F_GETFL);
	name_ok = !strncmp(g_fs.mkdirat_name, "bsc-aworker-snap.", 17) && strlen(g_fs.mkdirat_name) == 17 + 12
		  && strspn(g_fs.mkdirat_name + 17, "0123456789abcdef") == 12;
	printf("    private directory: named bsc-aworker-snap.<12 hex digits>: %s; created with mode %04o; mode %04o and"
	       " %s when the file was made\n", name_ok ? "yes" : "NO", (unsigned int)g_fs.mkdirat_mode,
	       (unsigned int)g_fs.dir_mode, g_fs.dir_uid == geteuid() ? "ours" : "NOT OURS");
	CHECK(name_ok && g_fs.mkdirat_mode == 0700 && g_fs.dir_mode == 0700 && g_fs.dir_uid == geteuid());
	if (named) {
		printf("    file created with a name: O_CREAT %s, O_EXCL %s, O_NOFOLLOW %s\n",
		       g_fs.file_flags & O_CREAT ? "yes" : "no", g_fs.file_flags & O_EXCL ? "yes" : "no",
		       g_fs.file_flags & O_NOFOLLOW ? "yes" : "no");
		CHECK((g_fs.file_flags & (O_CREAT | O_EXCL | O_NOFOLLOW)) == (O_CREAT | O_EXCL | O_NOFOLLOW));
	} else {
		printf("    file created with O_TMPFILE: %s\n", (g_fs.file_flags & O_TMPFILE) == O_TMPFILE ? "yes" : "no");
		CHECK((g_fs.file_flags & O_TMPFILE) == O_TMPFILE);
	}
	printf("    snapshot: regular file %s, links %lu, mode %04o, %s, read-only descriptor %s, the file created %s\n",
	       S_ISREG(st.st_mode) ? "yes" : "no", (unsigned long)st.st_nlink, (unsigned int)(st.st_mode & 07777),
	       st.st_uid == geteuid() ? "ours" : "NOT OURS", (fl & O_ACCMODE) == O_RDONLY ? "yes" : "no",
	       st.st_dev == g_fs.file_dev && st.st_ino == g_fs.file_ino ? "yes" : "NO");
	CHECK(S_ISREG(st.st_mode) && st.st_nlink == 0 && (st.st_mode & 07777) == 0400 && st.st_uid == geteuid());
	CHECK((fl & O_ACCMODE) == O_RDONLY);
	CHECK(st.st_dev == g_fs.file_dev && st.st_ino == g_fs.file_ino);
	errno = 0;
	n = (int)write(s->fd, &c, 1);
	printf("    write() on it: %s; contents: %s; verify: %s\n", n < 0 && errno == EBADF ? "EBADF" : "ALLOWED",
	       pread(s->fd, buf, sizeof(buf), 0) == (ssize_t)len && !memcmp(buf, view, len) ? "the view" : "WRONG",
	       errname(aworker_snapshot_verify(s)));
	CHECK(n < 0 && errno == EBADF);
	CHECK(pread(s->fd, buf, sizeof(buf), 0) == (ssize_t)len && !memcmp(buf, view, len));
	CHECK(s->fallback && aworker_snapshot_verify(s) == 0);
	printf("    private directory removed: %s\n", count_prefixed(d_ok) == 0 ? "yes" : "NO");
	CHECK(count_prefixed(d_ok) == 0);
	worker_read("a worker reads it", dup(s->fd), -1, view, len, 0);
}

static void test_s5(void)
{
	struct aworker_snapshot *s;
	char p[64], c;
	int fd, fd2;

	printf("S-5: the fallback's file\n");
	printf("  with O_TMPFILE:\n");
	s = fallback_try("made", d_ok, d_none, d_none, "the test's directory 'ok'");
	if (s && fallback_only(s)) {
		check_fallback_file(s, false);
		close_snap(s);
	}
	printf("  with a name (O_TMPFILE not supported):\n");
	g_fs.no_tmpfile = true;
	s = fallback_try("made", d_ok, d_none, d_none, "the test's directory 'ok'");
	g_fs.no_tmpfile = false;
	if (s && fallback_only(s)) {
		check_fallback_file(s, true);
		close_snap(s);
	}
	printf("  with a name (O_TMPFILE made, but no read-only descriptor of it through /proc):\n");
	g_fs.proc_reopen = 1;
	s = fallback_try("made", d_ok, d_none, d_none, "the test's directory 'ok'");
	g_fs.proc_reopen = 0;
	if (s && fallback_only(s)) {
		check_fallback_file(s, true);
		close_snap(s);
	}
	printf("  with a name (the read-only descriptor through /proc is another file):\n");
	g_fs.proc_reopen = 2;
	g_fs.other_file = d_file;
	s = fallback_try("made", d_ok, d_none, d_none, "the test's directory 'ok'");
	g_fs.proc_reopen = 0;
	if (s && fallback_only(s)) {
		printf("    another file refused: %s\n", strstr(g_log, "the read-only descriptor is not the file written")
		       ? "yes" : "NO");
		CHECK(strstr(g_log, "the read-only descriptor is not the file written"));
		check_fallback_file(s, true);
		close_snap(s);
	}
	printf("  a private directory name already taken once:\n");
	g_fs.mkdir_eexist = 1;
	s = fallback_try("made", d_ok, d_none, d_none, "the test's directory 'ok'");
	printf("    mkdirat() calls: %u\n", g_fs.mkdirat_calls);
	CHECK(g_fs.mkdirat_calls == 2);
	g_fs.mkdir_eexist = 0;
	close_snap(s);
	printf("  no file can be made at all (neither O_TMPFILE nor a name):\n");
	g_fs.no_tmpfile = true;
	g_fs.no_named = true;
	close_snap(fallback_try("in the test's directory", d_ok, d_none, d_none, NULL));
	g_fs.no_tmpfile = false;
	g_fs.no_named = false;
	printf("    private directory removed: %s\n", count_prefixed(d_ok) == 0 ? "yes" : "NO");
	CHECK(count_prefixed(d_ok) == 0);
	printf("  the named file made, but no read-only descriptor of it:\n");
	g_fs.no_tmpfile = true;
	g_fs.no_named_reopen = true;
	close_snap(fallback_try("in the test's directory", d_ok, d_none, d_none, NULL));
	g_fs.no_tmpfile = false;
	g_fs.no_named_reopen = false;
	printf("    its name and the private directory removed: %s\n", count_prefixed(d_ok) == 0 ? "yes" : "NO");
	CHECK(count_prefixed(d_ok) == 0);

	/* Changed after it was made (same user): verify refuses it, and so would a worker. */
	printf("  changed after it was made (same user, through /proc):\n");
	s = fallback_try("made", d_ok, d_none, d_none, "the test's directory 'ok'");
	if (s && fallback_only(s)) {
		snprintf(p, sizeof(p), "/proc/self/fd/%d", s->fd);
		fd = open(p, O_RDONLY | O_CLOEXEC);
		OSMO_ASSERT(fd >= 0 && fchmod(fd, 0600) == 0);
		printf("    made writable: verify %s\n", errname(aworker_snapshot_verify(s)));
		CHECK(aworker_snapshot_verify(s) == -EPERM);
		worker_read("    a worker reads it", dup(s->fd), -1, NULL, 0, -EPERM);
		fd2 = open(p, O_RDWR | O_CLOEXEC);
		OSMO_ASSERT(fd2 >= 0);
		c = 'X';
		OSMO_ASSERT(pwrite(fd2, &c, 1, 0) == 1);
		close(fd2);
		OSMO_ASSERT(fchmod(fd, 0400) == 0);
		close(fd);
		printf("    contents changed, mode 0400 again: verify %s\n", errname(aworker_snapshot_verify(s)));
		CHECK(aworker_snapshot_verify(s) == -ESTALE);
		close_snap(s);
	}
	printf("  held on a read-write descriptor instead of its read-only one:\n");
	s = fallback_try("made", d_ok, d_none, d_none, "the test's directory 'ok'");
	if (s && fallback_only(s)) {
		int ro = s->fd;
		snprintf(p, sizeof(p), "/proc/self/fd/%d", ro);
		fd = open(p, O_RDONLY | O_CLOEXEC);
		OSMO_ASSERT(fd >= 0 && fchmod(fd, 0600) == 0);
		fd2 = open(p, O_RDWR | O_CLOEXEC);
		OSMO_ASSERT(fd2 >= 0 && fchmod(fd, 0400) == 0);
		close(fd);
		s->fd = fd2;
		printf("    verify: %s\n", errname(aworker_snapshot_verify(s)));
		CHECK(aworker_snapshot_verify(s) == -EPERM);
		s->fd = ro;
		close(fd2);
		close_snap(s);
	}
	/* Files like it that a worker does not accept. */
	printf("  what a worker refuses that looks like the fallback's file:\n");
	{
		char path[PATH_MAX];
		snprintf(path, sizeof(path), "%s/like", d_ok);
		fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
		OSMO_ASSERT(fd >= 0 && write(fd, view, strlen(view)) == (ssize_t)strlen(view));
		OSMO_ASSERT(fchmod(fd, 0400) == 0);
		fd2 = open(path, O_RDONLY | O_CLOEXEC);
		OSMO_ASSERT(fd2 >= 0);
		worker_read("mode 0400, read-only descriptor, but still linked", fd2, -1, NULL, 0, -EPERM);
		OSMO_ASSERT(unlink(path) == 0);
		worker_read("mode 0400, unlinked, but a read-write descriptor", fd, -1, NULL, 0, -EPERM);
		fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
		OSMO_ASSERT(fd >= 0 && write(fd, view, strlen(view)) == (ssize_t)strlen(view));
		OSMO_ASSERT(fchmod(fd, 0400) == 0);
		fd2 = open(path, O_RDONLY | O_CLOEXEC);
		OSMO_ASSERT(fd2 >= 0 && unlink(path) == 0);
		close(fd);
		fake_owner(NULL, fd2, 4242);
		worker_read("mode 0400, unlinked, read-only descriptor, but another user's", fd2, -1, NULL, 0, -EPERM);
		g_fs.fake_uid = false;
	}
	printf("  another user's by the time it is checked:\n");
	s = fallback_try("made", d_ok, d_none, d_none, "the test's directory 'ok'");
	if (s && fallback_only(s)) {
		fake_owner(NULL, s->fd, 4242);
		printf("    verify: %s\n", errname(aworker_snapshot_verify(s)));
		CHECK(aworker_snapshot_verify(s) == -EPERM);
		g_fs.fake_uid = false;
		close_snap(s);
	}
}

/* S-6: private directories an earlier process left behind are reported, and nothing about them changes. */
static void test_s6(void)
{
	static const char *const names[] = { "bsc-aworker-snap.left1", "bsc-aworker-snap.left2",
					     "bsc-aworker-snap.link", "bsc-aworker-snap.file", "other-dir" };
	struct stat before[5], after[5];
	struct aworker_snapshot *s;
	char path[PATH_MAX], needle[PATH_MAX + 64], buf[16];
	unsigned int i;
	bool same = true;
	int fd;

	printf("S-6: directories left over by an earlier process\n");
	snprintf(path, sizeof(path), "%s/%s", d_left, names[0]);
	OSMO_ASSERT(mkdir(path, 0700) == 0);
	snprintf(path, sizeof(path), "%s/%s/snapshot", d_left, names[0]);
	fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0400);
	OSMO_ASSERT(fd >= 0 && write(fd, "old\n", 4) == 4);
	close(fd);
	snprintf(path, sizeof(path), "%s/%s", d_left, names[1]);
	OSMO_ASSERT(mkdir(path, 0700) == 0);
	snprintf(path, sizeof(path), "%s/%s", d_left, names[2]);
	OSMO_ASSERT(symlink(d_ok, path) == 0);
	snprintf(path, sizeof(path), "%s/%s", d_left, names[3]);
	fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	OSMO_ASSERT(fd >= 0);
	close(fd);
	snprintf(path, sizeof(path), "%s/%s", d_left, names[4]);
	OSMO_ASSERT(mkdir(path, 0700) == 0);
	for (i = 0; i < ARRAY_SIZE(names); i++) {
		snprintf(path, sizeof(path), "%s/%s", d_left, names[i]);
		OSMO_ASSERT(lstat(path, &before[i]) == 0);
	}

	s = fallback_try("made in a directory with left-overs", d_left, d_none, d_none, "the test's directory 'left'");
	for (i = 0; i < ARRAY_SIZE(names); i++) {
		bool reported;
		snprintf(needle, sizeof(needle), "WARNING: %s/%s is left over", d_left, names[i]);
		reported = strstr(g_log, needle);
		printf("    %s: %s\n", names[i], reported ? "reported" : "not reported");
		CHECK(reported == (i < 2));
	}
	printf("    left-over warnings: %u\n", log_count(" is left over from an earlier configuration snapshot"));
	CHECK(log_count(" is left over from an earlier configuration snapshot") == 2);
	for (i = 0; i < ARRAY_SIZE(names); i++) {
		snprintf(path, sizeof(path), "%s/%s", d_left, names[i]);
		if (lstat(path, &after[i]) < 0 || after[i].st_ino != before[i].st_ino
		    || after[i].st_mode != before[i].st_mode
		    || after[i].st_mtim.tv_sec != before[i].st_mtim.tv_sec
		    || after[i].st_mtim.tv_nsec != before[i].st_mtim.tv_nsec)
			same = false;
	}
	snprintf(path, sizeof(path), "%s/%s/snapshot", d_left, names[0]);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0 || read(fd, buf, sizeof(buf)) != 4 || memcmp(buf, "old\n", 4))
		same = false;
	if (fd >= 0)
		close(fd);
	printf("    all of them unchanged afterwards (inode, mode, modification time, contents): %s\n",
	       same ? "yes" : "NO");
	CHECK(same);
	printf("    the new private directory removed: %s\n", count_prefixed(d_left) == 4 ? "yes" : "NO");
	CHECK(count_prefixed(d_left) == 4);
	close_snap(s);
}

int main(int argc, char **argv)
{
	char cmd[300];

	static const struct log_info log_info = {
		.cat = osmo_bsc_categories,
		.num_cat = Debug_LastEntry,
	};
	struct log_target *capture;

	ctx = talloc_named_const(NULL, 0, "aworker_snapshot_test");
	setvbuf(stdout, NULL, _IOLBF, 0);
	snprintf(g_dir, sizeof(g_dir), "%s/aworker_snapshot.XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
	OSMO_ASSERT(mkdtemp(g_dir));
	fs_setup();
	/* Nothing the module does may depend on the working directory: run in a scratch one. */
	OSMO_ASSERT(chdir(d_cwd) == 0);

	/* The module's log messages (A-worker category) are captured for the checks; stderr gets them too. */
	osmo_init_logging2(ctx, &log_info);
	capture = log_target_create();
	OSMO_ASSERT(capture);
	capture->output = log_capture;
	log_set_print_filename2(capture, LOG_FILENAME_NONE);
	log_set_print_category(capture, 0);
	log_set_print_category_hex(capture, 0);
	log_set_print_level(capture, 0);
	log_set_print_timestamp(capture, 0);
	log_set_use_color(capture, 0);
	log_set_all_filter(capture, 1);
	log_set_category_filter(capture, aworker_log_cat, 1, LOGL_DEBUG);
	log_add_target(capture);
	unsetenv("RUNTIME_DIRECTORY");

	test_s1();
	test_s2();
	test_s7_s8();
	test_s9();
	test_s10();
	test_s3_s4();
	test_s5();
	test_s6();

	if (!failures) {
		OSMO_ASSERT(chdir("/") == 0);
		snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_dir);
		if (system(cmd)) {}
	} else {
		printf("files kept in %s\n", g_dir);
	}
	printf("%s\n", failures ? "FAILED" : "PASSED");
	talloc_free(ctx);
	return failures ? 1 : 0;
}
