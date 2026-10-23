/*
 * SQLite VFS for the PlayStation Vita.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Compiled together with the SQLite amalgamation (SQLITE_OS_OTHER=1).
 *
 * The stock unix VFS needs POSIX advisory locks, mmap and friends which the
 * Vita does not have. This VFS talks to the kernel directly through sceIo*
 * and is tuned for how Luanti uses SQLite:
 *
 *  - Positional I/O (sceIoPread/sceIoPwrite): one syscall per request, no
 *    seek + read pairs.
 *  - A write-behind buffer coalesces the sequential page writes SQLite does
 *    for journals and at commit time into large writes. The memory card is
 *    much faster with few large requests than with many 4 KiB ones.
 *  - Locking is done in-process (a process cannot share files with other
 *    processes on the Vita anyway), with the usual SQLite lock levels so
 *    that several connections to one file still behave correctly.
 */

#include "sqlite3.h"

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/rng.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/rtc.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define VITA_MAX_PATHNAME 512
#define VITA_WRITE_BUFFER (64 * 1024)

/* ---- in-process lock table ------------------------------------------ */

typedef struct LockNode LockNode;
struct LockNode {
	char path[VITA_MAX_PATHNAME];
	int refs;      /* open file handles */
	int shared;    /* number of SHARED (or higher) holders */
	int reserved;  /* a handle holds RESERVED or PENDING */
	int pending;   /* a handle holds PENDING (blocks new SHARED) */
	int exclusive; /* a handle holds EXCLUSIVE */
	LockNode *next;
};

static LockNode *g_locks = NULL;
static pthread_mutex_t g_lock_mutex = PTHREAD_MUTEX_INITIALIZER;

static LockNode *lock_node_acquire(const char *path)
{
	LockNode *n;
	pthread_mutex_lock(&g_lock_mutex);
	for (n = g_locks; n; n = n->next) {
		if (strcmp(n->path, path) == 0)
			break;
	}
	if (!n) {
		n = calloc(1, sizeof(*n));
		if (n) {
			strncpy(n->path, path, VITA_MAX_PATHNAME - 1);
			n->next = g_locks;
			g_locks = n;
		}
	}
	if (n)
		n->refs++;
	pthread_mutex_unlock(&g_lock_mutex);
	return n;
}

static void lock_node_release(LockNode *node)
{
	LockNode **pp;
	pthread_mutex_lock(&g_lock_mutex);
	if (--node->refs == 0) {
		for (pp = &g_locks; *pp; pp = &(*pp)->next) {
			if (*pp == node) {
				*pp = node->next;
				free(node);
				break;
			}
		}
	}
	pthread_mutex_unlock(&g_lock_mutex);
}

/* ---- file ------------------------------------------------------------- */

typedef struct VitaFile {
	sqlite3_file base;
	SceUID fd;
	int lock;           /* current SQLITE_LOCK_* of this handle */
	LockNode *node;     /* NULL for temp/journal files without locking */
	char *path;         /* only set for delete-on-close */

	/* write-behind buffer */
	unsigned char *buf;
	sqlite3_int64 buf_off; /* file offset of buf[0] */
	int buf_len;
} VitaFile;

static int flush_buffer(VitaFile *f)
{
	int off = 0;
	while (off < f->buf_len) {
		int w = sceIoPwrite(f->fd, f->buf + off, f->buf_len - off,
				f->buf_off + off);
		if (w <= 0)
			return SQLITE_IOERR_WRITE;
		off += w;
	}
	f->buf_len = 0;
	return SQLITE_OK;
}

static int vita_close(sqlite3_file *file)
{
	VitaFile *f = (VitaFile *)file;
	int rc = flush_buffer(f);
	sceIoClose(f->fd);
	if (f->path) {
		sceIoRemove(f->path);
		sqlite3_free(f->path);
	}
	if (f->node)
		lock_node_release(f->node);
	sqlite3_free(f->buf);
	return rc;
}

