/*-------------------------------------------------------------------------
 *
 * nolru.h
 *		Manager for transaction status logfiles without using LRU buffering
 *
 * Portions Copyright (c) 2016, Takashi Horikawa
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/nolru.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef NOLRU_H
#define NOLRU_H

#include "access/xlogdefs.h"
#include "storage/lwlock.h"
#include "storage/spin.h"

/*
 * Define NOLRU segment size.  A page is the same BLCKSZ as is used everywhere
 * else in Postgres.  The segment size can be chosen somewhat arbitrarily;
 * we make it 32 pages by default, or 256Kb, i.e. 1M transactions for CLOG
 * or 64K transactions for SUBTRANS.
 *
 * Note: because TransactionIds are 32 bits and wrap around at 0xFFFFFFFF,
 * page numbering also wraps around at 0xFFFFFFFF/xxxx_XACTS_PER_PAGE (where
 * xxxx is CLOG or SUBTRANS, respectively), and segment numbering at
 * 0xFFFFFFFF/xxxx_XACTS_PER_PAGE/NOLRU_PAGES_PER_SEGMENT.  We need
 * take no explicit notice of that fact in nolru.c, except when comparing
 * segment and page numbers in NoLruTruncate (see PagePrecedes()).
 *
 * Note: nolru.c currently assumes that segment file names will be four hex
 * digits.  This sets a lower bound on the segment size (64K transactions
 * for 32-bit TransactionIds).
 */
#define NOLRU_PAGES_PER_SEGMENT	32
#define NUM_CLOG_PARTITIONS  32

/* Maximum length of an NOLRU name */
#define NOLRU_MAX_NAME_LENGTH	32

/*
 * Page status codes.  Note that these do not include the "dirty" bit.
 * page_dirty can be TRUE only in the VALID or WRITE_IN_PROGRESS states;
 * in the latter case it implies that the page has been re-dirtied since
 * the write started.
 */
typedef enum
{
	NOLRU_PAGE_EMPTY,			/* buffer is not in use */
	NOLRU_PAGE_READ_IN_PROGRESS, /* page is being read in */
	NOLRU_PAGE_VALID,			/* page is valid and not being written */
	NOLRU_PAGE_WRITE_IN_PROGRESS /* page is being written out */
} NolruPageStatus;

/*
 * Shared-memory state
 */
typedef struct NolruSharedData
{
	LWLock	   *ControlLock;

	/* Number of buffers managed by this NOLRU structure */
	int			num_slots;

	/*
	 * Arrays holding info for each buffer slot.  Page number is undefined
	 * when status is EMPTY, as is page_lru_count.
	 */
	char	  *page_buffer;
	NolruPageStatus *page_status;
	bool	   *page_dirty;
	int		   *page_number;

	/*
	 * Optional array of WAL flush LSNs associated with entries in the NOLRU
	 * pages.  If not zero/NULL, we must flush WAL before writing pages (true
	 * for pg_clog, false for multixact, pg_subtrans, pg_notify).  group_lsn[]
	 * has lsn_groups_per_page entries per buffer slot, each containing the
	 * highest LSN known for a contiguous group of NOLRU entries on that slot's
	 * page.
	 */
	XLogRecPtr *group_lsn;
	int			lsn_groups_per_page;

	/*----------
	 * We mark a page "most recently used" by setting
	 *		page_lru_count[slotno] = ++cur_lru_count;
	 * The oldest page is therefore the one with the highest value of
	 *		cur_lru_count - page_lru_count[slotno]
	 * The counts will eventually wrap around, but this calculation still
	 * works as long as no page's age exceeds INT_MAX counts.
	 *----------
	 */
	int			cur_lru_count;

	/*
	 * latest_page_number is the page number of the current end of the log;
	 * this is not critical data, since we use it only to avoid swapping out
	 * the latest page.
	 */
	int			latest_page_number;

	/* LWLocks */
	int			lwlock_tranche_id;
	LWLockTranche lwlock_tranche;
	char		lwlock_tranche_name[NOLRU_MAX_NAME_LENGTH];
	LWLockPadded *buffer_locks;

	int			tail_page_number;
	int			head_page_number;

	slock_t		mutex[NUM_CLOG_PARTITIONS];
	slock_t		doing;
} NolruSharedData;

