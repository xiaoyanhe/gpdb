/*-------------------------------------------------------------------------
 *
 * vacuumlazy.c
 *	  Concurrent ("lazy") vacuuming.
 *
 *
 * The major space usage for LAZY VACUUM is storage for the array of dead
 * tuple TIDs, with the next biggest need being storage for per-disk-page
 * free space info.  We want to ensure we can vacuum even the very largest
 * relations with finite memory space usage.  To do that, we set upper bounds
 * on the number of tuples and pages we will keep track of at once.
 *
 * We are willing to use at most maintenance_work_mem memory space to keep
 * track of dead tuples.  We initially allocate an array of TIDs of that size,
 * with an upper limit that depends on table size (this limit ensures we don't
 * allocate a huge area uselessly for vacuuming small tables).	If the array
 * threatens to overflow, we suspend the heap scan phase and perform a pass of
 * index cleanup and page compaction, then resume the heap scan with an empty
 * TID array.
 *
 * We can limit the storage for page free space to MaxFSMPages entries,
 * since that's the most the free space map will be willing to remember
 * anyway.	If the relation has fewer than that many pages with free space,
 * life is easy: just build an array of per-page info.	If it has more,
 * we store the free space info as a heap ordered by amount of free space,
 * so that we can discard the pages with least free space to ensure we never
 * have more than MaxFSMPages entries in all.  The surviving page entries
 * are passed to the free space map at conclusion of the scan.
 *
 * If we're processing a table with no indexes, we can just vacuum each page
 * as we go; there's no need to save up multiple tuples to minimize the number
 * of index scans performed.  So we don't use maintenance_work_mem memory for
 * the TID array, just enough to hold as many heap tuples as fit on one page.
 *
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/commands/vacuumlazy.c,v 1.106 2008/03/26 21:10:38 alvherre Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/genam.h"
#include "access/heapam.h"
#include "access/nbtree.h"
#include "access/transam.h"
#include "access/aosegfiles.h"
#include "access/aocssegfiles.h"
#include "access/aomd.h"
#include "access/appendonly_compaction.h"
#include "access/aocs_compaction.h"
#include "commands/dbcommands.h"
#include "commands/vacuum.h"
#include "catalog/catalog.h"
#include "catalog/indexing.h"
#include "catalog/pg_appendonly_fn.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc.h"
#include "cdb/cdbappendonlyam.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/autovacuum.h"
#include "storage/freespace.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/pg_rusage.h"
#include "utils/tqual.h"

#include "cdb/cdbvars.h"
#include "cdb/cdbpersistentfilesysobj.h"
#include "storage/smgr.h"
#include "utils/faultinjector.h"
#include "utils/snapmgr.h"


/*
 * Space/time tradeoff parameters: do these need to be user-tunable?
 *
 * To consider truncating the relation, we want there to be at least
 * REL_TRUNCATE_MINIMUM or (relsize / REL_TRUNCATE_FRACTION) (whichever
 * is less) potentially-freeable pages.
 */
#define REL_TRUNCATE_MINIMUM	1000
#define REL_TRUNCATE_FRACTION	16

/*
 * Guesstimation of number of dead tuples per page.  This is used to
 * provide an upper limit to memory allocated when vacuuming small
 * tables.
 */
#define LAZY_ALLOC_TUPLES		MaxHeapTuplesPerPage

typedef struct LVRelStats
{
	/* hasindex = true means two-pass strategy; false means one-pass */
	bool		hasindex;
	/* Overall statistics about rel */
	BlockNumber rel_pages;
	double		rel_tuples;
	BlockNumber pages_removed;
	double		tuples_deleted;
	BlockNumber nonempty_pages; /* actually, last nonempty page + 1 */
	Size		threshold;		/* minimum interesting free space */
	/* List of TIDs of tuples we intend to delete */
	/* NB: this list is ordered by TID address */
	int			num_dead_tuples;	/* current # of entries */
	int			max_dead_tuples;	/* # slots allocated in array */
	ItemPointer dead_tuples;	/* array of ItemPointerData */
	/* Array or heap of per-page info about free space */
	/* We use a simple array until it fills up, then convert to heap */
	bool		fs_is_heap;		/* are we using heap organization? */
	int			num_free_pages; /* current # of entries */
	int			max_free_pages; /* # slots allocated in array */
	FSMPageData *free_pages;	/* array or heap of blkno/avail */
	BlockNumber tot_free_pages; /* total pages with >= threshold space */
	int			num_index_scans;
} LVRelStats;


/* A few variables that don't seem worth passing around as parameters */
static int	elevel = -1;

static TransactionId OldestXmin;
static TransactionId FreezeLimit;

static BufferAccessStrategy vac_strategy;


/* non-export function prototypes */
static void lazy_vacuum_aorel(Relation onerel, VacuumStmt *vacstmt,
				  List *updated_stats);
static void lazy_scan_heap(Relation onerel, LVRelStats *vacrelstats,
			   Relation *Irel, int nindexes, List *updated_stats);
static void lazy_vacuum_heap(Relation onerel, LVRelStats *vacrelstats);
static void lazy_vacuum_index(Relation indrel,
				  IndexBulkDeleteResult **stats,
				  LVRelStats *vacrelstats);
static void lazy_cleanup_index(Relation indrel,
				   IndexBulkDeleteResult *stats,
				   LVRelStats *vacrelstats,
				   List *updated_stats);
static int lazy_vacuum_page(Relation onerel, BlockNumber blkno, Buffer buffer,
				 int tupindex, LVRelStats *vacrelstats);
static void lazy_truncate_heap(Relation onerel, LVRelStats *vacrelstats);
static BlockNumber count_nondeletable_pages(Relation onerel,
						 LVRelStats *vacrelstats);
static void lazy_space_alloc(LVRelStats *vacrelstats, BlockNumber relblocks);
static void lazy_record_dead_tuple(LVRelStats *vacrelstats,
					   ItemPointer itemptr);
static void lazy_record_free_space(LVRelStats *vacrelstats,
					   BlockNumber page, Size avail);
static bool lazy_tid_reaped(ItemPointer itemptr, void *state);
static void lazy_update_fsm(Relation onerel, LVRelStats *vacrelstats);
static int	vac_cmp_itemptr(const void *left, const void *right);
static int	vac_cmp_page_spaces(const void *left, const void *right);


/*
 *	lazy_vacuum_rel() -- perform LAZY VACUUM for one heap relation
 *
 *		This routine vacuums a single heap, cleans out its indexes, and
 *		updates its relpages and reltuples statistics.
 *
 *		At entry, we have already established a transaction and opened
 *		and locked the relation.
 *
 *		The return value indicates whether this function has held off
 *		interrupts -- caller must RESUME_INTERRUPTS() after commit if true.
 */
