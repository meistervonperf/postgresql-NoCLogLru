/*-------------------------------------------------------------------------
 *
 * nolru.c
 *		No LRU buffering for transaction status logfiles
 *
 * We does not use simple least-recently-used schemes to manage a pool of
 * page buffers.
 *
 *
 * Portions Copyright (c) 2016, Takashi Horikawa
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/access/transam/nolru.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "access/nolru.h"
#include "access/transam.h"
#include "access/xlog.h"
#include "storage/fd.h"
#include "storage/shmem.h"
#include "miscadmin.h"


#define NolruFileName(ctl, path, seg) \
	snprintf(path, MAXPGPATH, "%s/%04X", (ctl)->Dir, seg)

/*
 * During NoLruFlush(), we will usually not need to write/fsync more
 * than one or two physical files, but we may need to write several pages
 * per file.  We can consolidate the I/O requests by leaving files open
 * until control returns to NoLruFlush().  This data structure remembers
 * which files are open.
 */
#define MAX_FLUSH_BUFFERS	16
#define MAX_DIRTY_PAGES	64

typedef struct NolruFlushData
{
	int			num_files;		/* # files actually open */
	int			fd[MAX_FLUSH_BUFFERS];	/* their FD's */
	int			segno[MAX_FLUSH_BUFFERS];		/* their log seg#s */
} NolruFlushData;

typedef struct NolruFlushData *NolruFlush;

/*
 * Macro to mark a buffer slot "most recently used".  Note multiple evaluation
 * of arguments!
 *
 * The reason for the if-test is that there are often many consecutive
 * accesses to the same page (particularly the latest page).  By suppressing
 * useless increments of cur_lru_count, we reduce the probability that old
 * pages' counts will "wrap around" and make them appear recently used.
 *
 * We allow this code to be executed concurrently by multiple processes within
 * NoLruReadPage_ReadOnly().  As long as int reads and writes are atomic,
 * this should not cause any completely-bogus values to enter the computation.
 * However, it is possible for either cur_lru_count or individual
 * page_lru_count entries to be "reset" to lower values than they should have,
 * in case a process is delayed while it executes this macro.  With care in
 * NolruSelectLRUPage(), this does little harm, and in any case the absolute
 * worst possible consequence is a nonoptimal choice of page to evict.  The
 * gain from allowing concurrent reads of NOLRU pages seems worth it.
 */

/* Saved info for NolruReportIOError */
typedef enum
{
	NOLRU_OPEN_FAILED,
	NOLRU_SEEK_FAILED,
	NOLRU_READ_FAILED,
	NOLRU_WRITE_FAILED,
	NOLRU_FSYNC_FAILED,
	NOLRU_CLOSE_FAILED
} NolruErrorCause;

static NolruErrorCause nolru_errcause;
static int	nolru_errno;


static void NoLruZeroLSNs(NolruCtl ctl, int slotno);
static void NoLruWaitIO(NolruCtl ctl, int slotno);
static void NolruInternalWritePage(NolruCtl ctl, int slotno, NolruFlush fdata);
static bool NolruPhysicalReadPage(NolruCtl ctl, int pageno, int slotno);
static bool NolruPhysicalWritePage(NolruCtl ctl, int pageno, int slotno,
					  NolruFlush fdata);
static void NolruReportIOError(NolruCtl ctl, int pageno, TransactionId xid);
static int	NolruSelectLRUPage(NolruCtl ctl, int pageno);

static bool NolruScanDirCbDeleteCutoff(NolruCtl ctl, char *filename,
						  int segpage, void *data);
static void NolruInternalDeleteSegment(NolruCtl ctl, char *filename);

/*
 * Initialization of shared memory
 */

Size
NoLruShmemSize(int nslots, int nlsns)
{
	Size		sz;

	/* we assume nslots isn't so large as to risk overflow */
	sz = MAXALIGN(sizeof(NolruSharedData));
	sz += MAXALIGN(nslots * sizeof(NolruPageStatus));	/* page_status[] */
	sz += MAXALIGN(nslots * sizeof(bool));		/* page_dirty[] */
	sz += MAXALIGN(nslots * sizeof(int));		/* page_number[] */
	sz += MAXALIGN(nslots * sizeof(int));		/* page_lru_count[] */
	sz += MAXALIGN(nslots * sizeof(LWLockPadded));		/* buffer_locks[] */

	if (nlsns > 0)
		sz += MAXALIGN(nslots * nlsns * sizeof(XLogRecPtr));	/* group_lsn[] */

	return BUFFERALIGN(sz) + BLCKSZ * nslots;
}

