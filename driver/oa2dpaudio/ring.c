/* SPDX-License-Identifier: MIT */
/*
 * oa2dpaudio.sys — shared PCM ring (producer side) and its user-mode
 * mapping for oa2dpsvc.exe. The WaveRT stream calls Oa2dpRingPump() from
 * its position timer with the frames Windows' audio engine wrote since the
 * last call (see minwavertstream.cpp).
 */
#include <ntifs.h>

#include "oa2dpaudio.h"

/* ~340 ms at 96 kHz; plenty for encoder + scheduling jitter. */
#define RING_FRAMES 32768

NTSTATUS Oa2dpRingCreate(_Inout_ OA2DP_AUDIO_RING *r, _In_ ULONG rate, _In_ USHORT channels,
                         _In_ USHORT format)
{
    PHYSICAL_ADDRESS low, high, skip;
    /* Sized for the largest channel count so the allocation never changes. */
    SIZE_T bytes = oa2dp_ring_bytes(RING_FRAMES, OA2DP_RING_MAX_CHANNELS);
    PMDL mdl;
    oa2dp_pcm_ring *ring;

    if (channels == 0 || channels > OA2DP_RING_MAX_CHANNELS)
        return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(r, sizeof(*r));
    KeInitializeSpinLock(&r->Lock);
    low.QuadPart = 0;
    high.QuadPart = (LONGLONG)-1;
    skip.QuadPart = 0;
    /* Non-paged, page-aligned memory we can map into the service later. */
    mdl = MmAllocatePagesForMdlEx(low, high, skip, bytes, MmCached,
                                  MM_ALLOCATE_FULLY_REQUIRED | MM_DONT_ZERO_ALLOCATION);
    if (!mdl)
        return STATUS_INSUFFICIENT_RESOURCES;
    if (MmGetMdlByteCount(mdl) < bytes) {
        MmFreePagesFromMdl(mdl);
        ExFreePool(mdl);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    ring = (oa2dp_pcm_ring *)MmGetSystemAddressForMdlSafe(mdl, NormalPagePriority | MdlMappingNoExecute);
    if (!ring) {
        MmFreePagesFromMdl(mdl);
        ExFreePool(mdl);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(ring, bytes);
    if (oa2dp_ring_init(ring, RING_FRAMES, rate, channels, format) != OA2DP_OK) {
        MmFreePagesFromMdl(mdl);
        ExFreePool(mdl);
        return STATUS_INVALID_PARAMETER;
    }
    r->Mdl = mdl;
    r->Ring = ring;
    r->Bytes = bytes;
    return STATUS_SUCCESS;
}

/* PASSIVE_LEVEL. Unmaps the service's view first (from any process). */
VOID Oa2dpRingDestroy(_Inout_ OA2DP_AUDIO_RING *r)
{
    KIRQL irql;
    PMDL mdl;

    Oa2dpRingUnmapUser(r);
    KeAcquireSpinLock(&r->Lock, &irql);
    mdl = r->Mdl;
    r->Ring = NULL; /* a late pump sees NULL and does nothing */
    r->Mdl = NULL;
    r->Bytes = 0;
    KeReleaseSpinLock(&r->Lock, irql);
    if (mdl) {
        MmFreePagesFromMdl(mdl);
        ExFreePool(mdl);
    }
}

/* Must run in the context of the service process: the IOCTL arrives at
 * PASSIVE_LEVEL in the caller's thread (we are the top of the stack). */
NTSTATUS Oa2dpRingMapUser(_Inout_ OA2DP_AUDIO_RING *r, _Out_ OA2DP_RING_MAPPING *out)
{
    PVOID va = NULL;

    RtlZeroMemory(out, sizeof(*out));
    if (!r->Mdl)
        return STATUS_DEVICE_NOT_READY;
    if (r->UserVa)
        return STATUS_DEVICE_BUSY; /* one consumer at a time */
    __try {
        va = MmMapLockedPagesSpecifyCache(r->Mdl, UserMode, MmCached, NULL, FALSE,
                                          NormalPagePriority | MdlMappingNoExecute);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }
    if (!va)
        return STATUS_INSUFFICIENT_RESOURCES;
    r->UserVa = va;
    r->UserProcess = PsGetCurrentProcess();
    ObReferenceObject(r->UserProcess);
    out->Address = (ULONGLONG)(ULONG_PTR)va;
    out->Bytes = r->Bytes;
    return STATUS_SUCCESS;
}

/* PASSIVE_LEVEL. Normally called in the service's context (UNMAP_RING,
 * IRP_MJ_CLEANUP); on device removal we attach to the service process, since
 * pages must never stay mapped in user space after we free them. */
VOID Oa2dpRingUnmapUser(_Inout_ OA2DP_AUDIO_RING *r)
{
    KAPC_STATE apc;

    if (!r->UserVa)
        return;
    if (r->UserProcess == PsGetCurrentProcess()) {
        MmUnmapLockedPages(r->UserVa, r->Mdl);
    } else {
        KeStackAttachProcess((PRKPROCESS)r->UserProcess, &apc);
        MmUnmapLockedPages(r->UserVa, r->Mdl);
        KeUnstackDetachProcess(&apc);
    }
    ObDereferenceObject(r->UserProcess);
    r->UserVa = NULL;
    r->UserProcess = NULL;
}

/* Rewrites only the format fields: positions keep counting, so a consumer
 * that is still running never sees them jump backwards. */
BOOLEAN Oa2dpRingSetFormat(_Inout_ OA2DP_AUDIO_RING *r, _In_ ULONG rate, _In_ USHORT channels,
                           _In_ USHORT format)
{
    KIRQL irql;
    BOOLEAN changed = FALSE;

    if (channels == 0 || channels > OA2DP_RING_MAX_CHANNELS)
        return FALSE;
    KeAcquireSpinLock(&r->Lock, &irql);
    if (r->Ring && (r->Ring->sample_rate != rate || r->Ring->channels != channels ||
                    r->Ring->format != format)) {
        r->Ring->sample_rate = rate;
        r->Ring->channels = channels;
        r->Ring->format = format;
        KeMemoryBarrier();
        InterlockedOr((volatile LONG *)&r->Ring->flags, OA2DP_RING_FLAG_FORMAT_CHANGED);
        changed = TRUE;
    }
    KeReleaseSpinLock(&r->Lock, irql);
    return changed;
}

VOID Oa2dpRingSetRunning(_Inout_ OA2DP_AUDIO_RING *r, _In_ BOOLEAN running)
{
    KIRQL irql;

    KeAcquireSpinLock(&r->Lock, &irql);
    if (r->Ring) {
        if (running)
            InterlockedOr((volatile LONG *)&r->Ring->flags, OA2DP_RING_FLAG_RUNNING);
        else
            InterlockedAnd((volatile LONG *)&r->Ring->flags, ~(LONG)OA2DP_RING_FLAG_RUNNING);
    }
    KeReleaseSpinLock(&r->Lock, irql);
}

static VOID ConvertToS24(_Out_writes_(samples) int32_t *dst, _In_ const BYTE *src,
                         _In_ ULONG samples, _In_ USHORT bits)
{
    ULONG i;

    if (bits == 16) {
        const SHORT *s = (const SHORT *)src;
        for (i = 0; i < samples; i++)
            dst[i] = (int32_t)s[i] * 256;
    } else { /* 24-bit packed little endian */
        for (i = 0; i < samples; i++, src += 3)
            dst[i] = (int32_t)((ULONG)src[0] | ((ULONG)src[1] << 8) | ((ULONG)(CHAR)src[2] << 16));
    }
}

/* DISPATCH_LEVEL (stream timer). Drops audio when the stream format does not
 * match the ring header, which only happens while a format change races a
 * closing stream. */
VOID Oa2dpRingPump(_Inout_ OA2DP_AUDIO_RING *r, _In_reads_bytes_(cyclicBytes) const BYTE *cyclic,
                   _In_ ULONG cyclicBytes, _In_ ULONG pos, _In_ ULONG frames, _In_ USHORT channels,
                   _In_ USHORT bits)
{
    KIRQL irql;
    ULONG frameBytes = (ULONG)channels * (bits / 8);
    USHORT want = bits == 32 ? OA2DP_RING_FMT_FLOAT32 : OA2DP_RING_FMT_S32;

    if (frameBytes == 0 || cyclicBytes < frameBytes || channels > OA2DP_RING_MAX_CHANNELS ||
        (bits != 16 && bits != 24 && bits != 32))
        return;
    if (pos >= cyclicBytes)
        pos = 0;
    KeAcquireSpinLock(&r->Lock, &irql);
    if (r->Ring && r->Ring->channels == channels && r->Ring->format == want) {
        while (frames) {
            ULONG untilWrap = (cyclicBytes - pos) / frameBytes;
            ULONG n = frames < untilWrap ? frames : untilWrap;
            if (n == 0) {
                pos = 0;
                continue;
            }
            if (bits == 32) {
                (void)oa2dp_ring_write(r->Ring, cyclic + pos, n);
            } else {
                if (n > OA2DP_PUMP_CHUNK)
                    n = OA2DP_PUMP_CHUNK;
                ConvertToS24(r->Scratch, cyclic + pos, n * channels, bits);
                (void)oa2dp_ring_write(r->Ring, r->Scratch, n);
            }
            pos += n * frameBytes;
            if (pos >= cyclicBytes)
                pos = 0;
            frames -= n;
        }
    }
    KeReleaseSpinLock(&r->Lock, irql);
}