bool
lazy_vacuum_rel(Relation onerel, VacuumStmt *vacstmt,
				BufferAccessStrategy bstrategy, List *updated_stats)
{
	LVRelStats *vacrelstats;
	Relation   *Irel;
	int			nindexes;
	BlockNumber possibly_freeable;
	PGRUsage	ru0;
	TimestampTz starttime = 0;
	bool		heldoff = false;

	pg_rusage_init(&ru0);

	/* measure elapsed time iff autovacuum logging requires it */
	if (IsAutoVacuumWorkerProcess() && Log_autovacuum_min_duration > 0)
		starttime = GetCurrentTimestamp();

	if (vacstmt->verbose)
		elevel = INFO;
	else
		elevel = DEBUG2;

	if (Gp_role == GP_ROLE_DISPATCH)
		elevel = DEBUG2; /* vacuum and analyze messages aren't interesting from the QD */

#ifdef FAULT_INJECTOR
	if (vacuumStatement_IsInAppendOnlyDropPhase(vacstmt))
	{
			FaultInjector_InjectFaultIfSet(
				CompactionBeforeSegmentFileDropPhase,
				DDLNotSpecified,
				"",	// databaseName
				""); // tableName
	}
	if (vacummStatement_IsInAppendOnlyCleanupPhase(vacstmt))
	{
			FaultInjector_InjectFaultIfSet(
				CompactionBeforeCleanupPhase,
				DDLNotSpecified,
				"",	// databaseName
				""); // tableName
	}
#endif

	/*
	 * MPP-23647.  Update xid limits for heap as well as appendonly
	 * relations.  This allows setting relfrozenxid to correct value
	 * for an appendonly (AO/CO) table.
	 */
	vac_strategy = bstrategy;

	vacuum_set_xid_limits(vacstmt->freeze_min_age, onerel->rd_rel->relisshared,
						  &OldestXmin, &FreezeLimit);

	/*
	 * Execute the various vacuum operations. Appendonly tables are treated
	 * differently.
	 */
	if (RelationIsAoRows(onerel) || RelationIsAoCols(onerel))
	{
		lazy_vacuum_aorel(onerel, vacstmt, updated_stats);
		return false;
	}

	vacrelstats = (LVRelStats *) palloc0(sizeof(LVRelStats));

	/* heap relation */

	/* Set threshold for interesting free space = average request size */
	/* XXX should we scale it up or down?  Adjust vacuum.c too, if so */
	vacrelstats->threshold = GetAvgFSMRequestSize(&onerel->rd_node);

	vacrelstats->num_index_scans = 0;

	/* Open all indexes of the relation */
	vac_open_indexes(onerel, RowExclusiveLock, &nindexes, &Irel);
	vacrelstats->hasindex = (nindexes > 0);

	/* Do the vacuuming */
	lazy_scan_heap(onerel, vacrelstats, Irel, nindexes, updated_stats);

	/* Done with indexes */
	vac_close_indexes(nindexes, Irel, NoLock);

	/*
	 * Optionally truncate the relation.
	 *
	 * Don't even think about it unless we have a shot at releasing a goodly
	 * number of pages.  Otherwise, the time taken isn't worth it.
	 *
	 * Note that after we've truncated the heap, it's too late to abort the
	 * transaction; doing so would lose the sinval messages needed to tell
	 * the other backends about the table being shrunk.  We prevent interrupts
	 * in that case; caller is responsible for re-enabling them after
	 * committing the transaction.
	 */
	possibly_freeable = vacrelstats->rel_pages - vacrelstats->nonempty_pages;
	if (possibly_freeable > 0 &&
		(possibly_freeable >= REL_TRUNCATE_MINIMUM ||
		 possibly_freeable >= vacrelstats->rel_pages / REL_TRUNCATE_FRACTION))
	{
		HOLD_INTERRUPTS();
		heldoff = true;
		lazy_truncate_heap(onerel, vacrelstats);
	}

	/* Update shared free space map with final free space info */
	lazy_update_fsm(onerel, vacrelstats);

	if (vacrelstats->tot_free_pages > MaxFSMPages)
		ereport(WARNING,
				(errmsg("relation \"%s.%s\" contains more than \"max_fsm_pages\" pages with useful free space",
						get_namespace_name(RelationGetNamespace(onerel)),
						RelationGetRelationName(onerel)),
				 /* Only suggest VACUUM FULL if > 20% free */
				 (vacrelstats->tot_free_pages > vacrelstats->rel_pages * 0.20) ?
				 errhint("Consider using VACUUM FULL on this relation or increasing the configuration parameter \"max_fsm_pages\".") :
				 errhint("Consider increasing the configuration parameter \"max_fsm_pages\".")));

	/* Update statistics in pg_class */
	vac_update_relstats_from_list(onerel,
						vacrelstats->rel_pages,
						vacrelstats->rel_tuples,
						vacrelstats->hasindex,
						FreezeLimit,
						updated_stats);

	/* report results to the stats collector, too */
	pgstat_report_vacuum(RelationGetRelid(onerel), onerel->rd_rel->relisshared,
						 true /*vacrelstats->scanned_all*/,
						 vacstmt->analyze, vacrelstats->rel_tuples);

	if (gp_indexcheck_vacuum == INDEX_CHECK_ALL ||
		(gp_indexcheck_vacuum == INDEX_CHECK_SYSTEM &&
		 PG_CATALOG_NAMESPACE == RelationGetNamespace(onerel)))
	{
		int			i;

		for (i = 0; i < nindexes; i++)
		{
			if (Irel[i]->rd_rel->relam == BTREE_AM_OID)
				_bt_validate_vacuum(Irel[i], onerel, OldestXmin);
		}
	}

	/* and log the action if appropriate */
	if (IsAutoVacuumWorkerProcess() && Log_autovacuum_min_duration >= 0)
	{
		if (Log_autovacuum_min_duration == 0 ||
			TimestampDifferenceExceeds(starttime, GetCurrentTimestamp(),
									   Log_autovacuum_min_duration))
			ereport(LOG,
					(errmsg("automatic vacuum of table \"%s.%s.%s\": index scans: %d\n"
							"pages: %d removed, %d remain\n"
							"tuples: %.0f removed, %.0f remain\n"
							"system usage: %s",
							get_database_name(MyDatabaseId),
							get_namespace_name(RelationGetNamespace(onerel)),
							RelationGetRelationName(onerel),
							vacrelstats->num_index_scans,
						  vacrelstats->pages_removed, vacrelstats->rel_pages,
						vacrelstats->tuples_deleted, vacrelstats->rel_tuples,
							pg_rusage_show(&ru0))));
	}

	return heldoff;
}

/*
 * lazy_vacuum_aorel -- perform LAZY VACUUM for one Append-only relation.
 */
