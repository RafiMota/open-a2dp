/*++

Copyright (c) Microsoft Corporation All Rights Reserved

Module Name:

    minwavertstream.cpp

Abstract:

    Implementation of wavert miniport stream.

--*/
/*
 * Derived from the SYSVAD sample (Windows-driver-samples/audio/sysvad,
 * commit 2dc3fd3a, MS-PL — see LICENSE.MS-PL in this folder). Modified for
 * OpenA2DP:
 *  - The 1 ms position timer (SYSVAD's hardware emulation) copies what the
 *    audio engine rendered into the PCM ring shared with oa2dpsvc.exe
 *    (Oa2dpRingPump) instead of writing it to a file. It runs in polled
 *    mode too, so the ring is fed either way.
 *  - Positions are derived from the elapsed RUN time in frames, so they do
 *    not drift and never split a frame.
 *  - SetState(RUN) / leaving RUN set OA2DP_RING_FLAG_RUNNING and complete
 *    IOCTL_OA2DP_AUDIO_WAIT_FORMAT; a new stream format rewrites the ring
 *    header in place (F4.3).
 *  - GetHWLatency and the presentation position include the Bluetooth
 *    transport and headset delay (IOCTL_OA2DP_AUDIO_SET_LATENCY, F4.4).
 *  - Capture, loopback, offload, tone generator, volume/peak meter, audio
 *    modules, DRM, ETW and sideband code removed.
 */
#include <limits.h>

#include "minwavert.h"
#include "minwavertstream.h"

static ULONGLONG HnsToFrames(_In_ ULONGLONG Hns, _In_ ULONG Rate)
{
    // Split to avoid overflow on long-running streams.
    return (Hns / OA2DP_HNS_PER_SECOND) * Rate + ((Hns % OA2DP_HNS_PER_SECOND) * Rate) / OA2DP_HNS_PER_SECOND;
}

//=============================================================================
#pragma code_seg("PAGE")
CMiniportWaveRTStream::~CMiniportWaveRTStream
(
    void
)
{
    PAGED_CODE();

    if (m_pNotificationTimer)
    {
        ExDeleteTimer
        (
            m_pNotificationTimer,
            TRUE, // Cancel the timer if it is currently set.
            TRUE, // Wait for the timer to finish expiring and for any callback to finish.
            NULL
        );
        m_pNotificationTimer = NULL;
    }

    while (!IsListEmpty(&m_NotificationList))
    {
        PLIST_ENTRY le = RemoveHeadList(&m_NotificationList);
        ExFreePoolWithTag(CONTAINING_RECORD(le, NotificationListEntry, ListEntry), OA2DP_AUDIO_TAG);
    }

    if (m_bCounted && m_Device)
    {
        // A friendly-name change waits for the last stream to close.
        if (InterlockedDecrement(&m_Device->Streams) == 0 && m_Device->NameChanged)
        {
            Oa2dpScheduleRebuild(m_Device);
        }
        m_bCounted = FALSE;
    }

    if (NULL != m_pMiniport)
    {
        m_pMiniport->Release();
        m_pMiniport = NULL;
    }
} // ~CMiniportWaveRTStream

