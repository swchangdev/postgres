/*-------------------------------------------------------------------------
 *
 * dsm_registry.c
 *	  Functions for interfacing with the dynamic shared memory registry.
 *
 * This provides a way for libraries to use shared memory without needing
 * to request it at startup time via a shmem_request_hook.  The registry
 * stores dynamic shared memory (DSM) segment handles keyed by a
 * library-specified string.
 *
 * The registry is accessed by calling GetNamedDSMSegment().  If a segment
 * with the provided name does not yet exist, it is created and initialized
 * with the provided init_callback callback function.  Otherwise,
 * GetNamedDSMSegment() simply ensures that the segment is attached to the
 * current backend.  This function guarantees that only one backend
 * initializes the segment and that all other backends just attach it.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/ipc/dsm_registry.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "lib/dshash.h"
#include "storage/dsm_registry.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "utils/memutils.h"

typedef struct DSMRegistryCtxStruct
{
	dsa_handle	dsah;
	dshash_table_handle dshh;
} DSMRegistryCtxStruct;

static DSMRegistryCtxStruct *DSMRegistryCtx;

typedef struct DSMRegistryEntry
{
	char		name[64];
	dsm_handle	handle;
	size_t		size;
} DSMRegistryEntry;

typedef struct DSMDetachCallbackContext
{
	void (*on_detach_callback) (void *);
	void *arg;
} DSMDetachCallbackContext;

static const dshash_parameters dsh_params = {
	offsetof(DSMRegistryEntry, handle),
	sizeof(DSMRegistryEntry),
	dshash_strcmp,
	dshash_strhash,
	dshash_strcpy,
	LWTRANCHE_DSM_REGISTRY_HASH
};

static dsa_area *dsm_registry_dsa;
static dshash_table *dsm_registry_table;

Size
DSMRegistryShmemSize(void)
{
	return MAXALIGN(sizeof(DSMRegistryCtxStruct));
}

void
DSMRegistryShmemInit(void)
{
	bool		found;

	DSMRegistryCtx = (DSMRegistryCtxStruct *)
		ShmemInitStruct("DSM Registry Data",
						DSMRegistryShmemSize(),
						&found);

	if (!found)
	{
		DSMRegistryCtx->dsah = DSA_HANDLE_INVALID;
		DSMRegistryCtx->dshh = DSHASH_HANDLE_INVALID;
	}
}

/*
 * Initialize or attach to the dynamic shared hash table that stores the DSM
 * registry entries, if not already done.  This must be called before accessing
 * the table.
 */
static void
init_dsm_registry(void)
{
	/* Quick exit if we already did this. */
	if (dsm_registry_table)
		return;

	/* Otherwise, use a lock to ensure only one process creates the table. */
	LWLockAcquire(DSMRegistryLock, LW_EXCLUSIVE);

	if (DSMRegistryCtx->dshh == DSHASH_HANDLE_INVALID)
	{
		/* Initialize dynamic shared hash table for registry. */
		dsm_registry_dsa = dsa_create(LWTRANCHE_DSM_REGISTRY_DSA);
		dsa_pin(dsm_registry_dsa);
		dsa_pin_mapping(dsm_registry_dsa);
		dsm_registry_table = dshash_create(dsm_registry_dsa, &dsh_params, NULL);

		/* Store handles in shared memory for other backends to use. */
		DSMRegistryCtx->dsah = dsa_get_handle(dsm_registry_dsa);
		DSMRegistryCtx->dshh = dshash_get_hash_table_handle(dsm_registry_table);
	}
	else
	{
		/* Attach to existing dynamic shared hash table. */
		dsm_registry_dsa = dsa_attach(DSMRegistryCtx->dsah);
		dsa_pin_mapping(dsm_registry_dsa);
		dsm_registry_table = dshash_attach(dsm_registry_dsa, &dsh_params,
										   DSMRegistryCtx->dshh, NULL);
	}

	LWLockRelease(DSMRegistryLock);
}

static void
call_on_detach_callback(dsm_segment *seg, Datum arg)
{
	char *ptr = DatumGetPointer(arg);
	DSMDetachCallbackContext *cb_ctx = (DSMDetachCallbackContext *)ptr;
	cb_ctx->on_detach_callback(cb_ctx->arg);
}

static void
detach_dsm_segment(dsm_handle handle, void (*detach_cb) (void *), void *arg)
{
	dsm_segment *seg = dsm_find_mapping(handle);

	/* Detach the segment only if it is already attached */
	if (seg != NULL)
	{
		if (detach_cb != NULL)
		{
			DSMDetachCallbackContext *cb_ctx = palloc(sizeof(DSMDetachCallbackContext));
			cb_ctx->on_detach_callback = detach_cb;
			cb_ctx->arg = arg;

			on_dsm_detach(seg, call_on_detach_callback, PointerGetDatum(cb_ctx));
		}

		dsm_unpin_mapping(seg);
		dsm_detach(seg);
	}
}

/*
 * Initialize or attach a named DSM segment.
 *
 * This routine returns the address of the segment.  init_callback is called to
 * initialize the segment when it is first created.
 */
