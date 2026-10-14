/*++

Copyright (c) Microsoft Corporation All Rights Reserved

Module Name:

    bthhfpspeakerwavtable.h

Abstract:

    Declaration of wave miniport tables for the Bluetooth Handsfree speaker (external).

--*/
/*
 * Derived from the SYSVAD sample (Windows-driver-samples/audio/sysvad,
 * commit 2dc3fd3a, MS-PL — see LICENSE.MS-PL in this folder): the simple
 * render layout (one host pin -> DAC -> bridge pin) of its sideband speaker
 * endpoints. Modified for OpenA2DP: the data ranges are only an envelope;
 * the formats actually accepted are the list the service sends with
 * IOCTL_OA2DP_AUDIO_SET_FORMATS (CMiniportWaveRT::IsFormatSupported and the
 * KSPROPERTY_PIN_PROPOSEDATAFORMAT(2) handlers). One stream at a time.
 */
#pragma once

#define OA2DP_MIN_SAMPLE_RATE 8000
#define OA2DP_MAX_SAMPLE_RATE 192000
#define OA2DP_MAX_STREAMS 1

NTSTATUS PropertyHandler_WaveFilter(_In_ PPCPROPERTY_REQUEST PropertyRequest);
NTSTATUS EventHandler_PinCapsChange(_In_ PPCEVENT_REQUEST EventRequest);

static KSDATARANGE_AUDIO Oa2dpPinDataRangesStream[] =
{
    { // 0: IEEE float, 32-bit container (preferred: full precision for 24-bit codecs)
        {
            sizeof(KSDATARANGE_AUDIO),
            0,
            0,
            0,
            STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
            STATICGUIDOF(KSDATAFORMAT_SUBTYPE_IEEE_FLOAT),
            STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)
        },
        OA2DP_RING_MAX_CHANNELS,
        32,
        32,
        OA2DP_MIN_SAMPLE_RATE,
        OA2DP_MAX_SAMPLE_RATE
    },
    { // 1: integer PCM, 16 or 24-bit packed
        {
            sizeof(KSDATARANGE_AUDIO),
            0,
            0,
            0,
            STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
            STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),
            STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)
        },
        OA2DP_RING_MAX_CHANNELS,
        16,
        24,
        OA2DP_MIN_SAMPLE_RATE,
        OA2DP_MAX_SAMPLE_RATE
    }
};

static PKSDATARANGE Oa2dpPinDataRangePointersStream[] =
{
    PKSDATARANGE(&Oa2dpPinDataRangesStream[0]),
    PKSDATARANGE(&Oa2dpPinDataRangesStream[1])
};

static KSDATARANGE Oa2dpPinDataRangesBridge[] =
{
    {
        sizeof(KSDATARANGE),
        0,
        0,
        0,
        STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
        STATICGUIDOF(KSDATAFORMAT_SUBTYPE_ANALOG),
        STATICGUIDOF(KSDATAFORMAT_SPECIFIER_NONE)
    }
};

static PKSDATARANGE Oa2dpPinDataRangePointersBridge[] =
{
    &Oa2dpPinDataRangesBridge[0]
};

static PCPIN_DESCRIPTOR Oa2dpWaveMiniportPins[] =
{
    // OA2DP_WAVE_PIN_SINK
    {
        OA2DP_MAX_STREAMS,
        OA2DP_MAX_STREAMS,
        0,
        NULL,
        {
            0,
            NULL,
            0,
            NULL,
            RTL_NUMBER_OF(Oa2dpPinDataRangePointersStream),
            Oa2dpPinDataRangePointersStream,
            KSPIN_DATAFLOW_IN,
            KSPIN_COMMUNICATION_SINK,
            &KSCATEGORY_AUDIO,
            NULL,
            0
        }
    },
    // OA2DP_WAVE_PIN_BRIDGE
    {
        0,
        0,
        0,
        NULL,
        {
            0,
            NULL,
            0,
            NULL,
            RTL_NUMBER_OF(Oa2dpPinDataRangePointersBridge),
            Oa2dpPinDataRangePointersBridge,
            KSPIN_DATAFLOW_OUT,
            KSPIN_COMMUNICATION_NONE,
            &KSCATEGORY_AUDIO,
            NULL,
            0
        }
    },
};

#define OA2DP_WAVE_NODE_DAC 0

static PCNODE_DESCRIPTOR Oa2dpWaveMiniportNodes[] =
{
    {
        0,                          // Flags
        NULL,                       // AutomationTable
        &KSNODETYPE_DAC,            // Type
        NULL                        // Name
    }
};

static PCCONNECTION_DESCRIPTOR Oa2dpWaveMiniportConnections[] =
{
    { PCFILTER_NODE,          OA2DP_WAVE_PIN_SINK, OA2DP_WAVE_NODE_DAC, 1 },
    { OA2DP_WAVE_NODE_DAC,    0,                   PCFILTER_NODE,       OA2DP_WAVE_PIN_BRIDGE },
};

static PCPROPERTY_ITEM PropertiesOa2dpWaveFilter[] =
{
    {
        &KSPROPSETID_Pin,
        KSPROPERTY_PIN_PROPOSEDATAFORMAT,
        KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT,
        PropertyHandler_WaveFilter
    },
    {
        &KSPROPSETID_Pin,
        KSPROPERTY_PIN_PROPOSEDATAFORMAT2,
        KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT,
        PropertyHandler_WaveFilter
    }
};

static PCEVENT_ITEM Oa2dpFormatChangePinEvent[] =
{
    {
        &KSEVENTSETID_PinCapsChange,
        KSEVENT_PINCAPS_FORMATCHANGE,
        KSEVENT_TYPE_ENABLE | KSEVENT_TYPE_BASICSUPPORT,
        EventHandler_PinCapsChange
    }
};

DEFINE_PCAUTOMATION_TABLE_PROP_EVENT(AutomationOa2dpWaveFilter, PropertiesOa2dpWaveFilter, Oa2dpFormatChangePinEvent);

static PCFILTER_DESCRIPTOR Oa2dpWaveMiniportFilterDescriptor =
{
    0,                                              // Version
    &AutomationOa2dpWaveFilter,                     // AutomationTable
    sizeof(PCPIN_DESCRIPTOR),                       // PinSize
    RTL_NUMBER_OF(Oa2dpWaveMiniportPins),           // PinCount
    Oa2dpWaveMiniportPins,                          // Pins
    sizeof(PCNODE_DESCRIPTOR),                      // NodeSize
    RTL_NUMBER_OF(Oa2dpWaveMiniportNodes),          // NodeCount
    Oa2dpWaveMiniportNodes,                         // Nodes
    RTL_NUMBER_OF(Oa2dpWaveMiniportConnections),    // ConnectionCount
    Oa2dpWaveMiniportConnections,                   // Connections
    0,                                              // CategoryCount
    NULL                                            // Categories - defaults (audio, render)
};