void
NoLruInit(NolruCtl ctl, const char *name, int nslots, int nlsns,
			  LWLock *ctllock, const char *subdir, int tranche_id)
{
	NolruShared	shared;
	bool		found;

	shared = (NolruShared) ShmemInitStruct(name,
										  NoLruShmemSize(nslots, nlsns),
										  &found);

	if (!IsUnderPostmaster)
	{
		/* Initialize locks and shared memory area */
		char	   *ptr;
		Size		offset;
		int			slotno;

		Assert(!found);

		memset(shared, 0, sizeof(NolruSharedData));

		shared->ControlLock = ctllock;

		shared->num_slots = nslots;
		shared->lsn_groups_per_page = nlsns;

		/* shared->latest_page_number will be set later */

		ptr = (char *) shared;
		offset = MAXALIGN(sizeof(NolruSharedData));
		shared->page_status = (NolruPageStatus *) (ptr + offset);
		offset += MAXALIGN(nslots * sizeof(NolruPageStatus));
		shared->page_dirty = (bool *) (ptr + offset);
		offset += MAXALIGN(nslots * sizeof(bool));

		if (nlsns > 0)
		{
			shared->group_lsn = (XLogRecPtr *) (ptr + offset);
			offset += MAXALIGN(nslots * nlsns * sizeof(XLogRecPtr));
		}

		/* Initialize LWLocks */
		shared->buffer_locks = (LWLockPadded *) (ptr + offset);
		offset += MAXALIGN(nslots * sizeof(LWLockPadded));

		Assert(strlen(name) + 1 < SLRU_MAX_NAME_LENGTH);
		strlcpy(shared->lwlock_tranche_name, name, NOLRU_MAX_NAME_LENGTH);
		shared->lwlock_tranche_id = tranche_id;
		shared->lwlock_tranche.name = shared->lwlock_tranche_name;
		shared->lwlock_tranche.array_base = shared->buffer_locks;
		shared->lwlock_tranche.array_stride = sizeof(LWLockPadded);

		shared->page_buffer = ptr + BUFFERALIGN(offset);
		for (slotno = 0; slotno < nslots; slotno++)
		{
			LWLockInitialize(&shared->buffer_locks[slotno].lock,
							 shared->lwlock_tranche_id);

			shared->page_status[slotno] = NOLRU_PAGE_EMPTY;
			shared->page_dirty[slotno] = false;
		}
		for (slotno = 0; slotno < NUM_CLOG_PARTITIONS; slotno++)
		{
			SpinLockInit(&shared->mutex[slotno]);
		}
		SpinLockInit(&shared->doing);
	}
	else
		Assert(found);

	/* Register SLRU tranche in the main tranches array */
	LWLockRegisterTranche(shared->lwlock_tranche_id, &shared->lwlock_tranche);

	/*
	 * Initialize the unshared control struct, including directory path. We
	 * assume caller set PagePrecedes.
	 */
	ctl->shared = shared;
	ctl->do_fsync = true;		/* default behavior */
	StrNCpy(ctl->Dir, subdir, sizeof(ctl->Dir));
}

/*
 * Initialize (or reinitialize) a page to zeroes.
 *
 * The page is not actually written, just set up in shared memory.
 * The slot number of the new page is returned.
 *
 * Control lock must be held at entry, and will be held at exit.
 */
int
NoLruZeroPage(NolruCtl ctl, int pageno)
{
	NolruShared	shared = ctl->shared;
	int			slotno;

	/* Find a suitable buffer slot for the page */
	slotno = NolruSelectLRUPage(ctl, pageno);
	Assert(shared->page_status[slotno] == NOLRU_PAGE_EMPTY ||
		   (shared->page_status[slotno] == NOLRU_PAGE_VALID &&
			!shared->page_dirty[slotno]) ||
		   shared->page_number[slotno] == pageno);

	/* Mark the slot as containing this page */
	shared->page_status[slotno] = NOLRU_PAGE_VALID;
	shared->page_dirty[slotno] = true;

	/* Set the buffer to zeroes */
	MemSet(shared->page_buffer + ((long)slotno * BLCKSZ), 0, BLCKSZ);

	/* Set the LSNs for this new page to zero */
	NoLruZeroLSNs(ctl, slotno);

	/* Assume this page is now the latest active page */
	shared->latest_page_number = pageno;

	return slotno;
}

/*
 * Zero all the LSNs we store for this nolru page.
 *
 * This should be called each time we create a new page, and each time we read
 * in a page from disk into an existing buffer.  (Such an old page cannot
 * have any interesting LSNs, since we'd have flushed them before writing
 * the page in the first place.)
 *
 * This assumes that InvalidXLogRecPtr is bitwise-all-0.
 */
static void
NoLruZeroLSNs(NolruCtl ctl, int slotno)
{
	NolruShared	shared = ctl->shared;

	if (shared->lsn_groups_per_page > 0)
		MemSet(&shared->group_lsn[slotno * shared->lsn_groups_per_page], 0,
			   shared->lsn_groups_per_page * sizeof(XLogRecPtr));
}

/*
 * Wait for any active I/O on a page slot to finish.  (This does not
 * guarantee that new I/O hasn't been started before we return, though.
 * In fact the slot might not even contain the same page anymore.)
 *
 * Control lock must be held at entry, and will be held at exit.
 */
