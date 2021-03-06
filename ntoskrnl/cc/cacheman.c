/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS kernel
 * FILE:            ntoskrnl/cc/cacheman.c
 * PURPOSE:         Cache manager
 *
 * PROGRAMMERS:     David Welch (welch@cwcom.net)
 *                  Pierre Schweitzer (pierre@reactos.org)
 */

/* INCLUDES *****************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

BOOLEAN CcPfEnablePrefetcher;
PFSN_PREFETCHER_GLOBALS CcPfGlobals;

/* FUNCTIONS *****************************************************************/

VOID
NTAPI
INIT_FUNCTION
CcPfInitializePrefetcher(VOID)
{
    /* Notify debugger */
    DbgPrintEx(DPFLTR_PREFETCHER_ID,
               DPFLTR_TRACE_LEVEL,
               "CCPF: InitializePrefetecher()\n");

    /* Setup the Prefetcher Data */
    InitializeListHead(&CcPfGlobals.ActiveTraces);
    InitializeListHead(&CcPfGlobals.CompletedTraces);
    ExInitializeFastMutex(&CcPfGlobals.CompletedTracesLock);

    /* FIXME: Setup the rest of the prefetecher */
}

BOOLEAN
NTAPI
INIT_FUNCTION
CcInitializeCacheManager(VOID)
{
    return CcInitView();
}

VOID
NTAPI
CcShutdownSystem(VOID)
{
    /* Inform the lazy writer it has to stop activity */
    CcShutdownLazyWriter();
}

/*
 * @unimplemented
 */
LARGE_INTEGER
NTAPI
CcGetFlushedValidData (
    IN PSECTION_OBJECT_POINTERS SectionObjectPointer,
    IN BOOLEAN BcbListHeld
    )
{
	LARGE_INTEGER i;

	UNIMPLEMENTED;

	i.QuadPart = 0;
	return i;
}

/*
 * @unimplemented
 */
PVOID
NTAPI
CcRemapBcb (
    IN PVOID Bcb
    )
{
	UNIMPLEMENTED;

    return 0;
}

/*
 * @unimplemented
 */
VOID
NTAPI
CcScheduleReadAhead (
	IN	PFILE_OBJECT		FileObject,
	IN	PLARGE_INTEGER		FileOffset,
	IN	ULONG			Length
	)
{
	UNIMPLEMENTED;
}

/*
 * @unimplemented
 */
VOID
NTAPI
CcSetAdditionalCacheAttributes (
	IN	PFILE_OBJECT	FileObject,
	IN	BOOLEAN		DisableReadAhead,
	IN	BOOLEAN		DisableWriteBehind
	)
{
    CCTRACE(CC_API_DEBUG, "FileObject=%p DisableReadAhead=%d DisableWriteBehind=%d\n",
        FileObject, DisableReadAhead, DisableWriteBehind);

	UNIMPLEMENTED;
}

/*
 * @unimplemented
 */
VOID
NTAPI
CcSetBcbOwnerPointer (
	IN	PVOID	Bcb,
	IN	PVOID	Owner
	)
{
    PINTERNAL_BCB iBcb = Bcb;

    CCTRACE(CC_API_DEBUG, "Bcb=%p Owner=%p\n",
        Bcb, Owner);

    if (!ExIsResourceAcquiredExclusiveLite(&iBcb->Lock) && !ExIsResourceAcquiredSharedLite(&iBcb->Lock))
    {
        DPRINT1("Current thread doesn't own resource!\n");
        return;
    }

    ExSetResourceOwnerPointer(&iBcb->Lock, Owner);
}

/*
 * @implemented
 */
VOID
NTAPI
CcSetDirtyPageThreshold (
	IN	PFILE_OBJECT	FileObject,
	IN	ULONG		DirtyPageThreshold
	)
{
    PFSRTL_COMMON_FCB_HEADER Fcb;
    PROS_SHARED_CACHE_MAP SharedCacheMap;

    CCTRACE(CC_API_DEBUG, "FileObject=%p DirtyPageThreshold=%lu\n",
        FileObject, DirtyPageThreshold);

    SharedCacheMap = FileObject->SectionObjectPointer->SharedCacheMap;
    if (SharedCacheMap != NULL)
    {
        SharedCacheMap->DirtyPageThreshold = DirtyPageThreshold;
    }

    Fcb = FileObject->FsContext;
    if (!BooleanFlagOn(Fcb->Flags, FSRTL_FLAG_LIMIT_MODIFIED_PAGES))
    {
        SetFlag(Fcb->Flags, FSRTL_FLAG_LIMIT_MODIFIED_PAGES);
    }
}

/*
 * @unimplemented
 */
VOID
NTAPI
CcSetReadAheadGranularity (
	IN	PFILE_OBJECT	FileObject,
	IN	ULONG		Granularity
	)
{
    static ULONG Warn;

    CCTRACE(CC_API_DEBUG, "FileObject=%p Granularity=%lu\n",
        FileObject, Granularity);

    if (!Warn++) UNIMPLEMENTED;
}