typedef NolruSharedData *NolruShared;

/*
 * NolruCtlData is an unshared structure that points to the active information
 * in shared memory.
 */
typedef struct NolruCtlData
{
	NolruShared	shared;

	/*
	 * This flag tells whether to fsync writes (true for pg_clog and multixact
	 * stuff, false for pg_subtrans and pg_notify).
	 */
	bool		do_fsync;

	/*
	 * Decide which of two page numbers is "older" for truncation purposes. We
	 * need to use comparison of TransactionIds here in order to do the right
	 * thing with wraparound XID arithmetic.
	 */
	bool		(*PagePrecedes) (int, int);

	/*
	 * Dir is set during NoLruInit and does not change thereafter. Since
	 * it's always the same, it doesn't need to be in shared memory.
	 */
	char		Dir[64];
} NolruCtlData;

typedef NolruCtlData *NolruCtl;


extern Size NoLruShmemSize(int nslots, int nlsns);
extern void NoLruInit(NolruCtl ctl, const char *name, int nslots, int nlsns,
			  LWLock *ctllock, const char *subdir, int tranche_id);
extern int	NoLruZeroPage(NolruCtl ctl, int pageno);
extern int NoLruReadPage(NolruCtl ctl, int pageno, bool write_ok,
				  TransactionId xid);
extern int NoLruReadPage_ReadOnly(NolruCtl ctl, int pageno,
						   TransactionId xid);
extern void NoLruWritePage(NolruCtl ctl, int slotno);
extern void NoLruFlush(NolruCtl ctl, bool allow_redirtied);
extern void NoLruTruncate(NolruCtl ctl, int cutoffPage);
extern bool NoLruDoesPhysicalPageExist(NolruCtl ctl, int pageno);

typedef bool (*NolruScanCallback) (NolruCtl ctl, char *filename, int segpage,
											  void *data);
extern bool NolruScanDirectory(NolruCtl ctl, NolruScanCallback callback, void *data);

/* NolruScanDirectory public callbacks */
extern bool NolruScanDirCbReportPresence(NolruCtl ctl, char *filename,
							int segpage, void *data);
extern bool NolruScanDirCbDeleteAll(NolruCtl ctl, char *filename, int segpage,
					   void *data);

#define PAGE_MAX			((int)(((1LL<<32)/(4*8192))-1))		/* 128M pages */
#define PAGE_CMP_GE(a, b)	(PAGE_MAX / 2 >= (((a) - (b)) & PAGE_MAX))
#define PAGE_CMP_LT(a, b)	(PAGE_MAX / 2 < (((a) - (b)) & PAGE_MAX))

#define update_tail(ClogCtl, slotno)									\
	do {																\
		uint oldv = ClogCtl->shared->tail_page_number;					\
		while (1) {														\
			if (PAGE_CMP_GE(slotno, oldv)) break;						\
			if (pg_atomic_compare_exchange_u32((volatile struct pg_atomic_uint32 *) &ClogCtl->shared->tail_page_number, &oldv, slotno)) break; \
		}																\
	} while(0)

#define proceed_head(ClogCtl, pageno)									\
	do {																\
		uint oldv = ClogCtl->shared->head_page_number;					\
		while (1) {														\
			if (PAGE_CMP_GE(oldv, pageno)) break;						\
			if (pg_atomic_compare_exchange_u32((volatile struct pg_atomic_uint32 *) &ClogCtl->shared->head_page_number, &oldv, pageno)) break; \
		}																\
	} while(0)

#define page_diff(head, tail) ((head - tail) & PAGE_MAX)
#define next_page(page) ((page+1) & PAGE_MAX)

#endif   /* NOLRU_H */