static void
NoLruWaitIO(NolruCtl ctl, int slotno)
{
 	NolruShared	shared = ctl->shared;

	/* See notes at top of file */
	LWLockRelease(shared->ControlLock);
	LWLockAcquire(&shared->buffer_locks[slotno].lock, LW_SHARED);
	LWLockRelease(&shared->buffer_locks[slotno].lock);
	LWLockAcquire(shared->ControlLock, LW_SHARED);

	/*
	 * If the slot is still in an io-in-progress state, then either someone
	 * already started a new I/O on the slot, or a previous I/O failed and
	 * neglected to reset the page state.  That shouldn't happen, really, but
	 * it seems worth a few extra cycles to check and recover from it. We can
	 * cheaply test for failure by seeing if the buffer lock is still held (we
	 * assume that transaction abort would release the lock).
	 */
	if (shared->page_status[slotno] == NOLRU_PAGE_READ_IN_PROGRESS ||
		shared->page_status[slotno] == NOLRU_PAGE_WRITE_IN_PROGRESS)
	{
		if (LWLockConditionalAcquire(&shared->buffer_locks[slotno].lock, LW_SHARED))
		{
			/* indeed, the I/O must have failed */
			if (shared->page_status[slotno] == NOLRU_PAGE_READ_IN_PROGRESS)
				shared->page_status[slotno] = NOLRU_PAGE_EMPTY;
			else	/* write_in_progress */
			{
				shared->page_status[slotno] = NOLRU_PAGE_VALID;
				shared->page_dirty[slotno] = true;
			}
			LWLockRelease(&shared->buffer_locks[slotno].lock);
		}
	}
}

/*
 * Find a page in a shared buffer, reading it in if necessary.
 * The page number must correspond to an already-initialized page.
 *
 * If write_ok is true then it is OK to return a page that is in
 * WRITE_IN_PROGRESS state; it is the caller's responsibility to be sure
 * that modification of the page is safe.  If write_ok is false then we
 * will not return the page until it is not undergoing active I/O.
 *
 * The passed-in xid is used only for error reporting, and may be
 * InvalidTransactionId if no specific xid is associated with the action.
 *
 * Return value is the shared-buffer slot number now holding the page.
 * The buffer's LRU access info is updated.
 *
 * Control lock must be held at entry, and will be held at exit.
 */
int
NoLruReadPage(NolruCtl ctl, int pageno, bool write_ok,
				  TransactionId xid)
{
	NolruShared	shared = ctl->shared;

	/* Outer loop handles restart if we must wait for someone else's I/O */
	for (;;)
	{
		int			slotno;
		bool		ok;

		/* See if page already is in memory; if not, pick victim slot */
		slotno = NolruSelectLRUPage(ctl, pageno);

		/* Did we find the page in memory? */
		if (shared->page_status[slotno] != NOLRU_PAGE_EMPTY)
		{
			/*
			 * If page is still being read in, we must wait for I/O.  Likewise
			 * if the page is being written and the caller said that's not OK.
			 */
			if (shared->page_status[slotno] == NOLRU_PAGE_READ_IN_PROGRESS ||
				(shared->page_status[slotno] == NOLRU_PAGE_WRITE_IN_PROGRESS &&
				 !write_ok))
			{
				NoLruWaitIO(ctl, slotno);
				/* Now we must recheck state from the top */
				continue;
			}
			/* Otherwise, it's ready to use */
			return slotno;
		}

		/* We found no match; assert we selected a freeable slot */
		Assert(shared->page_status[slotno] == NOLRU_PAGE_EMPTY ||
			   (shared->page_status[slotno] == NOLRU_PAGE_VALID &&
				!shared->page_dirty[slotno]));

		/* Mark the slot read-busy */
		shared->page_status[slotno] = NOLRU_PAGE_READ_IN_PROGRESS;
		shared->page_dirty[slotno] = false;

		/* Acquire per-buffer lock (cannot deadlock, see notes at top) */
		LWLockAcquire(&shared->buffer_locks[slotno].lock, LW_EXCLUSIVE);

		/* Release control lock while doing I/O */
		LWLockRelease(shared->ControlLock);

		/* Do the read */
		ok = NolruPhysicalReadPage(ctl, pageno, slotno);

		/* Set the LSNs for this newly read-in page to zero */
		NoLruZeroLSNs(ctl, slotno);

		/* Re-acquire control lock and update page state */
		LWLockAcquire(shared->ControlLock, LW_SHARED);

		Assert(shared->page_number[slotno] == pageno &&
			   shared->page_status[slotno] == NOLRU_PAGE_READ_IN_PROGRESS &&
			   !shared->page_dirty[slotno]);

		shared->page_status[slotno] = ok ? NOLRU_PAGE_VALID : NOLRU_PAGE_EMPTY;

		LWLockRelease(&shared->buffer_locks[slotno].lock);

		/* Now it's okay to ereport if we failed */
		if (!ok)
			NolruReportIOError(ctl, pageno, xid);

		return slotno;
	}
}