static void
lazy_vacuum_aorel(Relation onerel, VacuumStmt *vacstmt, List *updated_stats)
{
	LVRelStats *vacrelstats;
	bool		update_relstats = true;

	vacrelstats = (LVRelStats *) palloc0(sizeof(LVRelStats));

	if (vacuumStatement_IsInAppendOnlyPreparePhase(vacstmt))
	{
		elogif(Debug_appendonly_print_compaction, LOG,
			   "Vacuum prepare phase %s", RelationGetRelationName(onerel));

		vacuum_appendonly_indexes(onerel, vacstmt, updated_stats);
		if (RelationIsAoRows(onerel))
			AppendOnlyTruncateToEOF(onerel);
		else
			AOCSTruncateToEOF(onerel);

		/*
		 * MPP-23647.  For empty tables, we skip compaction phase
		 * and cleanup phase.  Therefore, we update the stats
		 * (specifically, relfrozenxid) in prepare phase if the
		 * table is empty.  Otherwise, the stats will be updated in
		 * the cleanup phase, when we would have computed the
		 * correct values for stats.
		 */
		if (vacstmt->appendonly_relation_empty)
		{
			update_relstats = true;
			/*
			 * For an empty relation, the only stats we care about
			 * is relfrozenxid and relhasindex.  We need to be
			 * mindful of correctly setting relhasindex here.
			 * relfrozenxid is already taken care of above by
			 * calling vacuum_set_xid_limits().
			 */
			vacrelstats->hasindex = onerel->rd_rel->relhasindex;
		}
		else
		{
			/*
			 * For a non-empty relation, follow the usual
			 * compaction phases and do not update stats in
			 * prepare phase.
			 */
			update_relstats = false;
	}
	}
	else if (!vacummStatement_IsInAppendOnlyCleanupPhase(vacstmt))
	{
		vacuum_appendonly_rel(onerel, vacstmt);
		update_relstats = false;
	}
	else
	{
		elogif(Debug_appendonly_print_compaction, LOG,
			   "Vacuum cleanup phase %s", RelationGetRelationName(onerel));

		vacuum_appendonly_fill_stats(onerel, ActiveSnapshot,
									 &vacrelstats->rel_pages,
									 &vacrelstats->rel_tuples,
									 &vacrelstats->hasindex);
		/* reset the remaining LVRelStats values */
		vacrelstats->nonempty_pages = 0;
		vacrelstats->num_dead_tuples = 0;
		vacrelstats->max_dead_tuples = 0;
		vacrelstats->tuples_deleted = 0;
		vacrelstats->tot_free_pages = 0;
		vacrelstats->fs_is_heap = false;
		vacrelstats->num_free_pages = 0;
		vacrelstats->max_free_pages = 0;
		vacrelstats->pages_removed = 0;
	}

	if (update_relstats)
	{
		/* Update statistics in pg_class */
		vac_update_relstats_from_list(onerel,
							vacrelstats->rel_pages,
							vacrelstats->rel_tuples,
							vacrelstats->hasindex,
							FreezeLimit,
							updated_stats);

		/* report results to the stats collector, too */
		pgstat_report_vacuum(RelationGetRelid(onerel),
							 onerel->rd_rel->relisshared,
							 true /*vacrelstats->scanned_all*/,
							 vacstmt->analyze, vacrelstats->rel_tuples);
	}
}

/*
 *	lazy_scan_heap() -- scan an open heap relation
 *
 *		This routine sets commit status bits, builds lists of dead tuples
 *		and pages with free space, and calculates statistics on the number
 *		of live tuples in the heap.  When done, or when we run low on space
 *		for dead-tuple TIDs, invoke vacuuming of indexes and heap.
 *
 *		If there are no indexes then we just vacuum each dirty page as we
 *		process it, since there's no point in gathering many tuples.
 */
