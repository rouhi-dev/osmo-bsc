/* A-worker pool: worker configuration snapshots (sealed memfd) */
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
#pragma once

/* SNAPSHOTS
 * =========
 *
 * A worker of an inline pool parses its view (aworker_cfg_model_view()) from a snapshot: an anonymous file that holds
 * the view and can no longer change. The parent makes one per worker, once, before the first worker is started, and
 * hands the same one to every start of that worker; the worker reads it completely and closes it before parsing.
 *
 * Storage (decision D-12): a memfd named "bsc-aworker-<index>", created with MFD_CLOEXEC | MFD_ALLOW_SEALING |
 * MFD_NOEXEC_SEAL. If the kernel refuses that with EINVAL (and only then), it is created again without
 * MFD_NOEXEC_SEAL, which loses nothing but the exec seal. After the view is written, F_SEAL_SHRINK, F_SEAL_GROW,
 * F_SEAL_WRITE and F_SEAL_SEAL are added, and F_GET_SEALS must show all four: a memfd with fewer seals is never used.
 *
 * Size (D-5): from 1 to AWORKER_CFG_SNAPSHOT_MAX octets, both when it is made and when it is read.
 *
 * Reading (D-3): with pread() from offset 0 until the whole size is read, so neither the file offset (shared by
 * every process that inherited the descriptor) matters nor is it changed.
 *
 * Fallback (D-4), only when no sealed memfd can be made (memfd_create() fails, or the four seals cannot be set): an
 * unlinked file, mode 0400, on a read-only descriptor. It is NOT equivalent to a sealed memfd: another process of the
 * same user can reopen it through /proc/<pid>/fd/<n>, make it writable and change it, or read it; root can do
 * anything. Its use is logged with a WARNING that says so, and aworker_snapshot_verify() compares it with the view
 * before every worker start.
 *  - Directory, the first one usable of: $RUNTIME_DIRECTORY (only if set; its first entry, and only if absolute),
 *    /dev/shm, /tmp. Never $TMPDIR. Each is opened with O_DIRECTORY | O_NOFOLLOW (a symbolic link or anything but a
 *    directory is refused) and must be owned by this user (whatever its mode, even if others may write to it: the
 *    private directory made in it is 0700 and is only used through its descriptor), or else be a sticky directory
 *    owned by root. Anything else is refused. If none is usable, there is no snapshot.
 *  - Everything else goes through the descriptor of that directory: a private directory
 *    "bsc-aworker-snap.<random>" (mode 0700, checked to be the one made), in it the file (O_TMPFILE, else created
 *    O_CREAT | O_EXCL | O_NOFOLLOW and its name removed right away), written, made 0400, opened read-only again and
 *    checked to be the same inode; then the private directory is removed.
 *  - Private directories of this user that an earlier process left behind are reported (WARNING), never removed.
 *
 * Accepted by a worker: a regular file of an accepted size, on a descriptor above 2 that is not its IPC socket, that is
 * either a memfd with all four seals or the fallback's file (unlinked, mode 0400, owned by this user, read-only
 * descriptor). Anything else is refused: a memfd without seals or missing one of them that is not such a file, a
 * linked or writable file, a pipe, a closed descriptor. */

#include <stdbool.h>
#include <stddef.h>

/*! A worker's snapshot, as the parent holds it. */
struct aworker_snapshot {
	unsigned int idx;		/*!< the worker it is for */
	int fd;				/*!< sealed memfd, close-on-exec; the parent passes it on at each start */
	size_t len;			/*!< octets of the view */
	char *text;			/*!< the parent's copy of the view, for aworker_snapshot_verify() */
	bool noexec;			/*!< created with MFD_NOEXEC_SEAL (false: the kernel refused it with EINVAL, or
					 *   fallback) */
	bool fallback;			/*!< not a sealed memfd but the fallback's file (D-4, see above) */
};

/*! Make the snapshot of worker \a idx from \a len octets of \a text (talloc'ed under \a ctx).
 * A sealed memfd if at all possible, otherwise the fallback (see SNAPSHOTS).
 * \returns 0, -EINVAL if len is 0, -EFBIG if len is above AWORKER_CFG_SNAPSHOT_MAX; if neither a sealed memfd nor the
 *	    fallback could be made, the memfd's error: -EPERM if it could not be sealed completely, or -errno of the
 *	    failing call. On failure nothing is left open or behind. */
int aworker_snapshot_create(void *ctx, unsigned int idx, const char *text, size_t len, struct aworker_snapshot **out);

/*! Check, before a worker is started with it, that the snapshot still has all four seals (the fallback's file: is
 * still unlinked, mode 0400, this user's, on a read-only descriptor), its size, and exactly the parent's copy of the
 * view. \returns 0, or -ESTALE (content or size differ), -EPERM (seals missing, or the file changed), -errno. */
int aworker_snapshot_verify(const struct aworker_snapshot *s);

/*! Close the descriptor and free the snapshot (NULL is fine). */
void aworker_snapshot_close(struct aworker_snapshot *s);

/*! Worker side: take the snapshot on \a fd (refusing it unless it is acceptable, see SNAPSHOTS), read it completely
 * with pread() from offset 0, and close \a fd -- on success and on every refusal, except when \a fd is 0, 1, 2 or
 * \a ipc_fd: those are not a snapshot, and are left alone. On success, *text (talloc'ed under \a ctx, NUL-terminated)
 * and *len hold the view. \a why, if not NULL, receives the reason of a refusal.
 * \returns 0, or a negative errno: -EBADF (no open descriptor), -EINVAL (not a snapshot descriptor, wrong file type,
 *	    empty), -EPERM (neither sealed nor the fallback's file), -EFBIG (too large), -EIO (could not be read
 *	    completely). */
int aworker_snapshot_read(void *ctx, int fd, int ipc_fd, char **text, size_t *len, char *why, size_t why_len);
