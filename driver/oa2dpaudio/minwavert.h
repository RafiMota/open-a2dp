/*++

Copyright (c) Microsoft Corporation All Rights Reserved

Module Name:

    minwavert.h

Abstract:

    Definition of wavert miniport class.

--*/
/*
 * Derived from the SYSVAD sample (Windows-driver-samples/audio/sysvad,
 * commit 2dc3fd3a, MS-PL — see LICENSE.MS-PL in this folder). Modified for
 * OpenA2DP: one render endpoint whose format list comes from oa2dpsvc.exe;
 * offload, loopback, keyword detector, audio modules, DRM and sideband
 * support removed.
 */
#pragma once

#include "driver.h"

class CMiniportWaveRT :
    public IMiniportWaveRT,     // must stay first: property handlers cast MajorTarget
    public CUnknown
{
public:
    DECLARE_STD_UNKNOWN();
    CMiniportWaveRT(_In_opt_ PUNKNOWN UnknownOuter, _In_ OA2DP_AUDIO_DEVICE *Device)
        : CUnknown(UnknownOuter), m_Device(Device), m_pPortEvents(NULL)
    {
    }
    ~CMiniportWaveRT();

    IMP_IMiniportWaveRT;

    OA2DP_AUDIO_DEVICE *Device() const { return m_Device; }

    // STATUS_SUCCESS if DataFormat is one of the service's formats.
    NTSTATUS IsFormatSupported(_In_ PKSDATAFORMAT DataFormat, _Out_opt_ OA2DP_FORMAT *Format);
    // Raise KSEVENT_PINCAPS_FORMATCHANGE so the audio engine re-reads formats.
    VOID FormatsChanged();

    NTSTATUS PropertyHandlerProposedFormat(_In_ PPCPROPERTY_REQUEST PropertyRequest);
    NTSTATUS PropertyHandlerProposedFormat2(_In_ PPCPROPERTY_REQUEST PropertyRequest);
    NTSTATUS EventHandlerPinCapsChange(_In_ PPCEVENT_REQUEST EventRequest);

private:
    OA2DP_AUDIO_DEVICE *m_Device;
    PPORTEVENTS m_pPortEvents;
};
typedef CMiniportWaveRT *PCMiniportWaveRT;

VOID Oa2dpBuildWaveFormat(_In_ const OA2DP_FORMAT *Format, _Out_ PKSDATAFORMAT_WAVEFORMATEXTENSIBLE Out);