static void
lazy_scan_heap(Relation onerel, LVRelStats *vacrelstats,
			   Relation *Irel, int nindexes, List *updated_stats)
{
	MIRROREDLOCK_BUFMGR_DECLARE;

	BlockNumber nblocks,
				blkno;
	HeapTupleData tuple;
	char	   *relname;
	BlockNumber empty_pages,
				vacuumed_pages;
	double		num_tuples,
				tups_vacuumed,
				nkeep,
				nunused;
	IndexBulkDeleteResult **indstats;
	int			i;
	int reindex_count = 1;
	PGRUsage	ru0;

	/* Fetch gp_persistent_relation_node information that will be added to XLOG record. */
	RelationFetchGpRelationNodeForXLog(onerel);

	pg_rusage_init(&ru0);

	relname = RelationGetRelationName(onerel);
	ereport(elevel,
			(errmsg("vacuuming \"%s.%s\"",
					get_namespace_name(RelationGetNamespace(onerel)),
					relname)));

	empty_pages = vacuumed_pages = 0;
	num_tuples = tups_vacuumed = nkeep = nunused = 0;

	indstats = (IndexBulkDeleteResult **)
		palloc0(nindexes * sizeof(IndexBulkDeleteResult *));

	nblocks = RelationGetNumberOfBlocks(onerel);
	vacrelstats->rel_pages = nblocks;
	vacrelstats->nonempty_pages = 0;

	lazy_space_alloc(vacrelstats, nblocks);

	for (blkno = 0; blkno < nblocks; blkno++)
	{
		Buffer		buf;
		Page		page;
		OffsetNumber offnum,
					maxoff;
		bool		tupgone,
					hastup;
		int			prev_dead_count;
		OffsetNumber frozen[MaxOffsetNumber];
		int			nfrozen;

		vacuum_delay_point();

		/*
		 * If we are close to overrunning the available space for dead-tuple
		 * TIDs, pause and do a cycle of vacuuming before we tackle this page.
		 */
		if ((vacrelstats->max_dead_tuples - vacrelstats->num_dead_tuples) < MaxHeapTuplesPerPage &&
			vacrelstats->num_dead_tuples > 0)
		{
			/* Remove index entries */
			for (i = 0; i < nindexes; i++)
				lazy_vacuum_index(Irel[i], &indstats[i], vacrelstats);

			reindex_count++;

			/* Remove tuples from heap */
			lazy_vacuum_heap(onerel, vacrelstats);
			/* Forget the now-vacuumed tuples, and press on */
			vacrelstats->num_dead_tuples = 0;
			vacrelstats->num_index_scans++;
		}

		/* -------- MirroredLock ---------- */
		MIRROREDLOCK_BUFMGR_LOCK;

		buf = ReadBufferWithStrategy(onerel, blkno, vac_strategy);

		/* We need buffer cleanup lock so that we can prune HOT chains. */
		LockBufferForCleanup(buf);

		page = BufferGetPage(buf);

		if (PageIsNew(page))
		{
			/*
			 * An all-zeroes page could be left over if a backend extends the
			 * relation but crashes before initializing the page. Reclaim such
			 * pages for use.
			 *
			 * We have to be careful here because we could be looking at a
			 * page that someone has just added to the relation and not yet
			 * been able to initialize (see RelationGetBufferForTuple). To
			 * protect against that, release the buffer lock, grab the
			 * relation extension lock momentarily, and re-lock the buffer. If
			 * the page is still uninitialized by then, it must be left over
			 * from a crashed backend, and we can initialize it.
			 *
			 * We don't really need the relation lock when this is a new or
			 * temp relation, but it's probably not worth the code space to
			 * check that, since this surely isn't a critical path.
			 *
			 * Note: the comparable code in vacuum.c need not worry because
			 * it's got exclusive lock on the whole relation.
			 */
			LockBuffer(buf, BUFFER_LOCK_UNLOCK);

			MIRROREDLOCK_BUFMGR_UNLOCK;
			/* -------- MirroredLock ---------- */

			LockRelationForExtension(onerel, ExclusiveLock);
			UnlockRelationForExtension(onerel, ExclusiveLock);

			/* -------- MirroredLock ---------- */
			MIRROREDLOCK_BUFMGR_LOCK;

			LockBufferForCleanup(buf);
			if (PageIsNew(page))
			{
				ereport(WARNING,
				(errmsg("relation \"%s\" page %u is uninitialized --- fixing",
						relname, blkno)));
				PageInit(page, BufferGetPageSize(buf), 0);

				/* must record in xlog so that changetracking will know about this change */
				log_heap_newpage(onerel, page, blkno);

				empty_pages++;
				lazy_record_free_space(vacrelstats, blkno,
									   PageGetHeapFreeSpace(page));
			}
			MarkBufferDirty(buf);
			UnlockReleaseBuffer(buf);

			MIRROREDLOCK_BUFMGR_UNLOCK;
			/* -------- MirroredLock ---------- */

			continue;
		}

		if (PageIsEmpty(page))
		{
			empty_pages++;
			lazy_record_free_space(vacrelstats, blkno,
								   PageGetHeapFreeSpace(page));
			UnlockReleaseBuffer(buf);

			MIRROREDLOCK_BUFMGR_UNLOCK;
			/* -------- MirroredLock ---------- */

			continue;
		}

		/*
		 * Prune all HOT-update chains in this page.
		 *
		 * We count tuples removed by the pruning step as removed by VACUUM.
		 */
		tups_vacuumed += heap_page_prune(onerel, buf, OldestXmin,
										 false, false);

		/*
		 * Now scan the page to collect vacuumable items and check for tuples
		 * requiring freezing.
		 */
		nfrozen = 0;
		hastup = false;
		prev_dead_count = vacrelstats->num_dead_tuples;
		maxoff = PageGetMaxOffsetNumber(page);
		for (offnum = FirstOffsetNumber;
			 offnum <= maxoff;
			 offnum = OffsetNumberNext(offnum))
		{
			ItemId		itemid;

			itemid = PageGetItemId(page, offnum);

			/* Unused items require no processing, but we count 'em */
			if (!ItemIdIsUsed(itemid))
			{
				nunused += 1;
				continue;
			}

			/* Redirect items mustn't be touched */
			if (ItemIdIsRedirected(itemid))
			{
				hastup = true;	/* this page won't be truncatable */
				continue;
			}

			ItemPointerSet(&(tuple.t_self), blkno, offnum);

			/*
			 * DEAD item pointers are to be vacuumed normally; but we don't
			 * count them in tups_vacuumed, else we'd be double-counting (at
			 * least in the common case where heap_page_prune() just freed up
			 * a non-HOT tuple).
			 */
			if (ItemIdIsDead(itemid))
			{
				lazy_record_dead_tuple(vacrelstats, &(tuple.t_self));
				continue;
			}

			Assert(ItemIdIsNormal(itemid));

			tuple.t_data = (HeapTupleHeader) PageGetItem(page, itemid);
			tuple.t_len = ItemIdGetLength(itemid);

			tupgone = false;

			switch (HeapTupleSatisfiesVacuum(onerel, tuple.t_data, OldestXmin, buf))
			{
				case HEAPTUPLE_DEAD:

					/*
					 * Ordinarily, DEAD tuples would have been removed by
					 * heap_page_prune(), but it's possible that the tuple
					 * state changed since heap_page_prune() looked.  In
					 * particular an INSERT_IN_PROGRESS tuple could have
					 * changed to DEAD if the inserter aborted.  So this
					 * cannot be considered an error condition.
					 *
					 * If the tuple is HOT-updated then it must only be
					 * removed by a prune operation; so we keep it just as if
					 * it were RECENTLY_DEAD.  Also, if it's a heap-only
					 * tuple, we choose to keep it, because it'll be a lot
					 * cheaper to get rid of it in the next pruning pass than
					 * to treat it like an indexed tuple.
					 */
					if (HeapTupleIsHotUpdated(&tuple) ||
						HeapTupleIsHeapOnly(&tuple))
						nkeep += 1;
					else
						tupgone = true; /* we can delete the tuple */
					break;
				case HEAPTUPLE_LIVE:
					/* Tuple is good --- but let's do some validity checks */
					if (onerel->rd_rel->relhasoids &&
						!OidIsValid(HeapTupleGetOid(&tuple)))
						elog(WARNING, "relation \"%s\" TID %u/%u: OID is invalid",
							 relname, blkno, offnum);
					break;
				case HEAPTUPLE_RECENTLY_DEAD:

					/*
					 * If tuple is recently deleted then we must not remove it
					 * from relation.
					 */
					nkeep += 1;
					break;
				case HEAPTUPLE_INSERT_IN_PROGRESS:
					/* This is an expected case during concurrent vacuum */
					break;
				case HEAPTUPLE_DELETE_IN_PROGRESS:
					/* This is an expected case during concurrent vacuum */
					break;
				default:
					elog(ERROR, "unexpected HeapTupleSatisfiesVacuum result");
					break;
			}

			if (tupgone)
			{
				lazy_record_dead_tuple(vacrelstats, &(tuple.t_self));
				tups_vacuumed += 1;
			}
			else
			{
				num_tuples += 1;
				hastup = true;

				/*
				 * Each non-removable tuple must be checked to see if it needs
				 * freezing.  Note we already have exclusive buffer lock.
				 */
				if (heap_freeze_tuple(tuple.t_data, &FreezeLimit,
									  InvalidBuffer, false))
					frozen[nfrozen++] = offnum;
			}
		}						/* scan along page */

		/*
		 * If we froze any tuples, mark the buffer dirty, and write a WAL
		 * record recording the changes.  We must log the changes to be
		 * crash-safe against future truncation of CLOG.
		 */
		if (nfrozen > 0)
		{
			MarkBufferDirty(buf);
			/* no XLOG for temp tables, though */
			if (!onerel->rd_istemp)
			{
				XLogRecPtr	recptr;

				recptr = log_heap_freeze(onerel, buf, FreezeLimit,
										 frozen, nfrozen);
				PageSetLSN(page, recptr);
			}
		}

		/*
		 * If there are no indexes then we can vacuum the page right now
		 * instead of doing a second scan.
		 */
		if (nindexes == 0 &&
			vacrelstats->num_dead_tuples > 0)
		{
			/* Remove tuples from heap */
			lazy_vacuum_page(onerel, blkno, buf, 0, vacrelstats);
			/* Forget the now-vacuumed tuples, and press on */
			vacrelstats->num_dead_tuples = 0;
			vacuumed_pages++;
		}

		/*
		 * If we remembered any tuples for deletion, then the page will be
		 * visited again by lazy_vacuum_heap, which will compute and record
		 * its post-compaction free space.	If not, then we're done with this
		 * page, so remember its free space as-is.	(This path will always be
		 * taken if there are no indexes.)
		 */
		if (vacrelstats->num_dead_tuples == prev_dead_count)
		{
			lazy_record_free_space(vacrelstats, blkno,
								   PageGetHeapFreeSpace(page));
		}

		/* Remember the location of the last page with nonremovable tuples */
		if (hastup)
			vacrelstats->nonempty_pages = blkno + 1;

		UnlockReleaseBuffer(buf);

		MIRROREDLOCK_BUFMGR_UNLOCK;
		/* -------- MirroredLock ---------- */

	}

	/* save stats for use later */
	vacrelstats->rel_tuples = num_tuples;
	vacrelstats->tuples_deleted = tups_vacuumed;

	/* If any tuples need to be deleted, perform final vacuum cycle */
	/* XXX put a threshold on min number of tuples here? */
	if (vacrelstats->num_dead_tuples > 0)
	{
		/* Remove index entries */
		for (i = 0; i < nindexes; i++)
			lazy_vacuum_index(Irel[i], &indstats[i], vacrelstats);

		reindex_count++;

		/* Remove tuples from heap */
		lazy_vacuum_heap(onerel, vacrelstats);
		vacrelstats->num_index_scans++;
	}

	/* Do post-vacuum cleanup and statistics update for each index */
	for (i = 0; i < nindexes; i++)
		lazy_cleanup_index(Irel[i], indstats[i], vacrelstats, updated_stats);

	/* If no indexes, make log report that lazy_vacuum_heap would've made */
	if (vacuumed_pages)
		ereport(elevel,
				(errmsg("\"%s\": removed %.0f row versions in %u pages",
						RelationGetRelationName(onerel),
						tups_vacuumed, vacuumed_pages)));

	ereport(elevel,
			(errmsg("\"%s\": found %.0f removable, %.0f nonremovable row versions in %u pages",
					RelationGetRelationName(onerel),
					tups_vacuumed, num_tuples, nblocks),
			 errdetail("%.0f dead row versions cannot be removed yet.\n"
					   "There were %.0f unused item pointers.\n"
					   "%u pages contain useful free space.\n"
					   "%u pages are entirely empty.\n"
					   "%s.",
					   nkeep,
					   nunused,
					   vacrelstats->tot_free_pages,
					   empty_pages,
					   pg_rusage_show(&ru0))));
}


