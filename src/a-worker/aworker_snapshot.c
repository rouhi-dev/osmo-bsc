/* A-worker pool: worker configuration snapshots (see aworker_snapshot.h) */
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

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "bscconfig.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <osmocom/core/logging.h>
#include <osmocom/core/talloc.h>
#include <osmocom/core/utils.h>

#include <osmocom/bsc/aworker_cfg_view.h>
#include <osmocom/bsc/aworker_log.h>
#include <osmocom/bsc/aworker_snapshot.h>

/* Older C library headers may lack these; the values are the kernel's ABI. */
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC		0x0001U
#endif
#ifndef MFD_ALLOW_SEALING
#define MFD_ALLOW_SEALING	0x0002U
#endif
#ifndef MFD_NOEXEC_SEAL
#define MFD_NOEXEC_SEAL		0x0008U
#endif
#ifndef F_ADD_SEALS
#define F_ADD_SEALS		1033
#define F_GET_SEALS		1034
#define F_SEAL_SEAL		0x0001
#define F_SEAL_SHRINK		0x0002
#define F_SEAL_GROW		0x0004
#define F_SEAL_WRITE		0x0008
#endif

#ifndef O_TMPFILE
#define O_TMPFILE		(020000000 | O_DIRECTORY)
#endif

/* The four seals every snapshot must have (D-12). */
#define SNAP_SEALS	(F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL)

/* The fallback (D-4): the private directories' name prefix, and the name of the file in it when O_TMPFILE cannot be
 * used. */
#define SNAP_DIR_PREFIX	"bsc-aworker-snap."
#define SNAP_FILE	"snapshot"

/* memfd_create() of the C library if it has one (configure: HAVE_MEMFD_CREATE), otherwise the system call itself.
 * AWORKER_SNAPSHOT_USE_SYSCALL makes a build take the system call even so, so that this path is compiled and tested
 * too (tests/aworker_snapshot). */
static int snap_memfd_create(const char *name, unsigned int flags)
{
#if defined(HAVE_MEMFD_CREATE) && !defined(AWORKER_SNAPSHOT_USE_SYSCALL)
	return memfd_create(name, flags);
#elif defined(SYS_memfd_create)
	return (int)syscall(SYS_memfd_create, name, flags);
#else
	errno = ENOSYS;
	return -1;
#endif
}

static ssize_t pwrite_full(int fd, const char *buf, size_t len)
{
	size_t done = 0;

	while (done < len) {
		ssize_t n = pwrite(fd, buf + done, len - done, (off_t)done);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return -1;
		done += (size_t)n;
	}
	return (ssize_t)done;
}

/* Read len octets from offset 0 with pread(); \returns the number read (short only at end-of-file), -1 on error. */
static ssize_t pread_full(int fd, char *buf, size_t len)
{
	size_t got = 0;

	while (got < len) {
		ssize_t n = pread(fd, buf + got, len - got, (off_t)got);
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0)
			return -1;
		if (!n)
			break;
		got += (size_t)n;
	}
	return (ssize_t)got;
}

/***********************************************************************
 * Fallback (D-4): an unlinked, read-only file made in a private directory
 ***********************************************************************/

/* Open candidate directory path and check it. \returns its descriptor, or -1 with the reason in why. */
static int open_candidate(const char *path, char *why, size_t why_len)
{
	uid_t euid = geteuid();
	struct stat st;
	int dfd;

	dfd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC, 0);
	if (dfd < 0) {
		int err = errno;
		struct stat l;
		/* Refused by the open() above; lstat() only names the reason (a symbolic link gives ELOOP or, with
		 * O_DIRECTORY, ENOTDIR). */
		if ((err == ELOOP || err == ENOTDIR) && lstat(path, &l) == 0 && S_ISLNK(l.st_mode))
			snprintf(why, why_len, "it is a symbolic link");
		else if (err == ENOTDIR)
			snprintf(why, why_len, "it is not a directory");
		else
			snprintf(why, why_len, "%s", strerror(err));
		return -1;
	}
	if (fstat(dfd, &st) < 0 || !S_ISDIR(st.st_mode)) {
		snprintf(why, why_len, "it is not a directory");
		goto refuse;
	}
	/* D-4: a directory of this user, whatever its mode; otherwise only a sticky directory owned by root. */
	if (st.st_uid != euid && !(st.st_uid == 0 && (st.st_mode & S_ISVTX))) {
		snprintf(why, why_len, "it is owned by uid %u, not by uid %u, and it is not a sticky directory owned by"
			 " root", (unsigned int)st.st_uid, (unsigned int)euid);
		goto refuse;
	}
	return dfd;