/*
 * Find a page in a shared buffer, reading it in if necessary.
 * The page number must correspond to an already-initialized page.
 * The caller must intend only read-only access to the page.
 *
 * The passed-in xid is used only for error reporting, and may be
 * InvalidTransactionId if no specific xid is associated with the action.
 *
 * Return value is the shared-buffer slot number now holding the page.
 * The buffer's LRU access info is updated.
 *
 * Control lock must NOT be held at entry, but will be held at exit.
 * It is unspecified whether the lock will be shared or exclusive.
 */
int
NoLruReadPage_ReadOnly(NolruCtl ctl, int pageno, TransactionId xid)
{
	NolruShared	shared = ctl->shared;
	int			slotno;

	/* Try to find the page while holding only shared lock */
	LWLockAcquire(shared->ControlLock, LW_SHARED);

	/* See if page is already in a buffer */
	slotno = pageno;
	if (shared->page_status[slotno] != NOLRU_PAGE_EMPTY &&
		shared->page_status[slotno] != NOLRU_PAGE_READ_IN_PROGRESS)
	{
		/* See comments for NolruRecentlyUsed macro */
		return slotno;
	}

	/* No luck, so switch to normal exclusive lock and do regular read */

	return NoLruReadPage(ctl, pageno, true, xid);
}

/*
 * Write a page from a shared buffer, if necessary.
 * Does nothing if the specified slot is not dirty.
 *
 * NOTE: only one write attempt is made here.  Hence, it is possible that
 * the page is still dirty at exit (if someone else re-dirtied it during
 * the write).  However, we *do* attempt a fresh write even if the page
 * is already being written; this is for checkpoints.
 *
 * Control lock must be held at entry, and will be held at exit.
 */
static void
NolruInternalWritePage(NolruCtl ctl, int slotno, NolruFlush fdata)
{
	NolruShared	shared = ctl->shared;
	int			pageno = slotno;
	bool		ok;

	/* If a write is in progress, wait for it to finish */
	while (shared->page_status[slotno] == NOLRU_PAGE_WRITE_IN_PROGRESS)
	{
		NoLruWaitIO(ctl, slotno);
	}

	/*
	 * Do nothing if page is not dirty, or if buffer no longer contains the
	 * same page we were called for.
	 */
	if (!shared->page_dirty[slotno] ||
		shared->page_status[slotno] != NOLRU_PAGE_VALID)
		return;

	/*
	 * Mark the slot write-busy, and clear the dirtybit.  After this point, a
	 * transaction status update on this page will mark it dirty again.
	 */
	shared->page_status[slotno] = NOLRU_PAGE_WRITE_IN_PROGRESS;
	shared->page_dirty[slotno] = false;

	/* Acquire per-buffer lock (cannot deadlock, see notes at top) */
	LWLockAcquire(&shared->buffer_locks[slotno].lock, LW_EXCLUSIVE);

	/* Release control lock while doing I/O */
	LWLockRelease(shared->ControlLock);

	/* Do the write */
	ok = NolruPhysicalWritePage(ctl, pageno, slotno, fdata);

	/* If we failed, and we're in a flush, better close the files */
	if (!ok && fdata)
	{
		int			i;

		for (i = 0; i < fdata->num_files; i++)
			CloseTransientFile(fdata->fd[i]);
	}

	/* Re-acquire control lock and update page state */
	LWLockAcquire(shared->ControlLock, LW_SHARED);

	Assert(shared->page_number[slotno] == pageno &&
		   shared->page_status[slotno] == NOLRU_PAGE_WRITE_IN_PROGRESS);

	/* If we failed to write, mark the page dirty again */
	if (!ok)
	{
		shared->page_dirty[slotno] = true;
		update_tail(ctl, slotno);
	}

	shared->page_status[slotno] = NOLRU_PAGE_VALID;

	LWLockRelease(&shared->buffer_locks[slotno].lock);

	/* Now it's okay to ereport if we failed */
	if (!ok)
		NolruReportIOError(ctl, pageno, InvalidTransactionId);
}

/*
 * Wrapper of NolruInternalWritePage, for external callers.
 * fdata is always passed a NULL here.
 */
void
NoLruWritePage(NolruCtl ctl, int slotno)
{
	NolruInternalWritePage(ctl, slotno, NULL);
}

/*
 * Return whether the given page exists on disk.
 *
 * A false return means that either the file does not exist, or that it's not
 * large enough to contain the given page.
 */