/*
 *	lazy_vacuum_heap() -- second pass over the heap
 *
 *		This routine marks dead tuples as unused and compacts out free
 *		space on their pages.  Pages not having dead tuples recorded from
 *		lazy_scan_heap are not visited at all.
 *
 * Note: the reason for doing this as a second pass is we cannot remove
 * the tuples until we've removed their index entries, and we want to
 * process index entry removal in batches as large as possible.
 */
static void
lazy_vacuum_heap(Relation onerel, LVRelStats *vacrelstats)
{
	MIRROREDLOCK_BUFMGR_DECLARE;

	int			tupindex;
	int			npages;
	PGRUsage	ru0;

	pg_rusage_init(&ru0);
	npages = 0;

	tupindex = 0;

	/* Fetch gp_persistent_relation_node information that will be added to XLOG record. */
	RelationFetchGpRelationNodeForXLog(onerel);

	while (tupindex < vacrelstats->num_dead_tuples)
	{
		BlockNumber tblk;
		Buffer		buf;
		Page		page;

		vacuum_delay_point();

		tblk = ItemPointerGetBlockNumber(&vacrelstats->dead_tuples[tupindex]);

		/* -------- MirroredLock ---------- */
		MIRROREDLOCK_BUFMGR_LOCK;

		buf = ReadBufferWithStrategy(onerel, tblk, vac_strategy);
		LockBufferForCleanup(buf);
		tupindex = lazy_vacuum_page(onerel, tblk, buf, tupindex, vacrelstats);
		/* Now that we've compacted the page, record its available space */
		page = BufferGetPage(buf);
		lazy_record_free_space(vacrelstats, tblk,
							   PageGetHeapFreeSpace(page));
		UnlockReleaseBuffer(buf);

		MIRROREDLOCK_BUFMGR_UNLOCK;
		/* -------- MirroredLock ---------- */

		npages++;
	}

	ereport(elevel,
			(errmsg("\"%s\": removed %d row versions in %d pages",
					RelationGetRelationName(onerel),
					tupindex, npages),
			 errdetail("%s.",
					   pg_rusage_show(&ru0))));
}

/*
 *	lazy_vacuum_page() -- free dead tuples on a page
 *					 and repair its fragmentation.
 *
 * Caller must hold pin and buffer cleanup lock on the buffer.
 *
 * tupindex is the index in vacrelstats->dead_tuples of the first dead
 * tuple for this page.  We assume the rest follow sequentially.
 * The return value is the first tupindex after the tuples of this page.
 */
static int
lazy_vacuum_page(Relation onerel, BlockNumber blkno, Buffer buffer,
				 int tupindex, LVRelStats *vacrelstats)
{
	Page		page = BufferGetPage(buffer);
	OffsetNumber unused[MaxOffsetNumber];
	int			uncnt = 0;

	MIRROREDLOCK_BUFMGR_MUST_ALREADY_BE_HELD;

	START_CRIT_SECTION();

	for (; tupindex < vacrelstats->num_dead_tuples; tupindex++)
	{
		BlockNumber tblk;
		OffsetNumber toff;
		ItemId		itemid;

		tblk = ItemPointerGetBlockNumber(&vacrelstats->dead_tuples[tupindex]);
		if (tblk != blkno)
			break;				/* past end of tuples for this block */
		toff = ItemPointerGetOffsetNumber(&vacrelstats->dead_tuples[tupindex]);
		itemid = PageGetItemId(page, toff);
		ItemIdSetUnused(itemid);
		unused[uncnt++] = toff;
	}

	PageRepairFragmentation(page);

	MarkBufferDirty(buffer);

	/* XLOG stuff */
	if (!onerel->rd_istemp)
	{
		XLogRecPtr	recptr;

		recptr = log_heap_clean(onerel, buffer,
								NULL, 0, NULL, 0,
								unused, uncnt,
								false);
		PageSetLSN(page, recptr);
	}

	END_CRIT_SECTION();

	return tupindex;
}

/*
 *	lazy_vacuum_index() -- vacuum one index relation.
 *
 *		Delete all the index entries pointing to tuples listed in
 *		vacrelstats->dead_tuples, and update running statistics.
 */
static void
lazy_vacuum_index(Relation indrel,
				  IndexBulkDeleteResult **stats,
				  LVRelStats *vacrelstats)
{
	IndexVacuumInfo ivinfo;
	PGRUsage	ru0;

	pg_rusage_init(&ru0);

	ivinfo.index = indrel;
	ivinfo.vacuum_full = false;
	ivinfo.message_level = elevel;
	/* We don't yet know rel_tuples, so pass -1 */
	ivinfo.num_heap_tuples = -1;
	ivinfo.strategy = vac_strategy;

	/* Do bulk deletion */
	*stats = index_bulk_delete(&ivinfo, *stats,
							   lazy_tid_reaped, (void *) vacrelstats);

	ereport(elevel,
			(errmsg("scanned index \"%s\" to remove %d row versions",
					RelationGetRelationName(indrel),
					vacrelstats->num_dead_tuples),
			 errdetail("%s.", pg_rusage_show(&ru0))));
}

/*
 *	lazy_cleanup_index() -- do post-vacuum cleanup for one index relation.
 */
static void
lazy_cleanup_index(Relation indrel,
				   IndexBulkDeleteResult *stats,
				   LVRelStats *vacrelstats,
				   List *updated_stats)
{
	IndexVacuumInfo ivinfo;
	PGRUsage	ru0;

	pg_rusage_init(&ru0);

	ivinfo.index = indrel;
	ivinfo.vacuum_full = false;
	ivinfo.message_level = elevel;
	ivinfo.num_heap_tuples = vacrelstats->rel_tuples;
	ivinfo.strategy = vac_strategy;

	stats = index_vacuum_cleanup(&ivinfo, stats);

	if (!stats)
		return;

	/* now update statistics in pg_class */
	vac_update_relstats_from_list(indrel,
						stats->num_pages,
						stats->num_index_tuples,
						false,
						InvalidTransactionId,
						updated_stats);

	ereport(elevel,
			(errmsg("index \"%s\" now contains %.0f row versions in %u pages",
					RelationGetRelationName(indrel),
					stats->num_index_tuples,
					stats->num_pages),
			 errdetail("%.0f index row versions were removed.\n"
			 "%u index pages have been deleted, %u are currently reusable.\n"
					   "%s.",
					   stats->tuples_removed,
					   stats->pages_deleted, stats->pages_free,
					   pg_rusage_show(&ru0))));

	pfree(stats);
}

