/* SPDX-License-Identifier: MIT */
/* Common include for the PortCls (C++) parts of oa2dpaudio.sys. */
#pragma once

#include <portcls.h>
#include <stdunk.h>
#include <ksmedia.h>
#include <ntstrsafe.h>

#include "newdelete.h"
#include "oa2dpaudio.h"

/* PcAddAdapterDevice reserves the first PORT_CLASS_DEVICE_EXTENSION_SIZE
 * bytes of the FDO extension for PortCls; ours follows. */
#define OA2DP_DEVICE(fdo) \
    ((OA2DP_AUDIO_DEVICE *)((PUCHAR)(fdo)->DeviceExtension + PORT_CLASS_DEVICE_EXTENSION_SIZE))

#define OA2DP_HNS_PER_SECOND 10000000ULL
#define OA2DP_HNS_PER_MS 10000LL

#ifndef SAFE_RELEASE
#define SAFE_RELEASE(p) \
    do {                \
        if (p) {        \
            (p)->Release(); \
            (p) = NULL; \
        }               \
    } while (0)
#endif

class CMiniportWaveRT;

NTSTATUS CreateMiniportWaveRT(_Out_ CMiniportWaveRT **Out, _In_ OA2DP_AUDIO_DEVICE *Device);
NTSTATUS CreateMiniportTopology(_Out_ PUNKNOWN *Out);