bool
NoLruDoesPhysicalPageExist(NolruCtl ctl, int pageno)
{
	int			segno = pageno / NOLRU_PAGES_PER_SEGMENT;
	int			rpageno = pageno % NOLRU_PAGES_PER_SEGMENT;
	int			offset = rpageno * BLCKSZ;
	char		path[MAXPGPATH];
	int			fd;
	bool		result;
	off_t		endpos;

	NolruFileName(ctl, path, segno);

	fd = OpenTransientFile(path, O_RDWR | PG_BINARY, S_IRUSR | S_IWUSR);
	if (fd < 0)
	{
		/* expected: file doesn't exist */
		if (errno == ENOENT)
			return false;

		/* report error normally */
		nolru_errcause = NOLRU_OPEN_FAILED;
		nolru_errno = errno;
		NolruReportIOError(ctl, pageno, 0);
	}

	if ((endpos = lseek(fd, 0, SEEK_END)) < 0)
	{
		nolru_errcause = NOLRU_OPEN_FAILED;
		nolru_errno = errno;
		NolruReportIOError(ctl, pageno, 0);
	}

	result = endpos >= (off_t) (offset + BLCKSZ);

	CloseTransientFile(fd);
	return result;
}

/*
 * Physical read of a (previously existing) page into a buffer slot
 *
 * On failure, we cannot just ereport(ERROR) since caller has put state in
 * shared memory that must be undone.  So, we return FALSE and save enough
 * info in static variables to let NolruReportIOError make the report.
 *
 * For now, assume it's not worth keeping a file pointer open across
 * read/write operations.  We could cache one virtual file pointer ...
 */
static bool
NolruPhysicalReadPage(NolruCtl ctl, int pageno, int slotno)
{
	NolruShared	shared = ctl->shared;
	int			segno = pageno / NOLRU_PAGES_PER_SEGMENT;
	int			rpageno = pageno % NOLRU_PAGES_PER_SEGMENT;
	int			offset = rpageno * BLCKSZ;
	char		path[MAXPGPATH];
	int			fd;

	NolruFileName(ctl, path, segno);

	/*
	 * In a crash-and-restart situation, it's possible for us to receive
	 * commands to set the commit status of transactions whose bits are in
	 * already-truncated segments of the commit log (see notes in
	 * NolruPhysicalWritePage).  Hence, if we are InRecovery, allow the case
	 * where the file doesn't exist, and return zeroes instead.
	 */
	fd = OpenTransientFile(path, O_RDWR | PG_BINARY, S_IRUSR | S_IWUSR);
	if (fd < 0)
	{
		if (errno != ENOENT || !InRecovery)
		{
			nolru_errcause = NOLRU_OPEN_FAILED;
			nolru_errno = errno;
			return false;
		}

		ereport(LOG,
				(errmsg("file \"%s\" doesn't exist, reading as zeroes",
						path)));
		MemSet(shared->page_buffer + ((long)slotno * BLCKSZ), 0, BLCKSZ);
		return true;
	}

	if (lseek(fd, (off_t) offset, SEEK_SET) < 0)
	{
		nolru_errcause = NOLRU_SEEK_FAILED;
		nolru_errno = errno;
		CloseTransientFile(fd);
		return false;
	}

	errno = 0;
	if (read(fd, shared->page_buffer + ((long)slotno * BLCKSZ), BLCKSZ) != BLCKSZ)
	{
		nolru_errcause = NOLRU_READ_FAILED;
		nolru_errno = errno;
		CloseTransientFile(fd);
		return false;
	}

	if (CloseTransientFile(fd))
	{
		nolru_errcause = NOLRU_CLOSE_FAILED;
		nolru_errno = errno;
		return false;
	}

	return true;
}

/*
 * Physical write of a page from a buffer slot
 *
 * On failure, we cannot just ereport(ERROR) since caller has put state in
 * shared memory that must be undone.  So, we return FALSE and save enough
 * info in static variables to let NolruReportIOError make the report.
 *
 * For now, assume it's not worth keeping a file pointer open across
 * independent read/write operations.  We do batch operations during
 * NoLruFlush, though.
 *
 * fdata is NULL for a standalone write, pointer to open-file info during
 * NoLruFlush.
 */