/*
 * lazy_truncate_heap - try to truncate off any empty pages at the end
 */
static void
lazy_truncate_heap(Relation onerel, LVRelStats *vacrelstats)
{
	BlockNumber old_rel_pages = vacrelstats->rel_pages;
	BlockNumber new_rel_pages;
	FSMPageData *pageSpaces;
	int			n;
	int			i,
				j;
	PGRUsage	ru0;

	/*
	 * Persistent table TIDs are stored in other locations like gp_relation_node
	 * and changeTracking logs, which continue to have references to CTID even
	 * if PT tuple is marked deleted. This TID is used to read tuple during
	 * crash recovery or segment resyncs. Hence need to avoid truncating
	 * persistent tables to avoid error / crash in heap_fetch using the TID
	 * on lazy vacuum.
	 */
	if (GpPersistent_IsPersistentRelation(RelationGetRelid(onerel)))
		return;

	pg_rusage_init(&ru0);

	/*
	 * We need full exclusive lock on the relation in order to do truncation.
	 * If we can't get it, give up rather than waiting --- we don't want to
	 * block other backends, and we don't want to deadlock (which is quite
	 * possible considering we already hold a lower-grade lock).
	 */
	if (!ConditionalLockRelation(onerel, AccessExclusiveLock))
		return;

	/*
	 * Now that we have exclusive lock, look to see if the rel has grown
	 * whilst we were vacuuming with non-exclusive lock.  If so, give up; the
	 * newly added pages presumably contain non-deletable tuples.
	 */
	new_rel_pages = RelationGetNumberOfBlocks(onerel);
	if (new_rel_pages != old_rel_pages)
	{
		/* might as well use the latest news when we update pg_class stats */
		vacrelstats->rel_pages = new_rel_pages;
		UnlockRelation(onerel, AccessExclusiveLock);
		return;
	}

	/*
	 * Scan backwards from the end to verify that the end pages actually
	 * contain no tuples.  This is *necessary*, not optional, because other
	 * backends could have added tuples to these pages whilst we were
	 * vacuuming.
	 */
	new_rel_pages = count_nondeletable_pages(onerel, vacrelstats);

	if (new_rel_pages >= old_rel_pages)
	{
		/* can't do anything after all */
		UnlockRelation(onerel, AccessExclusiveLock);
		return;
	}

	/*
	 * Okay to truncate.
	 */
	RelationTruncate(
				onerel,
				new_rel_pages,
				/* markPersistentAsPhysicallyTruncated */ true);

	/*
	 * Note: once we have truncated, we *must* keep the exclusive lock until
	 * commit.	The sinval message that will be sent at commit (as a result of
	 * vac_update_relstats()) must be received by other backends, to cause
	 * them to reset their rd_targblock values, before they can safely access
	 * the table again.
	 */

	/*
	 * Drop free-space info for removed blocks; these must not get entered
	 * into the FSM!
	 */
	pageSpaces = vacrelstats->free_pages;
	n = vacrelstats->num_free_pages;
	j = 0;
	for (i = 0; i < n; i++)
	{
		if (FSMPageGetPageNum(&pageSpaces[i]) < new_rel_pages)
		{
			pageSpaces[j] = pageSpaces[i];
			j++;
		}
	}
	vacrelstats->num_free_pages = j;

	/*
	 * If tot_free_pages was more than num_free_pages, we can't tell for sure
	 * what its correct value is now, because we don't know which of the
	 * forgotten pages are getting truncated.  Conservatively set it equal to
	 * num_free_pages.
	 */
	vacrelstats->tot_free_pages = j;

	/* We destroyed the heap ordering, so mark array unordered */
	vacrelstats->fs_is_heap = false;

	/* update statistics */
	vacrelstats->rel_pages = new_rel_pages;
	vacrelstats->pages_removed = old_rel_pages - new_rel_pages;

	ereport(elevel,
			(errmsg("\"%s\": truncated %u to %u pages",
					RelationGetRelationName(onerel),
					old_rel_pages, new_rel_pages),
			 errdetail("%s.",
					   pg_rusage_show(&ru0))));
}


/*
 * Fills in the relation statistics for an append-only relation.
 *
 *	This information is used to update the reltuples and relpages information
 *	in pg_class. reltuples is the same as "pg_aoseg_<oid>:tupcount"
 *	column and we simulate relpages by subdividing the eof value
 *	("pg_aoseg_<oid>:eof") over the defined page size.
 */
void
vacuum_appendonly_fill_stats(Relation aorel, Snapshot snapshot,
							 BlockNumber *rel_pages, double *rel_tuples,
							 bool *relhasindex)
{
	FileSegTotals *fstotal;
	BlockNumber nblocks;
	char	   *relname;
	double		num_tuples;
	double		totalbytes;
	double		eof;
	int64       hidden_tupcount;
	AppendOnlyVisimap visimap;

	Assert(RelationIsAoRows(aorel) || RelationIsAoCols(aorel));

	relname = RelationGetRelationName(aorel);

	/* get updated statistics from the pg_aoseg table */
	if (RelationIsAoRows(aorel))
	{
		fstotal = GetSegFilesTotals(aorel, snapshot);
	}
	else
	{
		Assert(RelationIsAoCols(aorel));
		fstotal = GetAOCSSSegFilesTotals(aorel, snapshot);
	}

	/* calculate the values we care about */
	eof = (double)fstotal->totalbytes;
	num_tuples = (double)fstotal->totaltuples;
	totalbytes = eof;
	nblocks = (uint32)RelationGuessNumberOfBlocks(totalbytes);

	AppendOnlyVisimap_Init(&visimap,
						   aorel->rd_appendonly->visimaprelid,
						   aorel->rd_appendonly->visimapidxid,
						   AccessShareLock,
						   snapshot);
	hidden_tupcount = AppendOnlyVisimap_GetRelationHiddenTupleCount(&visimap);
	num_tuples -= hidden_tupcount;
	Assert(num_tuples > -1.0);
	AppendOnlyVisimap_Finish(&visimap, AccessShareLock);

	elogif (Debug_appendonly_print_compaction, LOG,
			"Gather statistics after vacuum for append-only relation %s: "
			"page count %d, tuple count %f",
			relname,
			nblocks, num_tuples);

	*rel_pages = nblocks;
	*rel_tuples = num_tuples;
	*relhasindex = aorel->rd_rel->relhasindex;

	ereport(elevel,
			(errmsg("\"%s\": found %.0f rows in %u pages.",
					relname, num_tuples, nblocks)));
	pfree(fstotal);
}

/*
 *	vacuum_appendonly_rel() -- vaccum an append-only relation
 *
 *		This procedure will be what gets executed both for VACUUM
 *		and VACUUM FULL (and also ANALYZE or any other thing that
 *		needs the pg_class stats updated).
 *
 *		The function can compact append-only segment files or just
 *		truncating the segment file to its existing eof.
 *
 *		Afterwards, the reltuples and relpages information in pg_class
 *		are updated. reltuples is the same as "pg_aoseg_<oid>:tupcount"
 *		column and we simulate relpages by subdividing the eof value
 *		("pg_aoseg_<oid>:eof") over the defined page size.
 *
 *
 *		There are txn ids, hint bits, free space, dead tuples,
 *		etc. these are all irrelevant in the append only relation context.
 *
 */
