/*++

Copyright (c) Microsoft Corporation All Rights Reserved

Module Name:

    speakertoptable.h

Abstract:

    Declaration of topology tables.

--*/
/*
 * Derived from the SYSVAD sample (Windows-driver-samples/audio/sysvad,
 * commit 2dc3fd3a, MS-PL — see LICENSE.MS-PL in this folder). Modified for
 * OpenA2DP: headphone output (Sound settings shows "Headphones"), no jack,
 * resource-group or posture properties.
 */
#pragma once

static KSDATARANGE Oa2dpTopoPinDataRangesBridge[] =
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

static PKSDATARANGE Oa2dpTopoPinDataRangePointersBridge[] =
{
    &Oa2dpTopoPinDataRangesBridge[0]
};

static PCPIN_DESCRIPTOR Oa2dpTopoMiniportPins[] =
{
    // OA2DP_TOPO_PIN_IN (from the wave filter)
    {
        0,
        0,
        0,                                                  // InstanceCount
        NULL,                                               // AutomationTable
        {                                                   // KsPinDescriptor
            0,                                              // InterfacesCount
            NULL,                                           // Interfaces
            0,                                              // MediumsCount
            NULL,                                           // Mediums
            RTL_NUMBER_OF(Oa2dpTopoPinDataRangePointersBridge), // DataRangesCount
            Oa2dpTopoPinDataRangePointersBridge,            // DataRanges
            KSPIN_DATAFLOW_IN,                              // DataFlow
            KSPIN_COMMUNICATION_NONE,                       // Communication
            &KSCATEGORY_AUDIO,                              // Category
            NULL,                                           // Name
            0                                               // Reserved
        }
    },
    // OA2DP_TOPO_PIN_OUT (the headset)
    {
        0,
        0,
        0,                                                  // InstanceCount
        NULL,                                               // AutomationTable
        {                                                   // KsPinDescriptor
            0,                                              // InterfacesCount
            NULL,                                           // Interfaces
            0,                                              // MediumsCount
            NULL,                                           // Mediums
            RTL_NUMBER_OF(Oa2dpTopoPinDataRangePointersBridge), // DataRangesCount
            Oa2dpTopoPinDataRangePointersBridge,            // DataRanges
            KSPIN_DATAFLOW_OUT,                             // DataFlow
            KSPIN_COMMUNICATION_NONE,                       // Communication
            &KSNODETYPE_HEADPHONES,                         // Category
            NULL,                                           // Name
            0                                               // Reserved
        }
    }
};

static PCCONNECTION_DESCRIPTOR Oa2dpTopoMiniportConnections[] =
{
    { PCFILTER_NODE, OA2DP_TOPO_PIN_IN, PCFILTER_NODE, OA2DP_TOPO_PIN_OUT }
};

static PCFILTER_DESCRIPTOR Oa2dpTopoMiniportFilterDescriptor =
{
    0,                                              // Version
    NULL,                                           // AutomationTable
    sizeof(PCPIN_DESCRIPTOR),                       // PinSize
    RTL_NUMBER_OF(Oa2dpTopoMiniportPins),           // PinCount
    Oa2dpTopoMiniportPins,                          // Pins
    sizeof(PCNODE_DESCRIPTOR),                      // NodeSize
    0,                                              // NodeCount
    NULL,                                           // Nodes
    RTL_NUMBER_OF(Oa2dpTopoMiniportConnections),    // ConnectionCount
    Oa2dpTopoMiniportConnections,                   // Connections
    0,                                              // CategoryCount
    NULL                                            // Categories
};