//=============================================================================
#pragma code_seg("PAGE")
NTSTATUS
CMiniportWaveRTStream::Init
(
    _In_ PCMiniportWaveRT           Miniport_,
    _In_ PPORTWAVERTSTREAM          PortStream_,
    _In_ const OA2DP_FORMAT        *Format_
)
{
    PAGED_CODE();

    InitializeListHead(&m_NotificationList);
    KeInitializeSpinLock(&m_PositionSpinLock);

    m_pMiniport = Miniport_;
    m_pMiniport->AddRef();
    m_Device = Miniport_->Device();
    m_pPortStream = PortStream_;
    m_Format = *Format_;
    m_ulBlockAlign = (ULONG)Format_->Channels * (Format_->BitsPerSample / 8);
    m_bCounted = FALSE;
    m_KsState = KSSTATE_STOP;
    m_pDmaBuffer = NULL;
    m_ulDmaBufferSize = 0;
    m_ulNotificationsPerBuffer = 0;
    m_hnsRunBase = 0;
    m_ullLinearFrames = 0;
    m_ullPumpedFrames = 0;
    m_llPacketCounter = 0;
    m_ulLastOsWritePacket = ULONG_MAX;
    m_bEoSReceived = FALSE;
    m_ullEosFrame = 0;
    KeQueryPerformanceCounter(&m_PerfFrequency);
    m_RunStartQpc.QuadPart = 0;

    if (m_ulBlockAlign == 0)
    {
        return STATUS_INVALID_PARAMETER;
    }

    m_pNotificationTimer = ExAllocateTimer(
        TimerNotifyRT,
        this,
        EX_TIMER_HIGH_RESOLUTION
    );
    if (!m_pNotificationTimer)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    InterlockedIncrement(&m_Device->Streams);
    m_bCounted = TRUE;

    // F4.3: Windows opened the endpoint with a (possibly) new format. The ring
    // allocation never moves, so only its header changes; the service learns
    // about it from WAIT_FORMAT and reconfigures the headset.
    if (Oa2dpRingSetFormat(&m_Device->Ring, m_Format.SampleRate, m_Format.Channels,
                           m_Format.BitsPerSample == 32 ? OA2DP_RING_FMT_FLOAT32 : OA2DP_RING_FMT_S32))
    {
        Oa2dpReportStream(m_Device, &m_Format, FALSE);
    }

    return STATUS_SUCCESS;
} // Init

//=============================================================================
#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS)
CMiniportWaveRTStream::NonDelegatingQueryInterface
(
    _In_ REFIID  Interface,
    _COM_Outptr_ PVOID * Object
)
{
    PAGED_CODE();

    ASSERT(Object);

    if (IsEqualGUIDAligned(Interface, IID_IUnknown))
    {
        *Object = PVOID(PUNKNOWN(PMINIPORTWAVERTSTREAM(this)));
    }
    else if (IsEqualGUIDAligned(Interface, IID_IMiniportWaveRTStream))
    {
        *Object = PVOID(PMINIPORTWAVERTSTREAM(this));
    }
    else if (IsEqualGUIDAligned(Interface, IID_IMiniportWaveRTStreamNotification))
    {
        *Object = PVOID(PMINIPORTWAVERTSTREAMNOTIFICATION(this));
    }
    else if (IsEqualGUIDAligned(Interface, IID_IMiniportWaveRTOutputStream))
    {
        *Object = PVOID(PMINIPORTWAVERTOUTPUTSTREAM(this));
    }
    else
    {
        *Object = NULL;
    }

    if (*Object)
    {
        PUNKNOWN(*Object)->AddRef();
        return STATUS_SUCCESS;
    }

    return STATUS_INVALID_PARAMETER;
} // NonDelegatingQueryInterface