void
vacuum_appendonly_rel(Relation aorel, VacuumStmt *vacstmt)
{
	char	   *relname;
	PGRUsage	ru0;

	Assert(RelationIsAoRows(aorel) || RelationIsAoCols(aorel));
	Assert(!vacummStatement_IsInAppendOnlyCleanupPhase(vacstmt));

	pg_rusage_init(&ru0);
	relname = RelationGetRelationName(aorel);
	ereport(elevel,
			(errmsg("vacuuming \"%s.%s\"",
					get_namespace_name(RelationGetNamespace(aorel)),
					relname)));

	if (Gp_role == GP_ROLE_DISPATCH)
	{
		return;
	}
	Assert(list_length(vacstmt->appendonly_compaction_insert_segno) <= 1);
	if (vacstmt->appendonly_compaction_insert_segno == NULL)
	{
		elogif(Debug_appendonly_print_compaction, LOG,
			"Vacuum drop phase %s", RelationGetRelationName(aorel));

		if (RelationIsAoRows(aorel))
		{
			AppendOnlyDrop(aorel, vacstmt->appendonly_compaction_segno);
		}
		else
		{
			Assert(RelationIsAoCols(aorel));
			AOCSDrop(aorel, vacstmt->appendonly_compaction_segno);
		}
	}
	else
	{
		int insert_segno = linitial_int(vacstmt->appendonly_compaction_insert_segno);
		if (insert_segno == APPENDONLY_COMPACTION_SEGNO_INVALID)
		{
			elogif(Debug_appendonly_print_compaction, LOG,
			"Vacuum pseudo-compaction phase %s", RelationGetRelationName(aorel));
		}
		else
		{
			elogif(Debug_appendonly_print_compaction, LOG,
				"Vacuum compaction phase %s", RelationGetRelationName(aorel));
			if (RelationIsAoRows(aorel))
			{
				AppendOnlyCompact(aorel,
					vacstmt->appendonly_compaction_segno,
					insert_segno, vacstmt->full);
			}
			else
			{
				Assert(RelationIsAoCols(aorel));
				AOCSCompact(aorel,
					vacstmt->appendonly_compaction_segno,
					insert_segno, vacstmt->full);
			}
		}
	}
}

/*
 * Rescan end pages to verify that they are (still) empty of tuples.
 *
 * Returns number of nondeletable pages (last nonempty page + 1).
 */
static BlockNumber
count_nondeletable_pages(Relation onerel, LVRelStats *vacrelstats)
{
	MIRROREDLOCK_BUFMGR_DECLARE;

	BlockNumber blkno;

	/* Strange coding of loop control is needed because blkno is unsigned */
	blkno = vacrelstats->rel_pages;
	while (blkno > vacrelstats->nonempty_pages)
	{
		Buffer		buf;
		Page		page;
		OffsetNumber offnum,
					maxoff;
		bool		hastup;

		/*
		 * We don't insert a vacuum delay point here, because we have an
		 * exclusive lock on the table which we want to hold for as short a
		 * time as possible.  We still need to check for interrupts however.
		 */
		CHECK_FOR_INTERRUPTS();

		blkno--;

		/* -------- MirroredLock ---------- */
		MIRROREDLOCK_BUFMGR_LOCK;

		buf = ReadBufferWithStrategy(onerel, blkno, vac_strategy);

		/* In this phase we only need shared access to the buffer */
		LockBuffer(buf, BUFFER_LOCK_SHARE);

		page = BufferGetPage(buf);

		if (PageIsNew(page) || PageIsEmpty(page))
		{
			/* PageIsNew probably shouldn't happen... */
			UnlockReleaseBuffer(buf);

			MIRROREDLOCK_BUFMGR_UNLOCK;
			/* -------- MirroredLock ---------- */

			continue;
		}

		hastup = false;
		maxoff = PageGetMaxOffsetNumber(page);
		for (offnum = FirstOffsetNumber;
			 offnum <= maxoff;
			 offnum = OffsetNumberNext(offnum))
		{
			ItemId		itemid;

			itemid = PageGetItemId(page, offnum);

			/*
			 * Note: any non-unused item should be taken as a reason to keep
			 * this page.  We formerly thought that DEAD tuples could be
			 * thrown away, but that's not so, because we'd not have cleaned
			 * out their index entries.
			 */
			if (ItemIdIsUsed(itemid))
			{
				hastup = true;
				break;			/* can stop scanning */
			}
		}						/* scan along page */

		UnlockReleaseBuffer(buf);

		MIRROREDLOCK_BUFMGR_UNLOCK;
		/* -------- MirroredLock ---------- */

		/* Done scanning if we found a tuple here */
		if (hastup)
			return blkno + 1;
	}

	/*
	 * If we fall out of the loop, all the previously-thought-to-be-empty
	 * pages still are; we need not bother to look at the last known-nonempty
	 * page.
	 */
	return vacrelstats->nonempty_pages;
}

/*
 * lazy_space_alloc - space allocation decisions for lazy vacuum
 *
 * See the comments at the head of this file for rationale.
 */
static void
lazy_space_alloc(LVRelStats *vacrelstats, BlockNumber relblocks)
{
	long		maxtuples;
	int			maxpages;

	if (vacrelstats->hasindex)
	{
		maxtuples = (maintenance_work_mem * 1024L) / sizeof(ItemPointerData);
		maxtuples = Min(maxtuples, INT_MAX);
		maxtuples = Min(maxtuples, MaxAllocSize / sizeof(ItemPointerData));

		/* curious coding here to ensure the multiplication can't overflow */
		if ((BlockNumber) (maxtuples / LAZY_ALLOC_TUPLES) > relblocks)
			maxtuples = relblocks * LAZY_ALLOC_TUPLES;

		/* stay sane if small maintenance_work_mem */
		maxtuples = Max(maxtuples, MaxHeapTuplesPerPage);
	}
	else
	{
		maxtuples = MaxHeapTuplesPerPage;
	}

	vacrelstats->num_dead_tuples = 0;
	vacrelstats->max_dead_tuples = (int) maxtuples;
	vacrelstats->dead_tuples = (ItemPointer)
		palloc(maxtuples * sizeof(ItemPointerData));

	maxpages = MaxFSMPages;
	maxpages = Min(maxpages, MaxAllocSize / sizeof(FSMPageData));
	/* No need to allocate more pages than the relation has blocks */
	if (relblocks < (BlockNumber) maxpages)
		maxpages = (int) relblocks;

	vacrelstats->fs_is_heap = false;
	vacrelstats->num_free_pages = 0;
	vacrelstats->max_free_pages = maxpages;
	vacrelstats->free_pages = (FSMPageData *)
		palloc(maxpages * sizeof(FSMPageData));
	vacrelstats->tot_free_pages = 0;
}

/*
 * lazy_record_dead_tuple - remember one deletable tuple
 */