refuse:
	close(dfd);
	return -1;
}

/* Directories of earlier snapshots in dfd (of a process that did not get to remove its own): reported, never touched.
 * Only directories of this user with the prefix; nothing is followed. */
static void report_leftovers(int dfd, const char *path)
{
	struct dirent *de;
	DIR *dir;
	int d2;

	d2 = fcntl(dfd, F_DUPFD_CLOEXEC, 0);
	if (d2 < 0)
		return;
	dir = fdopendir(d2);
	if (!dir) {
		close(d2);
		return;
	}
	while ((de = readdir(dir))) {
		struct stat st;
		if (strncmp(de->d_name, SNAP_DIR_PREFIX, strlen(SNAP_DIR_PREFIX)))
			continue;
		if (fstatat(dfd, de->d_name, &st, AT_SYMLINK_NOFOLLOW) < 0 || !S_ISDIR(st.st_mode)
		    || st.st_uid != geteuid())
			continue;
		LOGP(aworker_log_cat, LOGL_NOTICE, "WARNING: %s/%s is left over from an earlier configuration snapshot of"
		     " uid %u (its process ended before removing it). It is not removed; remove it once no osmo-bsc or"
		     " bsc-aworker of this user can be using it.\n", path, de->d_name, (unsigned int)geteuid());
	}
	closedir(dir);
}

/* In private directory pd: the file with the view, written, made 0400, and a read-only descriptor of exactly that file,
 * which no name refers to any more. named: create it with a name (O_CREAT | O_EXCL) and remove the name, instead of
 * O_TMPFILE. \returns the read-only descriptor, or -1 with the reason in why; nothing is left behind on failure. */
static int fallback_file(int pd, bool named, const char *text, size_t len, char *why, size_t why_len)
{
	uid_t euid = geteuid();
	struct stat w, r;
	char proc[64];
	int wfd, rofd = -1;
	bool linked = false;

	if (named) {
		wfd = openat(pd, SNAP_FILE, O_CREAT | O_EXCL | O_NOFOLLOW | O_RDWR | O_CLOEXEC, 0600);
		linked = wfd >= 0;
	} else {
		wfd = openat(pd, ".", O_TMPFILE | O_RDWR | O_CLOEXEC, 0600);
	}
	if (wfd < 0) {
		snprintf(why, why_len, "cannot create the file (%s): %s", named ? "O_CREAT | O_EXCL" : "O_TMPFILE",
			 strerror(errno));
		return -1;
	}
	if (pwrite_full(wfd, text, len) != (ssize_t)len) {
		snprintf(why, why_len, "cannot write the file: %s", strerror(errno));
		goto fail;
	}
	if (fchmod(wfd, 0400) < 0 || fstat(wfd, &w) < 0) {
		snprintf(why, why_len, "cannot make the file read-only: %s", strerror(errno));
		goto fail;
	}
	/* A read-only descriptor of the very same file: by its name in pd, or (O_TMPFILE, which has none) by the
	 * descriptor itself. */
	if (named) {
		rofd = openat(pd, SNAP_FILE, O_RDONLY | O_NOFOLLOW | O_CLOEXEC, 0);
	} else {
		snprintf(proc, sizeof(proc), "/proc/self/fd/%d", wfd);
		rofd = open(proc, O_RDONLY | O_CLOEXEC, 0);
	}
	if (rofd < 0) {
		snprintf(why, why_len, "cannot open the file read-only: %s", strerror(errno));
		goto fail;
	}
	if (fstat(rofd, &r) < 0 || r.st_dev != w.st_dev || r.st_ino != w.st_ino) {
		snprintf(why, why_len, "the read-only descriptor is not the file written");
		goto fail;
	}
	if (linked) {
		if (unlinkat(pd, SNAP_FILE, 0) < 0) {
			snprintf(why, why_len, "cannot remove the file's name: %s", strerror(errno));
			goto fail;
		}
		linked = false;
	}
	if (fstat(rofd, &r) < 0 || !S_ISREG(r.st_mode) || r.st_nlink != 0 || (r.st_mode & 07777) != 0400
	    || r.st_uid != euid || r.st_size < 0 || (size_t)r.st_size != len) {
		snprintf(why, why_len, "the file is not as made (type, links, mode, owner or size)");
		goto fail;
	}
	close(wfd);
	return rofd;

fail:
	if (rofd >= 0)
		close(rofd);
	close(wfd);
	if (linked)
		unlinkat(pd, SNAP_FILE, 0);
	return -1;
}