//=============================================================================
#pragma code_seg()
STDMETHODIMP_(NTSTATUS)
CMiniportWaveRTStream::AllocateBufferWithNotification
(
    _In_    ULONG               NotificationCount_,
    _In_    ULONG               RequestedSize_,
    _Out_   PMDL                *AudioBufferMdl_,
    _Out_   ULONG               *ActualSize_,
    _Out_   ULONG               *OffsetFromFirstPage_,
    _Out_   MEMORY_CACHING_TYPE *CacheType_
)
{

    ULONG packetSize;
    ULONG size;
    KIRQL oldIrql;
    BYTE *buffer;

    if ((0 == RequestedSize_) || (RequestedSize_ < m_ulBlockAlign))
    {
        return STATUS_UNSUCCESSFUL;
    }

    if ((NotificationCount_ == 0) || (RequestedSize_ % NotificationCount_ != 0))
    {
        return STATUS_INVALID_PARAMETER;
    }

    // Whole frames per packet, so packet boundaries are frame boundaries.
    packetSize = RequestedSize_ / NotificationCount_;
    packetSize -= packetSize % m_ulBlockAlign;
    if (packetSize == 0)
    {
        return STATUS_INVALID_PARAMETER;
    }
    size = packetSize * NotificationCount_;

    PHYSICAL_ADDRESS highAddress;
    highAddress.HighPart = 0;
    highAddress.LowPart = MAXULONG;
    PMDL pBufferMdl = m_pPortStream->AllocatePagesForMdl (highAddress, size);

    if (NULL == pBufferMdl)
    {
        return STATUS_UNSUCCESSFUL;
    }

    // From MSDN: drivers must select a caching type for mapped memory
    // buffers that does not impose platform-specific alignment requirements.
    buffer = (BYTE*)m_pPortStream->MapAllocatedPages(pBufferMdl, MmCached);
    if (buffer == NULL)
    {
        m_pPortStream->FreePagesFromMdl(pBufferMdl);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    KeAcquireSpinLock(&m_PositionSpinLock, &oldIrql);
    m_pDmaBuffer = buffer;
    m_ulNotificationsPerBuffer = NotificationCount_;
    m_ulDmaBufferSize = size;
    KeReleaseSpinLock(&m_PositionSpinLock, oldIrql);

    *AudioBufferMdl_ = pBufferMdl;
    *ActualSize_ = size;
    *OffsetFromFirstPage_ = 0;
    *CacheType_ = MmCached;

    return STATUS_SUCCESS;
}

//=============================================================================
#pragma code_seg()
STDMETHODIMP_(VOID)
CMiniportWaveRTStream::FreeBufferWithNotification
(
    _In_        PMDL    Mdl_,
    _In_        ULONG   Size_
)
{
    FreeAudioBuffer(Mdl_, Size_);
}

//=============================================================================
#pragma code_seg()
STDMETHODIMP_(NTSTATUS)
CMiniportWaveRTStream::RegisterNotificationEvent
(
    _In_ PKEVENT NotificationEvent_
)
{

    NTSTATUS ntStatus = STATUS_SUCCESS;
    KIRQL oldIrql;
    NotificationListEntry *nleNew = (NotificationListEntry*)ExAllocatePool2(
        POOL_FLAG_NON_PAGED,
        sizeof(NotificationListEntry),
        OA2DP_AUDIO_TAG);
    if (NULL == nleNew)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    nleNew->NotificationEvent = NotificationEvent_;

    KeAcquireSpinLock(&m_PositionSpinLock, &oldIrql);
    // Fail if the notification event already exists in our list.
    for (PLIST_ENTRY le = m_NotificationList.Flink; le != &m_NotificationList; le = le->Flink)
    {
        if (CONTAINING_RECORD(le, NotificationListEntry, ListEntry)->NotificationEvent == NotificationEvent_)
        {
            ntStatus = STATUS_UNSUCCESSFUL;
            break;
        }
    }
    if (NT_SUCCESS(ntStatus))
    {
        InsertTailList(&m_NotificationList, &(nleNew->ListEntry));
        nleNew = NULL;
    }
    KeReleaseSpinLock(&m_PositionSpinLock, oldIrql);

    if (nleNew)
    {
        ExFreePoolWithTag(nleNew, OA2DP_AUDIO_TAG);
    }
    return ntStatus;
}

//=============================================================================
#pragma code_seg()
STDMETHODIMP_(NTSTATUS)
CMiniportWaveRTStream::UnregisterNotificationEvent
(
    _In_ PKEVENT NotificationEvent_
)
{

    NotificationListEntry *found = NULL;
    KIRQL oldIrql;

    KeAcquireSpinLock(&m_PositionSpinLock, &oldIrql);
    for (PLIST_ENTRY le = m_NotificationList.Flink; le != &m_NotificationList; le = le->Flink)
    {
        NotificationListEntry* nleCurrent = CONTAINING_RECORD(le, NotificationListEntry, ListEntry);
        if (nleCurrent->NotificationEvent == NotificationEvent_)
        {
            RemoveEntryList(le);
            found = nleCurrent;
            break;
        }
    }
    KeReleaseSpinLock(&m_PositionSpinLock, oldIrql);

    if (found)
    {
        ExFreePoolWithTag(found, OA2DP_AUDIO_TAG);
        return STATUS_SUCCESS;
    }
    return STATUS_NOT_FOUND;
}

//=============================================================================
#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS)
CMiniportWaveRTStream::GetClockRegister
(
    _Out_ PKSRTAUDIO_HWREGISTER Register_
)
{
    UNREFERENCED_PARAMETER(Register_);
    PAGED_CODE();
    return STATUS_NOT_IMPLEMENTED;
}

//=============================================================================
#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS)
CMiniportWaveRTStream::GetPositionRegister
(
    _Out_ PKSRTAUDIO_HWREGISTER Register_
)
{
    UNREFERENCED_PARAMETER(Register_);
    PAGED_CODE();
    return STATUS_NOT_IMPLEMENTED;
}

//=============================================================================
#pragma code_seg("PAGE")
STDMETHODIMP_(VOID)
CMiniportWaveRTStream::GetHWLatency
(
    _Out_ PKSRTAUDIO_HWLATENCY  Latency_
)
{
    PAGED_CODE();
    ASSERT(Latency_);

    // F4.4: encoder + packet queue + air (service) and the headset's AVDTP
    // delay report. CodecDelay is in 100 ns units.
    Latency_->ChipsetDelay = 0;
    Latency_->CodecDelay = Oa2dpLatencyUs(m_Device) * 10;
    Latency_->FifoSize = 0;
}

//=============================================================================
#pragma code_seg()
STDMETHODIMP_(VOID)
CMiniportWaveRTStream::FreeAudioBuffer
(
    _In_opt_    PMDL        Mdl_,
    _In_        ULONG       Size_
)
{
    UNREFERENCED_PARAMETER(Size_);

    KIRQL oldIrql;
    BYTE *buffer;

    KeAcquireSpinLock(&m_PositionSpinLock, &oldIrql);
    buffer = m_pDmaBuffer;
    m_pDmaBuffer = NULL;
    m_ulDmaBufferSize = 0;
    m_ulNotificationsPerBuffer = 0;
    KeReleaseSpinLock(&m_PositionSpinLock, oldIrql);

    if (Mdl_ != NULL)
    {
        if (buffer != NULL)
        {
            m_pPortStream->UnmapAllocatedPages(buffer, Mdl_);
        }
        m_pPortStream->FreePagesFromMdl(Mdl_);
    }
}

//=============================================================================
#pragma code_seg()
STDMETHODIMP_(NTSTATUS)
CMiniportWaveRTStream::AllocateAudioBuffer
(
    _In_    ULONG                   RequestedSize_,
    _Out_   PMDL                   *AudioBufferMdl_,
    _Out_   ULONG                  *ActualSize_,
    _Out_   ULONG                  *OffsetFromFirstPage_,
    _Out_   MEMORY_CACHING_TYPE    *CacheType_
)
{

    KIRQL oldIrql;
    BYTE *buffer;

    if ((0 == RequestedSize_) || (RequestedSize_ < m_ulBlockAlign))
    {
        return STATUS_UNSUCCESSFUL;
    }

    RequestedSize_ -= RequestedSize_ % m_ulBlockAlign;

    PHYSICAL_ADDRESS highAddress;
    highAddress.HighPart = 0;
    highAddress.LowPart = MAXULONG;
    PMDL pBufferMdl = m_pPortStream->AllocatePagesForMdl(highAddress, RequestedSize_);

    if (NULL == pBufferMdl)
    {
        return STATUS_UNSUCCESSFUL;
    }

    buffer = (BYTE*)m_pPortStream->MapAllocatedPages(pBufferMdl, MmCached);
    if (buffer == NULL)
    {
        m_pPortStream->FreePagesFromMdl(pBufferMdl);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    KeAcquireSpinLock(&m_PositionSpinLock, &oldIrql);
    m_pDmaBuffer = buffer;
    m_ulDmaBufferSize = RequestedSize_;
    m_ulNotificationsPerBuffer = 0;
    KeReleaseSpinLock(&m_PositionSpinLock, oldIrql);

    *AudioBufferMdl_ = pBufferMdl;
    *ActualSize_ = RequestedSize_;
    *OffsetFromFirstPage_ = 0;
    *CacheType_ = MmCached;

    return STATUS_SUCCESS;
}

//=============================================================================
// Position bookkeeping (m_PositionSpinLock held).
#pragma code_seg()
ULONGLONG CMiniportWaveRTStream::RunningHns(_In_ LARGE_INTEGER Qpc)
{
    LARGE_INTEGER delta;
    delta.QuadPart = Qpc.QuadPart - m_RunStartQpc.QuadPart;
    if (delta.QuadPart < 0)
    {
        delta.QuadPart = 0;
    }
    return m_hnsRunBase + KSCONVERT_PERFORMANCE_TIME(m_PerfFrequency.QuadPart, delta);
}

#pragma code_seg()
VOID CMiniportWaveRTStream::Pump(_In_ ULONGLONG ToFrame)
{
    ULONGLONG start = m_ullPumpedFrames;
    ULONGLONG end = ToFrame;
    ULONGLONG bufferFrames;

    if (m_bEoSReceived && end > m_ullEosFrame)
    {
        end = m_ullEosFrame;  // nothing valid after the end of stream
    }
    if (end <= start)
    {
        return;
    }
    if (m_pDmaBuffer == NULL || m_ulDmaBufferSize < m_ulBlockAlign)
    {
        m_ullPumpedFrames = end;
        return;
    }
    bufferFrames = m_ulDmaBufferSize / m_ulBlockAlign;
    if (end - start > bufferFrames)
    {
        start = end - bufferFrames;  // timer starved: older frames are gone
    }
    Oa2dpRingPump(&m_Device->Ring, m_pDmaBuffer, m_ulDmaBufferSize,
                  (ULONG)((start % bufferFrames) * m_ulBlockAlign), (ULONG)(end - start),
                  m_Format.Channels, m_Format.BitsPerSample);
    m_ullPumpedFrames = end;
}

#pragma code_seg()
VOID CMiniportWaveRTStream::UpdatePosition(_In_ LARGE_INTEGER Qpc)
{
    ULONGLONG frames = HnsToFrames(RunningHns(Qpc), m_Format.SampleRate);

    if (frames <= m_ullLinearFrames)
    {
        return;
    }
    // The engine wrote these frames ahead of the play position; now they
    // are "played", i.e. handed to the service.
    Pump(frames);
    m_ullLinearFrames = frames;
}

//=============================================================================
#pragma code_seg()
STDMETHODIMP_(NTSTATUS)
CMiniportWaveRTStream::GetPosition
(
    _Out_   KSAUDIO_POSITION    *Position_
)
{
    KIRQL oldIrql;
    ULONGLONG offset = 0;

    KeAcquireSpinLock(&m_PositionSpinLock, &oldIrql);
    if (m_KsState == KSSTATE_RUN)
    {
        UpdatePosition(KeQueryPerformanceCounter(NULL));
    }
    if (m_ulDmaBufferSize)
    {
        offset = (m_ullLinearFrames * m_ulBlockAlign) % m_ulDmaBufferSize;
    }
    Position_->PlayOffset = offset;
    Position_->WriteOffset = offset;
    KeReleaseSpinLock(&m_PositionSpinLock, oldIrql);

    return STATUS_SUCCESS;
}

//=============================================================================
// CMiniportWaveRTStream::SetWritePacket
//  Event-driven mode: the OS wrote packet PacketNumber.
#pragma code_seg()
STDMETHODIMP_(NTSTATUS)
CMiniportWaveRTStream::SetWritePacket
(
    _In_ ULONG      PacketNumber,
    _In_ DWORD      Flags,
    _In_ ULONG      EosPacketLength
)
{
    KIRQL oldIrql;
    NTSTATUS ntStatus = STATUS_SUCCESS;

    // The call must be from event driven mode
    if (m_ulNotificationsPerBuffer == 0)
    {
        return STATUS_NOT_SUPPORTED;
    }

    KeAcquireSpinLock(&m_PositionSpinLock, &oldIrql);

    // This function should not be called once EoS has been set.
    if (m_bEoSReceived)
    {
        ntStatus = STATUS_INVALID_DEVICE_STATE;
        goto Done;
    }

    {
        // 1-based count of completed packets, 0-based packet number of
        // current packet. If not running, the OS should be writing the
        // current packet; if running, the one after it.
        ULONGLONG expectedPacket = (ULONGLONG)m_llPacketCounter + (m_KsState == KSSTATE_RUN ? 1 : 0);
        LONG deltaFromExpectedPacket = (LONG)(PacketNumber - (ULONG)expectedPacket); // Modulo arithmetic
        ULONG packetSize = m_ulDmaBufferSize / m_ulNotificationsPerBuffer;

        if (deltaFromExpectedPacket < 0)
        {
            ntStatus = STATUS_DATA_LATE_ERROR;
            goto Done;
        }
        else if (deltaFromExpectedPacket > 0)
        {
            ntStatus = STATUS_DATA_OVERRUN;
            goto Done;
        }

        if (Flags & KSSTREAM_HEADER_OPTIONSF_ENDOFSTREAM)
        {
            if (EosPacketLength > packetSize)
            {
                ntStatus = STATUS_INVALID_PARAMETER;
                goto Done;
            }
            // Stop feeding the ring after the last valid frame.
            m_bEoSReceived = TRUE;
            m_ullEosFrame = expectedPacket * (packetSize / m_ulBlockAlign) + EosPacketLength / m_ulBlockAlign;
        }
        m_ulLastOsWritePacket = PacketNumber;
    }

Done:
    KeReleaseSpinLock(&m_PositionSpinLock, oldIrql);
    return ntStatus;
}

//=============================================================================
#pragma code_seg()
_IRQL_requires_max_(PASSIVE_LEVEL)
STDMETHODIMP_(NTSTATUS)
CMiniportWaveRTStream::GetOutputStreamPresentationPosition
(
    _Out_ KSAUDIO_PRESENTATION_POSITION *pPresentationPosition
)
{
    KIRQL oldIrql;
    LARGE_INTEGER qpc;
    ULONGLONG linear;
    ULONGLONG latencyFrames;

    ASSERT (pPresentationPosition);

    // The call must be from event driven mode
    if (m_ulNotificationsPerBuffer == 0)
    {
        return STATUS_NOT_SUPPORTED;
    }

    KeAcquireSpinLock(&m_PositionSpinLock, &oldIrql);
    qpc = KeQueryPerformanceCounter(NULL);
    if (m_KsState == KSSTATE_RUN)
    {
        UpdatePosition(qpc);
    }
    linear = m_ullLinearFrames;
    KeReleaseSpinLock(&m_PositionSpinLock, oldIrql);

    // What the listener hears lags what we handed to the service by the
    // transport and headset delay (lip sync, F4.4).
    latencyFrames = HnsToFrames((ULONGLONG)Oa2dpLatencyUs(m_Device) * 10, m_Format.SampleRate);
    pPresentationPosition->u64PositionInBlocks = linear > latencyFrames ? linear - latencyFrames : 0;
    pPresentationPosition->u64QPCPosition = (UINT64)qpc.QuadPart;

    return STATUS_SUCCESS;
}

//=============================================================================
#pragma code_seg()
_IRQL_requires_max_(PASSIVE_LEVEL)
STDMETHODIMP_(NTSTATUS)
CMiniportWaveRTStream::GetPacketCount
(
    _Out_ ULONG *pPacketCount
)
{
    KIRQL oldIrql;

    ASSERT(pPacketCount);

    // The call must be from event driven mode
    if (m_ulNotificationsPerBuffer == 0)
    {
        return STATUS_NOT_SUPPORTED;
    }

    KeAcquireSpinLock(&m_PositionSpinLock, &oldIrql);
    if (m_KsState == KSSTATE_RUN)
    {
        UpdatePosition(KeQueryPerformanceCounter(NULL));
    }
    *pPacketCount = (ULONG)m_llPacketCounter;
    KeReleaseSpinLock(&m_PositionSpinLock, oldIrql);

    return STATUS_SUCCESS;
}

//=============================================================================
#pragma code_seg()
STDMETHODIMP_(NTSTATUS)
CMiniportWaveRTStream::SetState
(
    _In_    KSSTATE State_
)
{
    KIRQL oldIrql;
    LARGE_INTEGER qpc;
    BOOLEAN wasRunning = (m_KsState == KSSTATE_RUN);

    switch (State_)
    {
        case KSSTATE_STOP:
            KeAcquireSpinLock(&m_PositionSpinLock, &oldIrql);
            // Reset DMA
            m_llPacketCounter = 0;
            m_ullLinearFrames = 0;
            m_ullPumpedFrames = 0;
            m_hnsRunBase = 0;
            // Reset OS write positions
            m_ulLastOsWritePacket = ULONG_MAX;
            m_bEoSReceived = FALSE;
            m_ullEosFrame = 0;
            m_KsState = KSSTATE_STOP;
            KeReleaseSpinLock(&m_PositionSpinLock, oldIrql);
            break;

        case KSSTATE_ACQUIRE:
            break;

        case KSSTATE_PAUSE:
            if (wasRunning)
            {
                // Run -> Pause: account the frames played so far, then stop
                // the timer (its callback sees the new state and returns).
                KeAcquireSpinLock(&m_PositionSpinLock, &oldIrql);
                qpc = KeQueryPerformanceCounter(NULL);
                UpdatePosition(qpc);
                m_hnsRunBase = RunningHns(qpc);
                m_KsState = KSSTATE_PAUSE;
                KeReleaseSpinLock(&m_PositionSpinLock, oldIrql);

                ExCancelTimer(m_pNotificationTimer, NULL);

                Oa2dpRingSetRunning(&m_Device->Ring, FALSE);
                Oa2dpReportStream(m_Device, &m_Format, FALSE);
            }
            break;

        case KSSTATE_RUN:
            if (!wasRunning)
            {
                // Start DMA
                KeAcquireSpinLock(&m_PositionSpinLock, &oldIrql);
                m_RunStartQpc = KeQueryPerformanceCounter(&m_PerfFrequency);
                m_KsState = KSSTATE_RUN;
                KeReleaseSpinLock(&m_PositionSpinLock, oldIrql);

                // 1 ms timer, like SYSVAD's hardware emulation: it pumps the
                // ring and sends the buffer-completion events.
                ExSetTimer
                (
                    m_pNotificationTimer,
                    (-1) * OA2DP_HNS_PER_MS,
                    OA2DP_HNS_PER_MS,
                    NULL
                );

                Oa2dpRingSetRunning(&m_Device->Ring, TRUE);
                Oa2dpReportStream(m_Device, &m_Format, TRUE);
            }
            break;
    }

    m_KsState = State_;
    return STATUS_SUCCESS;
}

//=============================================================================
#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS)
CMiniportWaveRTStream::SetFormat
(
    _In_    KSDATAFORMAT    *DataFormat_
)
{
    UNREFERENCED_PARAMETER(DataFormat_);
    PAGED_CODE();
    // A new format means a new stream (and a WAIT_FORMAT event).
    return STATUS_NOT_SUPPORTED;
}

//=============================================================================
#pragma code_seg()
void
TimerNotifyRT
(
    _In_      PEX_TIMER    Timer,
    _In_opt_  PVOID        DeferredContext
)
{
    BOOLEAN bufferCompleted = FALSE;

    UNREFERENCED_PARAMETER(Timer);

    _IRQL_limited_to_(DISPATCH_LEVEL);

    CMiniportWaveRTStream* _this = (CMiniportWaveRTStream*)DeferredContext;

    if (NULL == _this)
    {
        return;
    }

    KIRQL oldIrql;
    KeAcquireSpinLock(&_this->m_PositionSpinLock, &oldIrql);

    if (_this->m_KsState != KSSTATE_RUN)
    {
        goto End;
    }

    _this->UpdatePosition(KeQueryPerformanceCounter(NULL));

    // Send a buffer completion event each time a whole packet was played.
    if (_this->m_ulNotificationsPerBuffer != 0)
    {
        ULONGLONG packetFrames =
            (_this->m_ulDmaBufferSize / _this->m_ulNotificationsPerBuffer) / _this->m_ulBlockAlign;
        if (packetFrames != 0)
        {
            LONGLONG packets = (LONGLONG)(_this->m_ullLinearFrames / packetFrames);
            if (packets > _this->m_llPacketCounter)
            {
                _this->m_llPacketCounter = packets;
                bufferCompleted = TRUE;
            }
        }
    }

    if (bufferCompleted)
    {
        for (PLIST_ENTRY le = _this->m_NotificationList.Flink; le != &_this->m_NotificationList; le = le->Flink)
        {
            NotificationListEntry* nleCurrent = CONTAINING_RECORD(le, NotificationListEntry, ListEntry);
            KeSetEvent(nleCurrent->NotificationEvent, 0, 0);
        }
    }

End:
    KeReleaseSpinLock(&_this->m_PositionSpinLock, oldIrql);
    return;
}
