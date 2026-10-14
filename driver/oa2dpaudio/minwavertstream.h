/*++

Copyright (c) 1997-2010  Microsoft Corporation All Rights Reserved

Module Name:

    minwavertstream.h

Abstract:

    Definition of wavert miniport class.

--*/
/*
 * Derived from the SYSVAD sample (Windows-driver-samples/audio/sysvad,
 * commit 2dc3fd3a, MS-PL — see LICENSE.MS-PL in this folder). Modified for
 * OpenA2DP: instead of saving rendered audio to a file, the position timer
 * pumps it into the PCM ring shared with oa2dpsvc.exe; positions are kept
 * in frames (no drift); presentation position and hardware latency include
 * the Bluetooth transport and headset delay (IOCTL_OA2DP_AUDIO_SET_LATENCY).
 */
#pragma once

#include "minwavert.h"

typedef struct _NotificationListEntry
{
    LIST_ENTRY  ListEntry;
    PKEVENT     NotificationEvent;
} NotificationListEntry;

EXT_CALLBACK TimerNotifyRT;

class CMiniportWaveRTStream :
    public IMiniportWaveRTStreamNotification,
    public IMiniportWaveRTOutputStream,
    public CUnknown
{
public:
    DECLARE_STD_UNKNOWN();
    DEFINE_STD_CONSTRUCTOR(CMiniportWaveRTStream);
    ~CMiniportWaveRTStream();

    IMP_IMiniportWaveRTStream;
    IMP_IMiniportWaveRTStreamNotification;
    IMP_IMiniportWaveRTOutputStream;

    NTSTATUS Init
    (
        _In_ PCMiniportWaveRT       Miniport,
        _In_ PPORTWAVERTSTREAM      PortStream,
        _In_ const OA2DP_FORMAT    *Format
    );

    friend EXT_CALLBACK TimerNotifyRT;

private:
    // All below with m_PositionSpinLock held.
    ULONGLONG RunningHns(_In_ LARGE_INTEGER Qpc);
    VOID UpdatePosition(_In_ LARGE_INTEGER Qpc);
    VOID Pump(_In_ ULONGLONG ToFrame);

    PCMiniportWaveRT            m_pMiniport;
    OA2DP_AUDIO_DEVICE         *m_Device;
    PPORTWAVERTSTREAM           m_pPortStream;
    OA2DP_FORMAT                m_Format;
    ULONG                       m_ulBlockAlign;
    BOOLEAN                     m_bCounted;

    LIST_ENTRY                  m_NotificationList;
    PEX_TIMER                   m_pNotificationTimer;
    KSPIN_LOCK                  m_PositionSpinLock;
    KSSTATE                     m_KsState;

    BYTE                       *m_pDmaBuffer;
    ULONG                       m_ulDmaBufferSize;
    ULONG                       m_ulNotificationsPerBuffer;

    LARGE_INTEGER               m_PerfFrequency;
    LARGE_INTEGER               m_RunStartQpc;
    ULONGLONG                   m_hnsRunBase;       // time spent in RUN before m_RunStartQpc
    ULONGLONG                   m_ullLinearFrames;  // frames played since STOP
    ULONGLONG                   m_ullPumpedFrames;  // frames copied to the ring
    LONGLONG                    m_llPacketCounter;  // completed packets (event mode)
    ULONG                       m_ulLastOsWritePacket;
    BOOLEAN                     m_bEoSReceived;
    ULONGLONG                   m_ullEosFrame;
};
typedef CMiniportWaveRTStream *PCMiniportWaveRTStream;