/* In candidate directory dfd: a private directory, the file in it (O_TMPFILE, else by name), the directory removed
 * again. \returns the read-only descriptor, or -1 with the reason in why; nothing is left behind on failure. */
static int fallback_in(int dfd, const char *text, size_t len, char *why, size_t why_len)
{
	uid_t euid = geteuid();
	char name[sizeof(SNAP_DIR_PREFIX) + 32], why2[256];
	uint8_t rnd[6];
	struct stat a, b;
	int pd = -1, fd = -1, i;
	bool made = false;

	for (i = 0; i < 16 && !made; i++) {
		if (getrandom(rnd, sizeof(rnd), 0) != sizeof(rnd)) {
			snprintf(why, why_len, "no random name: %s", strerror(errno));
			return -1;
		}
		snprintf(name, sizeof(name), SNAP_DIR_PREFIX "%s", osmo_hexdump_nospc(rnd, sizeof(rnd)));
		if (mkdirat(dfd, name, 0700) == 0)
			made = true;
		else if (errno != EEXIST) {
			snprintf(why, why_len, "cannot create a private directory: %s", strerror(errno));
			return -1;
		}
	}
	if (!made) {
		snprintf(why, why_len, "no unused name for a private directory");
		return -1;
	}
	/* The directory just made, and nothing else: ours, a directory, the same inode once opened, mode 0700
	 * whatever the umask did. */
	if (fstatat(dfd, name, &a, AT_SYMLINK_NOFOLLOW) < 0 || !S_ISDIR(a.st_mode) || a.st_uid != euid) {
		snprintf(why, why_len, "the private directory is not as made");
		goto out;
	}
	pd = openat(dfd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC, 0);
	if (pd < 0 || fstat(pd, &b) < 0 || b.st_dev != a.st_dev || b.st_ino != a.st_ino) {
		snprintf(why, why_len, "the private directory was replaced");
		goto out;
	}
	if (fchmod(pd, 0700) < 0 || fstat(pd, &b) < 0 || (b.st_mode & 07777) != 0700 || b.st_uid != euid) {
		snprintf(why, why_len, "the private directory is not mode 0700");
		goto out;
	}

	fd = fallback_file(pd, false, text, len, why2, sizeof(why2));
	if (fd < 0) {
		LOGP(aworker_log_cat, LOGL_INFO, "snapshot fallback: O_TMPFILE not usable (%s); creating a named file"
		     " instead\n", why2);
		fd = fallback_file(pd, true, text, len, why, why_len);
	}

out:
	if (pd >= 0)
		close(pd);
	/* Empty again (O_TMPFILE leaves no name, the named file's is removed): the directory goes. */
	if (unlinkat(dfd, name, AT_REMOVEDIR) < 0 && fd >= 0) {
		snprintf(why, why_len, "cannot remove the private directory: %s", strerror(errno));
		close(fd);
		fd = -1;
	}
	return fd;
}

/* The fallback, for worker idx, after the memfd failed with memfd_rc. \returns the read-only descriptor, or -1. */
static int fallback_snapshot(unsigned int idx, const char *text, size_t len, int memfd_rc)
{
	const char *env = getenv("RUNTIME_DIRECTORY");
	char path[PATH_MAX], why[256];
	unsigned int c;
	int dfd, fd;

	/* $RUNTIME_DIRECTORY (its first entry, absolute only, and only if set), /dev/shm, /tmp. Never $TMPDIR. */
	for (c = 0; c < 3; c++) {
		if (c == 0) {
			if (!env)
				continue;
			osmo_strlcpy(path, env, sizeof(path));
			path[strcspn(path, ":")] = '\0';
			if (path[0] != '/') {
				LOGP(aworker_log_cat, LOGL_NOTICE, "worker %u: snapshot fallback: not using $RUNTIME_DIRECTORY"
				     " '%s': not an absolute path\n", idx, path);
				continue;
			}
		} else {
			osmo_strlcpy(path, c == 1 ? "/dev/shm" : "/tmp", sizeof(path));
		}
		dfd = open_candidate(path, why, sizeof(why));
		if (dfd < 0) {
			LOGP(aworker_log_cat, LOGL_NOTICE, "worker %u: snapshot fallback: not using %s: %s\n", idx, path,
			     why);
			continue;
		}
		report_leftovers(dfd, path);
		fd = fallback_in(dfd, text, len, why, sizeof(why));
		close(dfd);
		if (fd >= 0) {
			LOGP(aworker_log_cat, LOGL_NOTICE, "WARNING: worker %u: no sealed memfd for the configuration snapshot"
			     " (%s); using an unlinked, read-only file made in a private directory under %s instead. This is"
			     " WEAKER than a sealed memfd: another process of the same user (uid %u) can reopen it through"
			     " /proc/%d/fd/%d, make it writable and change it, or read it, and root can do anything. It is"
			     " compared with the configuration before every worker start.\n", idx, strerror(-memfd_rc),
			     path, (unsigned int)geteuid(), (int)getpid(), fd);
			return fd;
		}
		LOGP(aworker_log_cat, LOGL_NOTICE, "worker %u: snapshot fallback: failed in %s: %s\n", idx, path, why);
	}
	LOGP(aworker_log_cat, LOGL_ERROR, "worker %u: no sealed memfd for the configuration snapshot (%s), and no fallback"
	     " directory could be used: no snapshot\n", idx, strerror(-memfd_rc));
	return -1;
}