static bool
NolruPhysicalWritePage(NolruCtl ctl, int pageno, int slotno, NolruFlush fdata)
{
	NolruShared	shared = ctl->shared;
	int			segno = pageno / NOLRU_PAGES_PER_SEGMENT;
	int			rpageno = pageno % NOLRU_PAGES_PER_SEGMENT;
	int			offset = rpageno * BLCKSZ;
	char		path[MAXPGPATH];
	int			fd = -1;

	/*
	 * Honor the write-WAL-before-data rule, if appropriate, so that we do not
	 * write out data before associated WAL records.  This is the same action
	 * performed during FlushBuffer() in the main buffer manager.
	 */
	if (shared->group_lsn != NULL)
	{
		/*
		 * We must determine the largest async-commit LSN for the page. This
		 * is a bit tedious, but since this entire function is a slow path
		 * anyway, it seems better to do this here than to maintain a per-page
		 * LSN variable (which'd need an extra comparison in the
		 * transaction-commit path).
		 */
		XLogRecPtr	max_lsn;
		int			lsnindex,
					lsnoff;

		lsnindex = slotno * shared->lsn_groups_per_page;
		max_lsn = shared->group_lsn[lsnindex++];
		for (lsnoff = 1; lsnoff < shared->lsn_groups_per_page; lsnoff++)
		{
			XLogRecPtr	this_lsn = shared->group_lsn[lsnindex++];

			if (max_lsn < this_lsn)
				max_lsn = this_lsn;
		}

		if (!XLogRecPtrIsInvalid(max_lsn))
		{
			/*
			 * As noted above, elog(ERROR) is not acceptable here, so if
			 * XLogFlush were to fail, we must PANIC.  This isn't much of a
			 * restriction because XLogFlush is just about all critical
			 * section anyway, but let's make sure.
			 */
			START_CRIT_SECTION();
			XLogFlush(max_lsn);
			END_CRIT_SECTION();
		}
	}

	/*
	 * During a Flush, we may already have the desired file open.
	 */
	if (fdata)
	{
		int			i;

		for (i = 0; i < fdata->num_files; i++)
		{
			if (fdata->segno[i] == segno)
			{
				fd = fdata->fd[i];
				break;
			}
		}
	}

	if (fd < 0)
	{
		/*
		 * If the file doesn't already exist, we should create it.  It is
		 * possible for this to need to happen when writing a page that's not
		 * first in its segment; we assume the OS can cope with that. (Note:
		 * it might seem that it'd be okay to create files only when
		 * NoLruZeroPage is called for the first page of a segment.
		 * However, if after a crash and restart the REDO logic elects to
		 * replay the log from a checkpoint before the latest one, then it's
		 * possible that we will get commands to set transaction status of
		 * transactions that have already been truncated from the commit log.
		 * Easiest way to deal with that is to accept references to
		 * nonexistent files here and in NolruPhysicalReadPage.)
		 *
		 * Note: it is possible for more than one backend to be executing this
		 * code simultaneously for different pages of the same file. Hence,
		 * don't use O_EXCL or O_TRUNC or anything like that.
		 */
		NolruFileName(ctl, path, segno);
		fd = OpenTransientFile(path, O_RDWR | O_CREAT | PG_BINARY,
							   S_IRUSR | S_IWUSR);
		if (fd < 0)
		{
			nolru_errcause = NOLRU_OPEN_FAILED;
			nolru_errno = errno;
			return false;
		}

		if (fdata)
		{
			if (fdata->num_files < MAX_FLUSH_BUFFERS)
			{
				fdata->fd[fdata->num_files] = fd;
				fdata->segno[fdata->num_files] = segno;
				fdata->num_files++;
			}
			else
			{
				/*
				 * In the unlikely event that we exceed MAX_FLUSH_BUFFERS,
				 * fall back to treating it as a standalone write.
				 */
				fdata = NULL;
			}
		}
	}

	if (lseek(fd, (off_t) offset, SEEK_SET) < 0)
	{
		nolru_errcause = NOLRU_SEEK_FAILED;
		nolru_errno = errno;
		if (!fdata)
			CloseTransientFile(fd);
		return false;
	}

	errno = 0;
	if (write(fd, shared->page_buffer + ((long)slotno * BLCKSZ), BLCKSZ) != BLCKSZ)
	{
		/* if write didn't set errno, assume problem is no disk space */
		if (errno == 0)
			errno = ENOSPC;
		nolru_errcause = NOLRU_WRITE_FAILED;
		nolru_errno = errno;
		if (!fdata)
			CloseTransientFile(fd);
		return false;
	}

	/*
	 * If not part of Flush, need to fsync now.  We assume this happens
	 * infrequently enough that it's not a performance issue.
	 */
	if (!fdata)
	{
		if (ctl->do_fsync && pg_fsync(fd))
		{
			nolru_errcause = NOLRU_FSYNC_FAILED;
			nolru_errno = errno;
			CloseTransientFile(fd);
			return false;
		}

		if (CloseTransientFile(fd))
		{
			nolru_errcause = NOLRU_CLOSE_FAILED;
			nolru_errno = errno;
			return false;
		}
	}

	return true;
}

/*
 * Issue the error message after failure of NolruPhysicalReadPage or
 * NolruPhysicalWritePage.  Call this after cleaning up shared-memory state.
 */
static void
NolruReportIOError(NolruCtl ctl, int pageno, TransactionId xid)
{
	int			segno = pageno / NOLRU_PAGES_PER_SEGMENT;
	int			rpageno = pageno % NOLRU_PAGES_PER_SEGMENT;
	int			offset = rpageno * BLCKSZ;
	char		path[MAXPGPATH];

	NolruFileName(ctl, path, segno);
	errno = nolru_errno;
	switch (nolru_errcause)
	{
		case NOLRU_OPEN_FAILED:
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not access status of transaction %u", xid),
					 errdetail("Could not open file \"%s\": %m.", path)));
			break;
		case NOLRU_SEEK_FAILED:
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not access status of transaction %u", xid),
				 errdetail("Could not seek in file \"%s\" to offset %u: %m.",
						   path, offset)));
			break;
		case NOLRU_READ_FAILED:
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not access status of transaction %u", xid),
			   errdetail("Could not read from file \"%s\" at offset %u: %m.",
						 path, offset)));
			break;
		case NOLRU_WRITE_FAILED:
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not access status of transaction %u", xid),
				errdetail("Could not write to file \"%s\" at offset %u: %m.",
						  path, offset)));
			break;
		case NOLRU_FSYNC_FAILED:
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not access status of transaction %u", xid),
					 errdetail("Could not fsync file \"%s\": %m.",
							   path)));
			break;
		case NOLRU_CLOSE_FAILED:
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not access status of transaction %u", xid),
					 errdetail("Could not close file \"%s\": %m.",
							   path)));
			break;
		default:
			/* can't get here, we trust */
			elog(ERROR, "unrecognized NoLru error cause: %d",
				 (int) nolru_errcause);
			break;
	}
}