void *
GetNamedDSMSegment(const char *name, size_t size,
				   void (*init_callback) (void *ptr), bool *found)
{
	DSMRegistryEntry *entry;
	MemoryContext oldcontext;
	void	   *ret;

	Assert(found);

	if (!name || *name == '\0')
		ereport(ERROR,
				(errmsg("DSM segment name cannot be empty")));

	if (strlen(name) >= offsetof(DSMRegistryEntry, handle))
		ereport(ERROR,
				(errmsg("DSM segment name too long")));

	if (size == 0)
		ereport(ERROR,
				(errmsg("DSM segment size must be nonzero")));

	/* Be sure any local memory allocated by DSM/DSA routines is persistent. */
	oldcontext = MemoryContextSwitchTo(TopMemoryContext);

	/* Connect to the registry. */
	init_dsm_registry();

	entry = dshash_find_or_insert(dsm_registry_table, name, found);
	if (!(*found))
	{
		/* Initialize the segment. */
		dsm_segment *seg = dsm_create(size, 0);

		dsm_pin_segment(seg);
		dsm_pin_mapping(seg);
		entry->handle = dsm_segment_handle(seg);
		entry->size = size;
		ret = dsm_segment_address(seg);

		if (init_callback)
			(*init_callback) (ret);
	}
	else if (entry->size != size)
	{
		dshash_release_lock(dsm_registry_table, entry);
		MemoryContextSwitchTo(oldcontext);
		ereport(ERROR,
				(errmsg("requested DSM segment size does not match size of "
						"existing segment")));
	}
	else
	{
		dsm_segment *seg = dsm_find_mapping(entry->handle);

		/* If the existing segment is not already attached, attach it now. */
		if (seg == NULL)
		{
			seg = dsm_attach(entry->handle);
			if (seg == NULL)
			{
				dshash_release_lock(dsm_registry_table, entry);
				MemoryContextSwitchTo(oldcontext);
				elog(ERROR, "could not map dynamic shared memory segment");
			}
			dsm_pin_mapping(seg);
		}

		ret = dsm_segment_address(seg);
	}

	dshash_release_lock(dsm_registry_table, entry);
	MemoryContextSwitchTo(oldcontext);

	return ret;
}

/*
 * Detach a named DSM segment
 *
 * This routine detaches the DSM segment. If the DSM segment was not attached
 * by this process, then the routine just returns. on_detach_callback is passed
 * on to dsm_segment by calling on_dsm_detach for the corresponding dsm_segment
 * before actually detaching.
 */
void
DetachNamedDSMSegment(const char *name, size_t size,
					  void (*on_detach_callback) (void *), void *arg)
{
	DSMRegistryEntry *entry;
	MemoryContext oldcontext;

	if (!name || *name == '\0')
		ereport(ERROR,
				(errmsg("DSM segment name cannot be empty")));

	if (strlen(name) >= offsetof(DSMRegistryEntry, handle))
		ereport(ERROR,
				(errmsg("DSM segment name too long")));

	if (size == 0)
		ereport(ERROR,
				(errmsg("DSM segment size must be nonzero")));

	/* Be sure any local memory allocated by DSM/DSA routines is persistent. */
	oldcontext = MemoryContextSwitchTo(TopMemoryContext);

	/* Connect to the registry. */
	init_dsm_registry();

	entry = dshash_find(dsm_registry_table, name, true);

	if (entry == NULL)
	{
		MemoryContextSwitchTo(oldcontext);
		ereport(ERROR,
				(errmsg("cannot detach a DSM segment that does not exist")));
	}

	if (entry->size != size)
	{
		dshash_release_lock(dsm_registry_table, entry);
		MemoryContextSwitchTo(oldcontext);
		ereport(ERROR,
				(errmsg("requested DSM segment size does not match size of "
						"existing segment")));
	}

	detach_dsm_segment(entry->handle, on_detach_callback, arg);

	dshash_release_lock(dsm_registry_table, entry);
	MemoryContextSwitchTo(oldcontext);
}

/*
 * Attempt to destroy a named DSM segment
 *
 * This routine attempts to destroy the DSM segment. We unpin the dsm_segment
 * and delete the entry from dsm_registry_table. This may not destroy the
 * dsm_segment instantly, but it would die out once all the other processes
 * attached to this dsm_segment either exit or manually detach from the
 * dsm_segment.
 *
 * Because we deleted the key from dsm_registry_table, calling
 * GetNamedDSMSegment with the same key would result into creating a new
 * dsm_segment instead of retrieving the old unpinned dsm_segment.
 */
void
DestroyNamedDSMSegment(const char *name, size_t size,
					   void (*on_detach_callback) (void *), void *arg)
{
	DSMRegistryEntry *entry;
	MemoryContext oldcontext;

	if (!name || *name == '\0')
		ereport(ERROR,
				(errmsg("DSM segment name cannot be empty")));

	if (strlen(name) >= offsetof(DSMRegistryEntry, handle))
		ereport(ERROR,
				(errmsg("DSM segment name too long")));

	if (size == 0)
		ereport(ERROR,
				(errmsg("DSM segment size must be nonzero")));

	/* Be sure any local memory allocated by DSM/DSA routines is persistent. */
	oldcontext = MemoryContextSwitchTo(TopMemoryContext);

	/* Connect to the registry. */
	init_dsm_registry();

	entry = dshash_find(dsm_registry_table, name, true);

	if (entry == NULL)
	{
		MemoryContextSwitchTo(oldcontext);
		ereport(ERROR,
				(errmsg("cannot destroy a DSM segment that does not exist")));
	}

	if (entry->size != size)
	{
		dshash_release_lock(dsm_registry_table, entry);
		MemoryContextSwitchTo(oldcontext);
		ereport(ERROR,
				(errmsg("requested DSM segment size does not match size of "
						"existing segment")));
	}

	detach_dsm_segment(entry->handle, on_detach_callback, arg);
	dsm_unpin_segment(entry->handle);

	/* dshash_delete_entry calls LWLockRelease internally. We shouldn't
	 * release lock twice */
	dshash_delete_entry(dsm_registry_table, entry);
	dshash_delete_key(dsm_registry_table, name);

	MemoryContextSwitchTo(oldcontext);
}