static int vita_read(sqlite3_file *file, void *out, int amount, sqlite3_int64 offset)
{
	VitaFile *f = (VitaFile *)file;
	int got = 0;

	/* Serve reads that fall entirely inside the pending write buffer
	 * directly; otherwise flush so the file is consistent. */
	if (f->buf_len > 0) {
		if (offset >= f->buf_off && offset + amount <= f->buf_off + f->buf_len) {
			memcpy(out, f->buf + (offset - f->buf_off), amount);
			return SQLITE_OK;
		}
		if (flush_buffer(f) != SQLITE_OK)
			return SQLITE_IOERR_READ;
	}

	while (got < amount) {
		int r = sceIoPread(f->fd, (char *)out + got, amount - got, offset + got);
		if (r < 0)
			return SQLITE_IOERR_READ;
		if (r == 0)
			break;
		got += r;
	}
	if (got < amount) {
		memset((char *)out + got, 0, amount - got);
		return SQLITE_IOERR_SHORT_READ;
	}
	return SQLITE_OK;
}

static int vita_write(sqlite3_file *file, const void *data, int amount, sqlite3_int64 offset)
{
	VitaFile *f = (VitaFile *)file;

	if (f->buf) {
		/* Append to the buffer if the write continues it */
		if (f->buf_len > 0 && offset != f->buf_off + f->buf_len) {
			int rc = flush_buffer(f);
			if (rc != SQLITE_OK)
				return rc;
		}
		if (amount <= VITA_WRITE_BUFFER) {
			if (f->buf_len == 0)
				f->buf_off = offset;
			if (f->buf_len + amount > VITA_WRITE_BUFFER) {
				int rc = flush_buffer(f);
				if (rc != SQLITE_OK)
					return rc;
				f->buf_off = offset;
			}
			memcpy(f->buf + f->buf_len, data, amount);
			f->buf_len += amount;
			return SQLITE_OK;
		}
		/* Larger than the buffer: write through */
	}

	{
		int done = 0;
		while (done < amount) {
			int w = sceIoPwrite(f->fd, (const char *)data + done,
					amount - done, offset + done);
			if (w <= 0)
				return SQLITE_IOERR_WRITE;
			done += w;
		}
	}
	return SQLITE_OK;
}

static int vita_truncate(sqlite3_file *file, sqlite3_int64 size)
{
	VitaFile *f = (VitaFile *)file;
	SceIoStat st;
	if (flush_buffer(f) != SQLITE_OK)
		return SQLITE_IOERR_TRUNCATE;
	memset(&st, 0, sizeof(st));
	st.st_size = size;
	if (sceIoChstatByFd(f->fd, &st, SCE_CST_SIZE) < 0)
		return SQLITE_IOERR_TRUNCATE;
	return SQLITE_OK;
}

static int vita_sync(sqlite3_file *file, int flags)
{
	VitaFile *f = (VitaFile *)file;
	(void)flags;
	if (flush_buffer(f) != SQLITE_OK)
		return SQLITE_IOERR_FSYNC;
	if (sceIoSyncByFd(f->fd, 0) < 0)
		return SQLITE_IOERR_FSYNC;
	return SQLITE_OK;
}

static int vita_file_size(sqlite3_file *file, sqlite3_int64 *size)
{
	VitaFile *f = (VitaFile *)file;
	SceIoStat st;
	if (flush_buffer(f) != SQLITE_OK)
		return SQLITE_IOERR_FSTAT;
	if (sceIoGetstatByFd(f->fd, &st) < 0)
		return SQLITE_IOERR_FSTAT;
	*size = st.st_size;
	return SQLITE_OK;
}