static void
lazy_record_dead_tuple(LVRelStats *vacrelstats,
					   ItemPointer itemptr)
{
	/*
	 * The array shouldn't overflow under normal behavior, but perhaps it
	 * could if we are given a really small maintenance_work_mem. In that
	 * case, just forget the last few tuples (we'll get 'em next time).
	 */
	if (vacrelstats->num_dead_tuples < vacrelstats->max_dead_tuples)
	{
		vacrelstats->dead_tuples[vacrelstats->num_dead_tuples] = *itemptr;
		vacrelstats->num_dead_tuples++;
	}
}

/*
 * lazy_record_free_space - remember free space on one page
 */
static void
lazy_record_free_space(LVRelStats *vacrelstats,
					   BlockNumber page,
					   Size avail)
{
	FSMPageData *pageSpaces;
	int			n;

	/*
	 * A page with less than stats->threshold free space will be forgotten
	 * immediately, and never passed to the free space map.  Removing the
	 * uselessly small entries early saves cycles, and in particular reduces
	 * the amount of time we spend holding the FSM lock when we finally call
	 * RecordRelationFreeSpace.  Since the FSM will probably drop pages with
	 * little free space anyway, there's no point in making this really small.
	 *
	 * XXX Is it worth trying to measure average tuple size, and using that to
	 * adjust the threshold?  Would be worthwhile if FSM has no stats yet for
	 * this relation.  But changing the threshold as we scan the rel might
	 * lead to bizarre behavior, too.  Also, it's probably better if vacuum.c
	 * has the same thresholding behavior as we do here.
	 */
	if (avail < vacrelstats->threshold)
		return;

	/* Count all pages over threshold, even if not enough space in array */
	vacrelstats->tot_free_pages++;

	/* Copy pointers to local variables for notational simplicity */
	pageSpaces = vacrelstats->free_pages;
	n = vacrelstats->max_free_pages;

	/* If we haven't filled the array yet, just keep adding entries */
	if (vacrelstats->num_free_pages < n)
	{
		FSMPageSetPageNum(&pageSpaces[vacrelstats->num_free_pages], page);
		FSMPageSetSpace(&pageSpaces[vacrelstats->num_free_pages], avail);
		vacrelstats->num_free_pages++;
		return;
	}

	/*----------
	 * The rest of this routine works with "heap" organization of the
	 * free space arrays, wherein we maintain the heap property
	 *			avail[(j-1) div 2] <= avail[j]	for 0 < j < n.
	 * In particular, the zero'th element always has the smallest available
	 * space and can be discarded to make room for a new page with more space.
	 * See Knuth's discussion of heap-based priority queues, sec 5.2.3;
	 * but note he uses 1-origin array subscripts, not 0-origin.
	 *----------
	 */

	/* If we haven't yet converted the array to heap organization, do it */
	if (!vacrelstats->fs_is_heap)
	{
		/*
		 * Scan backwards through the array, "sift-up" each value into its
		 * correct position.  We can start the scan at n/2-1 since each entry
		 * above that position has no children to worry about.
		 */
		int			l = n / 2;

		while (--l >= 0)
		{
			BlockNumber R = FSMPageGetPageNum(&pageSpaces[l]);
			Size		K = FSMPageGetSpace(&pageSpaces[l]);
			int			i;		/* i is where the "hole" is */

			i = l;
			for (;;)
			{
				int			j = 2 * i + 1;

				if (j >= n)
					break;
				if (j + 1 < n && FSMPageGetSpace(&pageSpaces[j]) > FSMPageGetSpace(&pageSpaces[j + 1]))
					j++;
				if (K <= FSMPageGetSpace(&pageSpaces[j]))
					break;
				pageSpaces[i] = pageSpaces[j];
				i = j;
			}
			FSMPageSetPageNum(&pageSpaces[i], R);
			FSMPageSetSpace(&pageSpaces[i], K);
		}

		vacrelstats->fs_is_heap = true;
	}

	/* If new page has more than zero'th entry, insert it into heap */
	if (avail > FSMPageGetSpace(&pageSpaces[0]))
	{
		/*
		 * Notionally, we replace the zero'th entry with the new data, and
		 * then sift-up to maintain the heap property.	Physically, the new
		 * data doesn't get stored into the arrays until we find the right
		 * location for it.
		 */
		int			i = 0;		/* i is where the "hole" is */

		for (;;)
		{
			int			j = 2 * i + 1;

			if (j >= n)
				break;
			if (j + 1 < n && FSMPageGetSpace(&pageSpaces[j]) > FSMPageGetSpace(&pageSpaces[j + 1]))
				j++;
			if (avail <= FSMPageGetSpace(&pageSpaces[j]))
				break;
			pageSpaces[i] = pageSpaces[j];
			i = j;
		}
		FSMPageSetPageNum(&pageSpaces[i], page);
		FSMPageSetSpace(&pageSpaces[i], avail);
	}
}

/*
 *	lazy_tid_reaped() -- is a particular tid deletable?
 *
 *		This has the right signature to be an IndexBulkDeleteCallback.
 *
 *		Assumes dead_tuples array is in sorted order.
 */
static bool
lazy_tid_reaped(ItemPointer itemptr, void *state)
{
	LVRelStats *vacrelstats = (LVRelStats *) state;
	ItemPointer res;

	res = (ItemPointer) bsearch((void *) itemptr,
								(void *) vacrelstats->dead_tuples,
								vacrelstats->num_dead_tuples,
								sizeof(ItemPointerData),
								vac_cmp_itemptr);

	return (res != NULL);
}

/*
 * Update the shared Free Space Map with the info we now have about
 * free space in the relation, discarding any old info the map may have.
 */
static void
lazy_update_fsm(Relation onerel, LVRelStats *vacrelstats)
{
	FSMPageData *pageSpaces = vacrelstats->free_pages;
	int			nPages = vacrelstats->num_free_pages;

	/*
	 * Sort data into order, as required by RecordRelationFreeSpace.
	 */
	if (nPages > 1)
		qsort(pageSpaces, nPages, sizeof(FSMPageData),
			  vac_cmp_page_spaces);

	RecordRelationFreeSpace(&onerel->rd_node, vacrelstats->tot_free_pages,
							nPages, pageSpaces);
}

/*
 * Comparator routines for use with qsort() and bsearch().
 */
static int
vac_cmp_itemptr(const void *left, const void *right)
{
	BlockNumber lblk,
				rblk;
	OffsetNumber loff,
				roff;

	lblk = ItemPointerGetBlockNumber((ItemPointer) left);
	rblk = ItemPointerGetBlockNumber((ItemPointer) right);

	if (lblk < rblk)
		return -1;
	if (lblk > rblk)
		return 1;

	loff = ItemPointerGetOffsetNumber((ItemPointer) left);
	roff = ItemPointerGetOffsetNumber((ItemPointer) right);

	if (loff < roff)
		return -1;
	if (loff > roff)
		return 1;

	return 0;
}

static int
vac_cmp_page_spaces(const void *left, const void *right)
{
	FSMPageData *linfo = (FSMPageData *) left;
	FSMPageData *rinfo = (FSMPageData *) right;
	BlockNumber	lblkno = FSMPageGetPageNum(linfo);
	BlockNumber	rblkno = FSMPageGetPageNum(rinfo);

	if (lblkno < rblkno)
		return -1;
	else if (lblkno > rblkno)
		return 1;
	return 0;
}
