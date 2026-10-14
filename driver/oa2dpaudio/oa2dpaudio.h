/* SPDX-License-Identifier: MIT */
/*
 * oa2dpaudio.sys — state shared by the PortCls adapter (adapter.cpp), the
 * WaveRT miniport (minwavert*.cpp), the control interface for oa2dpsvc.exe
 * (control.cpp) and the PCM ring producer (ring.c).
 *
 * Plain C so ring.c can use it. Include after <wdm.h>, <ntifs.h> or
 * <portcls.h>.
 */
#pragma once

#include "../../core/include/oa2dp/pcm_ring.h"
#include "../common/oa2dp_ioctl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* COM pointers are opaque to the C parts (ring.c, control.cpp). */
#ifdef __cplusplus
#define OA2DP_UNKNOWN PUNKNOWN
#else
#define OA2DP_UNKNOWN PVOID
#endif

#define OA2DP_AUDIO_TAG 'dA2O'

/* KS subdevice names; must match the reference strings in oa2dpaudio.inf. */
#define OA2DP_WAVE_NAME L"WaveOa2dp"
#define OA2DP_TOPO_NAME L"TopologyOa2dp"
/* Reference string of GUID_DEVINTERFACE_OA2DP_AUDIO on our PDO. */
#define OA2DP_CONTROL_NAME L"Oa2dpControl"

/* Wave filter pins (derived from SYSVAD's simple render endpoint). */
#define OA2DP_WAVE_PIN_SINK 0   /* host render stream, from the audio engine */
#define OA2DP_WAVE_PIN_BRIDGE 1 /* to the topology filter */
#define OA2DP_TOPO_PIN_IN 0
#define OA2DP_TOPO_PIN_OUT 1

/* The ring is allocated once for this many channels and never moves, so a
 * format change only rewrites its header (the service's mapping stays). */
#define OA2DP_RING_MAX_CHANNELS 2
#define OA2DP_PUMP_CHUNK 256 /* frames converted per step */

typedef struct _OA2DP_AUDIO_RING {
    KSPIN_LOCK Lock;        /* header changes vs. the producer */
    PMDL Mdl;
    oa2dp_pcm_ring *Ring;   /* kernel mapping, NULL once destroyed */
    SIZE_T Bytes;
    PVOID UserVa;           /* service mapping, if any */
    PEPROCESS UserProcess;  /* referenced while UserVa is set */
    int32_t Scratch[OA2DP_PUMP_CHUNK * OA2DP_RING_MAX_CHANNELS];
} OA2DP_AUDIO_RING;

typedef struct _OA2DP_AUDIO_DEVICE {
    BOOLEAN Initialized;
    BOOLEAN Started;
    BOOLEAN Removed;
    PDEVICE_OBJECT Fdo;
    PDEVICE_OBJECT Pdo;

    /* Spin lock for everything below that streams read at DISPATCH_LEVEL. */
    KSPIN_LOCK Lock;
    OA2DP_FORMAT_LIST Formats;  /* what the negotiated codec accepts */
    OA2DP_LATENCY Latency;
    OA2DP_FORMAT Current;       /* format of the open (or last) stream */
    BOOLEAN Running;
    BOOLEAN EventPending;       /* Event not yet picked up by WAIT_FORMAT */
    OA2DP_FORMAT_CHANGE Event;

    OA2DP_AUDIO_RING Ring;

    /* Control interface (one handle, the service). */
    KEVENT ControlLock;         /* PASSIVE_LEVEL lock, used as a mutex */
    ULONG ControlTag;           /* &ControlTag marks our FILE_OBJECTs */
    PFILE_OBJECT ControlFile;
    UNICODE_STRING ControlLink;
    IO_CSQ WaitCsq;             /* pending IOCTL_OA2DP_AUDIO_WAIT_FORMAT */
    KSPIN_LOCK WaitLock;
    LIST_ENTRY WaitList;
    WCHAR Name[RTL_FIELD_SIZE(OA2DP_ENDPOINT_NAME, Name) / sizeof(WCHAR)];

    /* KS filters (PortCls subdevices). */
    KEVENT FilterLock;          /* PASSIVE_LEVEL lock, used as a mutex */
    OA2DP_UNKNOWN WavePort;
    OA2DP_UNKNOWN TopoPort;
    PVOID WaveMiniport;         /* CMiniportWaveRT *, referenced */
    volatile LONG Streams;      /* open WaveRT streams */
    BOOLEAN NameChanged;        /* re-register the filters when idle */
    PIO_WORKITEM WorkItem;
    LONG RebuildQueued;         /* under Lock: work item queued, not started */
    volatile LONG WorkQueued;   /* work items not yet finished */
} OA2DP_AUDIO_DEVICE;