inline static bool
SpinLockTry(volatile slock_t *lock) { return !TAS_SPIN(lock); }

static int
NolruSelectLRUPage(NolruCtl ctl, int pageno)
{
	int slotno = pageno & ((1LL << 31) - 1);

	proceed_head(ctl, pageno);
	if (page_diff(ctl->shared->head_page_number, ctl->shared->tail_page_number) <= MAX_DIRTY_PAGES) return slotno;

/*
 * 'ctl->shared->doing' is not for the purpose of spin lock.
 * It's just for a flag indicating whether some process is 
 * trying to invoke  NolruInternalWritePage().
 */
	if (SpinLockTry(&ctl->shared->doing)) {
		uint tail;

	conflict_retry:
		tail = ctl->shared->tail_page_number;
	cas_retry:
		do {
			uint newv = next_page(tail), curv;

			while (!ctl->shared->page_dirty[newv] && 
				   PAGE_CMP_LT(ctl->shared->head_page_number, newv)) newv = next_page(newv);
			if (!pg_atomic_compare_exchange_u32((volatile struct pg_atomic_uint32 *) &ctl->shared->tail_page_number, &tail, newv)) goto cas_retry;
			for (curv = next_page(tail); PAGE_CMP_LT(curv, newv); curv = next_page(curv)) {
				if (ctl->shared->page_dirty[curv]) goto conflict_retry;
			}
			NolruInternalWritePage(ctl, tail, NULL);
			SpinLockRelease(&ctl->shared->doing);
		} while (0);
	}
	return slotno;
}

/*
 * Flush dirty pages to disk during checkpoint or database shutdown
 */
void
NoLruFlush(NolruCtl ctl, bool checkpoint)
{
	NolruShared	shared = ctl->shared;
	NolruFlushData fdata;
	int			slotno;
	int			pageno = 0;
	int			i;
	bool		ok;

	/*
	 * Find and write dirty pages
	 */
	fdata.num_files = 0;

	LWLockAcquire(shared->ControlLock, LW_EXCLUSIVE);

	for (slotno = ctl->shared->tail_page_number; slotno <= ctl->shared->head_page_number; slotno = next_page(slotno))
	{
		NolruInternalWritePage(ctl, slotno, &fdata);

		/*
		 * When called during a checkpoint, we cannot assert that the slot is
		 * clean now, since another process might have re-dirtied it already.
		 * That's okay.
		 */
		Assert(checkpoint ||
			   shared->page_status[slotno] == NOLRU_PAGE_EMPTY ||
			   (shared->page_status[slotno] == NOLRU_PAGE_VALID &&
				!shared->page_dirty[slotno]));
	}

	LWLockRelease(shared->ControlLock);

	/*
	 * Now fsync and close any files that were open
	 */
	ok = true;
	for (i = 0; i < fdata.num_files; i++)
	{
		if (ctl->do_fsync && pg_fsync(fdata.fd[i]))
		{
			nolru_errcause = NOLRU_FSYNC_FAILED;
			nolru_errno = errno;
			pageno = fdata.segno[i] * NOLRU_PAGES_PER_SEGMENT;
			ok = false;
		}

		if (CloseTransientFile(fdata.fd[i]))
		{
			nolru_errcause = NOLRU_CLOSE_FAILED;
			nolru_errno = errno;
			pageno = fdata.segno[i] * NOLRU_PAGES_PER_SEGMENT;
			ok = false;
		}
	}
	if (!ok)
		NolruReportIOError(ctl, pageno, InvalidTransactionId);
}

/*
 * Remove all segments before the one holding the passed page number
 */