int aworker_snapshot_create(void *ctx, unsigned int idx, const char *text, size_t len, struct aworker_snapshot **out)
{
	struct aworker_snapshot *s;
	char name[32];
	bool noexec = true, fallback = false;
	int fd, seals, rc;

	*out = NULL;
	if (!len)
		return -EINVAL;
	if (len > AWORKER_CFG_SNAPSHOT_MAX)
		return -EFBIG;

	snprintf(name, sizeof(name), "bsc-aworker-%u", idx);
	fd = snap_memfd_create(name, MFD_CLOEXEC | MFD_ALLOW_SEALING | MFD_NOEXEC_SEAL);
	if (fd < 0 && errno == EINVAL) {
		/* A kernel without MFD_NOEXEC_SEAL (before 6.3): the same, without the exec seal. Only EINVAL means
		 * that; every other error is final. */
		noexec = false;
		fd = snap_memfd_create(name, MFD_CLOEXEC | MFD_ALLOW_SEALING);
	}
	if (fd < 0) {
		rc = -errno;
		goto no_memfd;
	}

	if (pwrite_full(fd, text, len) != (ssize_t)len) {
		rc = errno ? -errno : -EIO;
		goto fail;
	}
	if (fcntl(fd, F_ADD_SEALS, SNAP_SEALS) < 0) {
		rc = -EPERM;
		goto fail;
	}
	/* Never a weaker state: all four seals, or no snapshot. */
	seals = fcntl(fd, F_GET_SEALS);
	if (seals < 0 || (seals & SNAP_SEALS) != SNAP_SEALS) {
		rc = -EPERM;
		goto fail;
	}

made:
	s = talloc_zero(ctx, struct aworker_snapshot);
	OSMO_ASSERT(s);
	s->idx = idx;
	s->fd = fd;
	s->len = len;
	s->noexec = noexec;
	s->fallback = fallback;
	s->text = talloc_memdup(s, text, len);
	OSMO_ASSERT(s->text);
	*out = s;
	return 0;

fail:
	close(fd);
no_memfd:
	/* No sealed memfd (D-12): the fallback (D-4), or no snapshot at all. */
	fd = fallback_snapshot(idx, text, len, rc);
	if (fd < 0)
		return rc;
	noexec = false;
	fallback = true;
	goto made;
}

int aworker_snapshot_verify(const struct aworker_snapshot *s)
{
	char buf[4096];
	struct stat st;
	size_t off;
	int seals;

	if (s->fallback) {
		/* Not sealed: still unlinked, 0400, ours, and only readable through this descriptor. */
		int fl = fcntl(s->fd, F_GETFL);
		if (fl < 0 || fstat(s->fd, &st) < 0)
			return -errno;
		if (!S_ISREG(st.st_mode) || st.st_nlink != 0 || (st.st_mode & 07777) != 0400 || st.st_uid != geteuid()
		    || (fl & O_ACCMODE) != O_RDONLY)
			return -EPERM;
	} else {
		seals = fcntl(s->fd, F_GET_SEALS);
		if (seals < 0)
			return -errno;
		if ((seals & SNAP_SEALS) != SNAP_SEALS)
			return -EPERM;
	}
	if (fstat(s->fd, &st) < 0)
		return -errno;
	if (st.st_size < 0 || (size_t)st.st_size != s->len)
		return -ESTALE;
	for (off = 0; off < s->len; ) {
		size_t want = OSMO_MIN(sizeof(buf), s->len - off);
		ssize_t n = pread(s->fd, buf, want, (off_t)off);
		if (n < 0 && errno == EINTR)
			continue;
		if (n != (ssize_t)want || memcmp(buf, s->text + off, want))
			return -ESTALE;
		off += want;
	}
	return 0;
}