/* Lock transitions, following the semantics documented in os_unix.c */
static int vita_lock(sqlite3_file *file, int level)
{
	VitaFile *f = (VitaFile *)file;
	LockNode *n = f->node;
	int rc = SQLITE_OK;

	if (!n || f->lock >= level) {
		if (!n)
			f->lock = level;
		return SQLITE_OK;
	}

	pthread_mutex_lock(&g_lock_mutex);
	switch (level) {
	case SQLITE_LOCK_SHARED:
		if (n->pending || n->exclusive) {
			rc = SQLITE_BUSY;
		} else {
			n->shared++;
			f->lock = SQLITE_LOCK_SHARED;
		}
		break;
	case SQLITE_LOCK_RESERVED:
		if (n->reserved) {
			rc = SQLITE_BUSY;
		} else {
			n->reserved = 1;
			f->lock = SQLITE_LOCK_RESERVED;
		}
		break;
	case SQLITE_LOCK_EXCLUSIVE:
		/* Acquire PENDING first so no new readers come in */
		if (f->lock < SQLITE_LOCK_PENDING) {
			if (n->reserved && f->lock < SQLITE_LOCK_RESERVED) {
				rc = SQLITE_BUSY;
				break;
			}
			n->reserved = 1;
			n->pending = 1;
			f->lock = SQLITE_LOCK_PENDING;
		}
		/* EXCLUSIVE once we are the only reader left */
		if (n->shared > 1) {
			rc = SQLITE_BUSY;
		} else {
			n->exclusive = 1;
			f->lock = SQLITE_LOCK_EXCLUSIVE;
		}
		break;
	default:
		break;
	}
	pthread_mutex_unlock(&g_lock_mutex);
	return rc;
}

static int vita_unlock(sqlite3_file *file, int level)
{
	VitaFile *f = (VitaFile *)file;
	LockNode *n = f->node;

	if (!n || f->lock <= level) {
		if (!n)
			f->lock = level;
		return SQLITE_OK;
	}

	pthread_mutex_lock(&g_lock_mutex);
	if (f->lock >= SQLITE_LOCK_EXCLUSIVE)
		n->exclusive = 0;
	if (f->lock >= SQLITE_LOCK_PENDING)
		n->pending = 0;
	if (f->lock >= SQLITE_LOCK_RESERVED)
		n->reserved = 0;
	if (level == SQLITE_LOCK_NONE && f->lock >= SQLITE_LOCK_SHARED)
		n->shared--;
	f->lock = level;
	pthread_mutex_unlock(&g_lock_mutex);
	return SQLITE_OK;
}

static int vita_check_reserved_lock(sqlite3_file *file, int *out)
{
	VitaFile *f = (VitaFile *)file;
	if (!f->node) {
		*out = 0;
		return SQLITE_OK;
	}
	pthread_mutex_lock(&g_lock_mutex);
	*out = f->node->reserved;
	pthread_mutex_unlock(&g_lock_mutex);
	return SQLITE_OK;
}

static int vita_file_control(sqlite3_file *file, int op, void *arg)
{
	(void)file;
	(void)op;
	(void)arg;
	return SQLITE_NOTFOUND;
}

static int vita_sector_size(sqlite3_file *file)
{
	(void)file;
	return 4096;
}

static int vita_device_characteristics(sqlite3_file *file)
{
	(void)file;
	/* Writes to the memory card are not atomic, but a write never
	 * damages bytes outside of the range written. */
	return SQLITE_IOCAP_POWERSAFE_OVERWRITE;
}

static const sqlite3_io_methods vita_io_methods = {
	1,
	vita_close,
	vita_read,
	vita_write,
	vita_truncate,
	vita_sync,
	vita_file_size,
	vita_lock,
	vita_unlock,
	vita_check_reserved_lock,
	vita_file_control,
	vita_sector_size,
	vita_device_characteristics,
	0, 0, 0, 0, 0, 0
};

/* ---- VFS -------------------------------------------------------------- */