void
NoLruTruncate(NolruCtl ctl, int cutoffPage)
{
	NolruShared	shared = ctl->shared;
	int			slotno;

	/*
	 * The cutoff point is the start of the segment containing cutoffPage.
	 */
	cutoffPage -= cutoffPage % NOLRU_PAGES_PER_SEGMENT;

	/*
	 * Scan shared memory and remove any pages preceding the cutoff page, to
	 * ensure we won't rewrite them later.  (Since this is normally called in
	 * or just after a checkpoint, any dirty pages should have been flushed
	 * already ... we're just being extra careful here.)
	 */
	LWLockAcquire(shared->ControlLock, LW_EXCLUSIVE);

restart:;

	/*
	 * While we are holding the lock, make an important safety check: the
	 * planned cutoff point must be <= the current endpoint page. Otherwise we
	 * have already wrapped around, and proceeding with the truncation would
	 * risk removing the current segment.
	 */
	if (ctl->PagePrecedes(shared->latest_page_number, cutoffPage))
	{
		LWLockRelease(shared->ControlLock);
		ereport(LOG,
		  (errmsg("could not truncate directory \"%s\": apparent wraparound",
				  ctl->Dir)));
		return;
	}

	for (slotno = ctl->shared->tail_page_number; slotno <= ctl->shared->head_page_number; slotno = next_page(slotno))
	{
		if (shared->page_status[slotno] == NOLRU_PAGE_EMPTY)
			continue;
		if (!ctl->PagePrecedes(slotno, cutoffPage))
			continue;

		/*
		 * If page is clean, just change state to EMPTY (expected case).
		 */
		if (shared->page_status[slotno] == NOLRU_PAGE_VALID &&
			!shared->page_dirty[slotno])
		{
			shared->page_status[slotno] = NOLRU_PAGE_EMPTY;
			continue;
		}

		/*
		 * Hmm, we have (or may have) I/O operations acting on the page, so
		 * we've got to wait for them to finish and then start again. This is
		 * the same logic as in NolruSelectLRUPage.  (XXX if page is dirty,
		 * wouldn't it be OK to just discard it without writing it?  For now,
		 * keep the logic the same as it was.)
		 */
		if (shared->page_status[slotno] == NOLRU_PAGE_VALID)
			NolruInternalWritePage(ctl, slotno, NULL);
		else
			NoLruWaitIO(ctl, slotno);
		goto restart;
	}

	LWLockRelease(shared->ControlLock);

	/* Now we can remove the old segment(s) */
	(void) NolruScanDirectory(ctl, NolruScanDirCbDeleteCutoff, &cutoffPage);
}

static void
NolruInternalDeleteSegment(NolruCtl ctl, char *filename)
{
	char		path[MAXPGPATH];

	snprintf(path, MAXPGPATH, "%s/%s", ctl->Dir, filename);
	ereport(DEBUG2,
			(errmsg("removing file \"%s\"", path)));
	unlink(path);
}

/*
 * NolruScanDirectory callback
 *		This callback reports true if there's any segment prior to the one
 *		containing the page passed as "data".
 */
bool
NolruScanDirCbReportPresence(NolruCtl ctl, char *filename, int segpage, void *data)
{
	int			cutoffPage = *(int *) data;

	cutoffPage -= cutoffPage % NOLRU_PAGES_PER_SEGMENT;

	if (ctl->PagePrecedes(segpage, cutoffPage))
		return true;			/* found one; don't iterate any more */

	return false;				/* keep going */
}

/*
 * NolruScanDirectory callback.
 *		This callback deletes segments prior to the one passed in as "data".
 */
static bool
NolruScanDirCbDeleteCutoff(NolruCtl ctl, char *filename, int segpage, void *data)
{
	int			cutoffPage = *(int *) data;

	if (ctl->PagePrecedes(segpage, cutoffPage))
		NolruInternalDeleteSegment(ctl, filename);

	return false;				/* keep going */
}

/*
 * NolruScanDirectory callback.
 *		This callback deletes all segments.
 */
bool
NolruScanDirCbDeleteAll(NolruCtl ctl, char *filename, int segpage, void *data)
{
	NolruInternalDeleteSegment(ctl, filename);

	return false;				/* keep going */
}

/*
 * Scan the NoLRU directory and apply a callback to each file found in it.
 *
 * If the callback returns true, the scan is stopped.  The last return value
 * from the callback is returned.
 *
 * The callback receives the following arguments: 1. the NolruCtl struct for the
 * nolru being truncated; 2. the filename being considered; 3. the page number
 * for the first page of that file; 4. a pointer to the opaque data given to us
 * by the caller.
 *
 * Note that the ordering in which the directory is scanned is not guaranteed.
 *
 * Note that no locking is applied.
 */
bool
NolruScanDirectory(NolruCtl ctl, NolruScanCallback callback, void *data)
{
	bool		retval = false;
	DIR		   *cldir;
	struct dirent *clde;
	int			segno;
	int			segpage;

	cldir = AllocateDir(ctl->Dir);
	while ((clde = ReadDir(cldir, ctl->Dir)) != NULL)
	{
		size_t		len;

		len = strlen(clde->d_name);

		if ((len == 4 || len == 5) &&
			strspn(clde->d_name, "0123456789ABCDEF") == len)
		{
			segno = (int) strtol(clde->d_name, NULL, 16);
			segpage = segno * NOLRU_PAGES_PER_SEGMENT;

			elog(DEBUG2, "NolruScanDirectory invoking callback on %s/%s",
				 ctl->Dir, clde->d_name);
			retval = callback(ctl, clde->d_name, segpage, data);
			if (retval)
				break;
		}
	}
	FreeDir(cldir);

	return retval;
}