void aworker_snapshot_close(struct aworker_snapshot *s)
{
	if (!s)
		return;
	if (s->fd >= 0)
		close(s->fd);
	talloc_free(s);
}

static int refuse(int rc, char *why, size_t why_len, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static int refuse(int rc, char *why, size_t why_len, const char *fmt, ...)
{
	va_list ap;

	if (why && why_len) {
		va_start(ap, fmt);
		vsnprintf(why, why_len, fmt, ap);
		va_end(ap);
	}
	return rc;
}

int aworker_snapshot_read(void *ctx, int fd, int ipc_fd, char **text, size_t *len, char *why, size_t why_len)
{
	struct stat st;
	char *buf;
	size_t size;
	ssize_t n;
	int seals, rc;

	*text = NULL;
	*len = 0;
	if (why && why_len)
		why[0] = '\0';

	/* Not a snapshot descriptor at all: left alone (closing stdio or the IPC socket would only do harm). */
	if (fd < 0)
		return refuse(-EBADF, why, why_len, "no snapshot descriptor (%d)", fd);
	if (fd <= 2)
		return refuse(-EINVAL, why, why_len, "descriptor %d is a standard stream, not a snapshot", fd);
	if (fd == ipc_fd)
		return refuse(-EINVAL, why, why_len, "descriptor %d is the IPC socket, not a snapshot", fd);

	/* From here on, fd is the snapshot descriptor, and it is closed whatever happens. */
	if (fstat(fd, &st) < 0) {
		rc = -errno;
		refuse(rc, why, why_len, "snapshot descriptor %d: %s", fd, strerror(errno));
		goto out_close;
	}
	if (!S_ISREG(st.st_mode)) {
		rc = refuse(-EINVAL, why, why_len, "snapshot descriptor %d is not a regular file", fd);
		goto out_close;
	}
	seals = fcntl(fd, F_GET_SEALS);
	if (seals < 0 || (seals & SNAP_SEALS) != SNAP_SEALS) {
		/* Not a sealed memfd: only the fallback's file (D-4) is accepted instead -- unlinked, 0400, owned by
		 * this user, on a read-only descriptor. */
		int fl = fcntl(fd, F_GETFL);
		if (fl < 0 || st.st_nlink != 0 || (st.st_mode & 07777) != 0400 || st.st_uid != geteuid()
		    || (fl & O_ACCMODE) != O_RDONLY) {
			if (seals < 0)
				rc = refuse(-EPERM, why, why_len, "snapshot descriptor %d is neither a sealed memfd"
					    " (%s) nor an unlinked, read-only file of this user", fd, strerror(errno));
			else
				rc = refuse(-EPERM, why, why_len, "snapshot descriptor %d lacks seals (has 0x%x, needs"
					    " 0x%x) and is not an unlinked, read-only file of this user", fd, seals,
					    SNAP_SEALS);
			goto out_close;
		}
	}
	if (st.st_size <= 0) {
		rc = refuse(-EINVAL, why, why_len, "snapshot is empty");
		goto out_close;
	}
	if ((unsigned long long)st.st_size > AWORKER_CFG_SNAPSHOT_MAX) {
		rc = refuse(-EFBIG, why, why_len, "snapshot has %lld octets, more than the %u accepted",
			    (long long)st.st_size, AWORKER_CFG_SNAPSHOT_MAX);
		goto out_close;
	}
	size = (size_t)st.st_size;

	buf = talloc_size(ctx, size + 1);
	OSMO_ASSERT(buf);
	/* D-3: from offset 0, whatever the (shared) file offset is; it is not changed. */
	n = pread_full(fd, buf, size);
	if (n != (ssize_t)size) {
		talloc_free(buf);
		rc = refuse(-EIO, why, why_len, "snapshot could not be read completely (%zd of %zu octets)", n, size);
		goto out_close;
	}
	buf[size] = '\0';
	close(fd);
	*text = buf;
	*len = size;
	return 0;

out_close:
	close(fd);
	return rc;
}