/* ---- ring.c ---------------------------------------------------------------- */

NTSTATUS Oa2dpRingCreate(_Inout_ OA2DP_AUDIO_RING *r, _In_ ULONG rate, _In_ USHORT channels,
                         _In_ USHORT format);
VOID Oa2dpRingDestroy(_Inout_ OA2DP_AUDIO_RING *r);
NTSTATUS Oa2dpRingMapUser(_Inout_ OA2DP_AUDIO_RING *r, _Out_ OA2DP_RING_MAPPING *out);
VOID Oa2dpRingUnmapUser(_Inout_ OA2DP_AUDIO_RING *r);
/* Rewrite the header in place; TRUE if the format changed. */
BOOLEAN Oa2dpRingSetFormat(_Inout_ OA2DP_AUDIO_RING *r, _In_ ULONG rate, _In_ USHORT channels,
                           _In_ USHORT format);
VOID Oa2dpRingSetRunning(_Inout_ OA2DP_AUDIO_RING *r, _In_ BOOLEAN running);
/* Copy `frames` frames of the WaveRT cyclic buffer, starting at byte offset
 * `pos`, into the ring. `bits` is the container: 32 = IEEE float, 16/24 =
 * PCM (converted to 24-bit values in int32, OA2DP_RING_FMT_S32). */
VOID Oa2dpRingPump(_Inout_ OA2DP_AUDIO_RING *r, _In_reads_bytes_(cyclicBytes) const UCHAR *cyclic,
                   _In_ ULONG cyclicBytes, _In_ ULONG pos, _In_ ULONG frames, _In_ USHORT channels,
                   _In_ USHORT bits);

/* ---- control.cpp ----------------------------------------------------------- */

VOID Oa2dpControlInit(_Inout_ OA2DP_AUDIO_DEVICE *dev);
VOID Oa2dpLoadSettings(_Inout_ OA2DP_AUDIO_DEVICE *dev);
NTSTATUS Oa2dpControlStart(_Inout_ OA2DP_AUDIO_DEVICE *dev);
VOID Oa2dpControlStop(_Inout_ OA2DP_AUDIO_DEVICE *dev);
VOID Oa2dpControlRemove(_Inout_ OA2DP_AUDIO_DEVICE *dev);
BOOLEAN Oa2dpControlIsCreate(_In_ PIRP irp);
BOOLEAN Oa2dpControlOwns(_In_ OA2DP_AUDIO_DEVICE *dev, _In_ PIRP irp);
NTSTATUS Oa2dpControlDispatch(_Inout_ OA2DP_AUDIO_DEVICE *dev, _Inout_ PIRP irp);
/* Stream state for WAIT_FORMAT (F4.3); callable up to DISPATCH_LEVEL. */
VOID Oa2dpReportStream(_Inout_ OA2DP_AUDIO_DEVICE *dev, _In_ const OA2DP_FORMAT *format,
                       _In_ BOOLEAN running);
/* Snapshot of the format list; callable up to DISPATCH_LEVEL. */
VOID Oa2dpGetFormats(_In_ OA2DP_AUDIO_DEVICE *dev, _Out_ OA2DP_FORMAT_LIST *out);
BOOLEAN Oa2dpFormatListed(_In_ const OA2DP_FORMAT_LIST *list, _In_ const OA2DP_FORMAT *f);
/* Total presentation delay in microseconds (F4.4). */
ULONG Oa2dpLatencyUs(_In_ OA2DP_AUDIO_DEVICE *dev);
VOID Oa2dpLock(_Inout_ PKEVENT lock);
VOID Oa2dpUnlock(_Inout_ PKEVENT lock);

/* ---- adapter.cpp / minwavert.cpp ------------------------------------------- */

/* Re-register the KS filters once no stream is open (new friendly name). */
VOID Oa2dpScheduleRebuild(_Inout_ OA2DP_AUDIO_DEVICE *dev);
/* Tell the audio engine that the supported formats changed. */
VOID Oa2dpWaveFormatsChanged(_Inout_ OA2DP_AUDIO_DEVICE *dev);

#ifdef __cplusplus
}
#endif