static int vita_open(sqlite3_vfs *vfs, const char *name, sqlite3_file *file,
		int flags, int *out_flags)
{
	VitaFile *f = (VitaFile *)file;
	char tmpname[VITA_MAX_PATHNAME];
	int oflags = 0;

	memset(f, 0, sizeof(*f));
	f->fd = -1;

	if (!name) {
		/* Temporary file requested without a name */
		unsigned int r;
		sceKernelGetRandomNumber(&r, sizeof(r));
		snprintf(tmpname, sizeof(tmpname), "ux0:data/luanti/cache/sqlite_%08x.tmp", r);
		name = tmpname;
		flags |= SQLITE_OPEN_DELETEONCLOSE;
	}

	if (flags & SQLITE_OPEN_READWRITE)
		oflags = SCE_O_RDWR;
	else
		oflags = SCE_O_RDONLY;
	if (flags & SQLITE_OPEN_CREATE)
		oflags |= SCE_O_CREAT;
	if (flags & SQLITE_OPEN_EXCLUSIVE)
		oflags |= SCE_O_EXCL;

	f->fd = sceIoOpen(name, oflags, 0777);
	if (f->fd < 0)
		return SQLITE_CANTOPEN;

	if (flags & SQLITE_OPEN_DELETEONCLOSE) {
		size_t len = strlen(name) + 1;
		f->path = sqlite3_malloc((int)len);
		if (f->path)
			memcpy(f->path, name, len);
	}

	/* Only main databases take part in locking */
	if (flags & SQLITE_OPEN_MAIN_DB)
		f->node = lock_node_acquire(name);

	if (flags & SQLITE_OPEN_READWRITE)
		f->buf = sqlite3_malloc(VITA_WRITE_BUFFER);

	if (out_flags)
		*out_flags = flags;
	f->base.pMethods = &vita_io_methods;
	(void)vfs;
	return SQLITE_OK;
}

static int vita_delete(sqlite3_vfs *vfs, const char *name, int sync_dir)
{
	(void)vfs;
	(void)sync_dir;
	if (sceIoRemove(name) < 0) {
		SceIoStat st;
		/* Deleting something that is not there is not an error */
		if (sceIoGetstat(name, &st) < 0)
			return SQLITE_IOERR_DELETE_NOENT;
		return SQLITE_IOERR_DELETE;
	}
	return SQLITE_OK;
}

static int vita_access(sqlite3_vfs *vfs, const char *name, int flags, int *out)
{
	SceIoStat st;
	(void)vfs;
	(void)flags;
	*out = sceIoGetstat(name, &st) >= 0;
	return SQLITE_OK;
}

static int vita_full_pathname(sqlite3_vfs *vfs, const char *name, int n_out, char *out)
{
	(void)vfs;
	/* Luanti always passes absolute "dev:path" paths */
	sqlite3_snprintf(n_out, out, "%s", name);
	return SQLITE_OK;
}

static int vita_randomness(sqlite3_vfs *vfs, int n, char *out)
{
	int done = 0;
	(void)vfs;
	while (done < n) {
		int chunk = n - done > 64 ? 64 : n - done;
		sceKernelGetRandomNumber(out + done, chunk);
		done += chunk;
	}
	return n;
}

static int vita_sleep(sqlite3_vfs *vfs, int us)
{
	(void)vfs;
	sceKernelDelayThread(us);
	return us;
}

static int vita_current_time_int64(sqlite3_vfs *vfs, sqlite3_int64 *out)
{
	/* Julian day number in milliseconds */
	static const sqlite3_int64 unix_epoch = 24405875 * (sqlite3_int64)8640000;
	(void)vfs;
	*out = unix_epoch + (sqlite3_int64)time(NULL) * 1000;
	return SQLITE_OK;
}

static int vita_current_time(sqlite3_vfs *vfs, double *out)
{
	sqlite3_int64 t;
	vita_current_time_int64(vfs, &t);
	*out = t / 86400000.0;
	return SQLITE_OK;
}

static int vita_get_last_error(sqlite3_vfs *vfs, int n, char *out)
{
	(void)vfs;
	if (n > 0)
		out[0] = 0;
	return 0;
}

static sqlite3_vfs vita_vfs = {
	2,                    /* iVersion */
	sizeof(VitaFile),     /* szOsFile */
	VITA_MAX_PATHNAME,    /* mxPathname */
	0,                    /* pNext */
	"psp2",               /* zName */
	0,                    /* pAppData */
	vita_open,
	vita_delete,
	vita_access,
	vita_full_pathname,
	0, 0, 0, 0,           /* no dynamic loading */
	vita_randomness,
	vita_sleep,
	vita_current_time,
	vita_get_last_error,
	vita_current_time_int64,
	0, 0, 0
};

int sqlite3_os_init(void)
{
	return sqlite3_vfs_register(&vita_vfs, 1);
}

int sqlite3_os_end(void)
{
	return SQLITE_OK;
}
