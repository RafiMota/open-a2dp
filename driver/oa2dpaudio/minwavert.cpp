/*++

Copyright (c) Microsoft Corporation All Rights Reserved

Module Name:

    minwavert.cpp

Abstract:

    Implementation of wavert miniport.

--*/
/*
 * Derived from the SYSVAD sample (Windows-driver-samples/audio/sysvad,
 * commit 2dc3fd3a, MS-PL — see LICENSE.MS-PL in this folder). Modified for
 * OpenA2DP:
 *  - SYSVAD's static device-format tables are replaced by the list the
 *    service sends (IOCTL_OA2DP_AUDIO_SET_FORMATS, F4.2). IsFormatSupported,
 *    DataRangeIntersection and KSPROPERTY_PIN_PROPOSEDATAFORMAT(2) all answer
 *    from that list; float32 is offered first so 24-bit codecs get full
 *    precision.
 *  - A list change raises KSEVENT_PINCAPS_FORMATCHANGE, the mechanism SYSVAD
 *    uses for its Bluetooth HFP narrow/wide-band switch.
 *  - Offload, loopback, keyword detector, audio modules, DRM, effects and
 *    sideband code removed.
 */
#include "minwavert.h"
#include "minwavertstream.h"
#include "wavtable.h"

#ifndef VT_ILLEGAL
#define VT_ILLEGAL 0xffff
#endif

//=============================================================================
#pragma code_seg("PAGE")
NTSTATUS
CreateMiniportWaveRT
(
    _Out_   CMiniportWaveRT    **Out,
    _In_    OA2DP_AUDIO_DEVICE  *Device
)
{
    PAGED_CODE();

    CMiniportWaveRT *obj = new (POOL_FLAG_NON_PAGED, OA2DP_AUDIO_TAG) CMiniportWaveRT(NULL, Device);
    if (obj == NULL)
    {
        *Out = NULL;
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    obj->AddRef();
    *Out = obj;
    return STATUS_SUCCESS;
}

//=============================================================================
#pragma code_seg("PAGE")
CMiniportWaveRT::~CMiniportWaveRT
(
    void
)
{
    PAGED_CODE();
    SAFE_RELEASE(m_pPortEvents);
}

//=============================================================================
#pragma code_seg()
static ULONG ChannelMask(_In_ ULONG Channels)
{
    return Channels == 1 ? KSAUDIO_SPEAKER_MONO : KSAUDIO_SPEAKER_STEREO;
}

#pragma code_seg()
VOID Oa2dpBuildWaveFormat(_In_ const OA2DP_FORMAT *Format, _Out_ PKSDATAFORMAT_WAVEFORMATEXTENSIBLE Out)
{
    BOOLEAN isFloat = Format->BitsPerSample == 32;
    USHORT blockAlign = (USHORT)(Format->Channels * (Format->BitsPerSample / 8));

    RtlZeroMemory(Out, sizeof(*Out));
    Out->DataFormat.FormatSize = sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE);
    Out->DataFormat.Flags = 0;
    Out->DataFormat.SampleSize = blockAlign;
    Out->DataFormat.Reserved = 0;
    Out->DataFormat.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
    Out->DataFormat.SubFormat = isFloat ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;
    Out->DataFormat.Specifier = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;

    Out->WaveFormatExt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    Out->WaveFormatExt.Format.nChannels = Format->Channels;
    Out->WaveFormatExt.Format.nSamplesPerSec = Format->SampleRate;
    Out->WaveFormatExt.Format.nBlockAlign = blockAlign;
    Out->WaveFormatExt.Format.nAvgBytesPerSec = Format->SampleRate * blockAlign;
    Out->WaveFormatExt.Format.wBitsPerSample = Format->BitsPerSample;
    Out->WaveFormatExt.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    Out->WaveFormatExt.Samples.wValidBitsPerSample = Format->BitsPerSample;
    Out->WaveFormatExt.dwChannelMask = ChannelMask(Format->Channels);
    Out->WaveFormatExt.SubFormat = Out->DataFormat.SubFormat;
}

#pragma code_seg()
static VOID PreferredFormat(_In_ OA2DP_AUDIO_DEVICE *Device, _Out_ OA2DP_FORMAT *Format)
{
    OA2DP_FORMAT_LIST list;
    Oa2dpGetFormats(Device, &list);
    *Format = list.Formats[list.Preferred < list.Count ? list.Preferred : 0];
}

//=============================================================================
#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS)
CMiniportWaveRT::DataRangeIntersection
(
    _In_        ULONG                       PinId,
    _In_        PKSDATARANGE                ClientDataRange,
    _In_        PKSDATARANGE                MyDataRange,
    _In_        ULONG                       OutputBufferLength,
    _Out_writes_bytes_to_opt_(OutputBufferLength, *ResultantFormatLength)
                PVOID                       ResultantFormat,
    _Out_       PULONG                      ResultantFormatLength
)
/*++

Routine Description:

  Picks the service's preferred format if it fits the client range, else the
  first listed format that does. SYSVAD validated the channel count and let
  the class handler pick the maximum of the range; our range is only an
  envelope, so we build the result ourselves.

--*/
{
    ULONG               requiredSize = sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE);
    BOOLEAN             wantFloat;
    ULONG               maxChannels = MAXULONG, minBits = 0, maxBits = MAXULONG;
    ULONG               minRate = 0, maxRate = MAXULONG;
    OA2DP_FORMAT_LIST   list;

    PAGED_CODE();

    if (PinId != OA2DP_WAVE_PIN_SINK)
    {
        return STATUS_NOT_IMPLEMENTED;  // bridge pin: class handler
    }
    if (!IsEqualGUIDAligned(ClientDataRange->Specifier, KSDATAFORMAT_SPECIFIER_WAVEFORMATEX) &&
        !IsEqualGUIDAligned(ClientDataRange->Specifier, KSDATAFORMAT_SPECIFIER_WILDCARD))
    {
        return STATUS_NO_MATCH;
    }
    if (!IsEqualGUIDAligned(ClientDataRange->MajorFormat, KSDATAFORMAT_TYPE_AUDIO) &&
        !IsEqualGUIDAligned(ClientDataRange->MajorFormat, KSDATAFORMAT_TYPE_WILDCARD))
    {
        return STATUS_NO_MATCH;
    }
    if (!IsEqualGUIDAligned(ClientDataRange->SubFormat, MyDataRange->SubFormat) &&
        !IsEqualGUIDAligned(ClientDataRange->SubFormat, KSDATAFORMAT_SUBTYPE_WILDCARD))
    {
        return STATUS_NO_MATCH;
    }
    wantFloat = IsEqualGUIDAligned(MyDataRange->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);

    // Validate return buffer size; if the request is only for the size of
    // the resultant structure, return it now.
    if (!OutputBufferLength)
    {
        *ResultantFormatLength = requiredSize;
        return STATUS_BUFFER_OVERFLOW;
    }
    else if (OutputBufferLength < requiredSize)
    {
        return STATUS_BUFFER_TOO_SMALL;
    }

    if (ClientDataRange->FormatSize >= sizeof(KSDATARANGE_AUDIO))
    {
        PKSDATARANGE_AUDIO client = (PKSDATARANGE_AUDIO)ClientDataRange;
        maxChannels = client->MaximumChannels;
        minBits = client->MinimumBitsPerSample;
        maxBits = client->MaximumBitsPerSample;
        minRate = client->MinimumSampleFrequency;
        maxRate = client->MaximumSampleFrequency;
    }

    Oa2dpGetFormats(m_Device, &list);
    for (ULONG n = 0; n < list.Count; n++)
    {
        // Preferred first, then the others in list order.
        ULONG i = n == 0 ? list.Preferred : (n <= list.Preferred ? n - 1 : n);
        const OA2DP_FORMAT *f = &list.Formats[i];
        if ((f->BitsPerSample == 32) != wantFloat) continue;
        if (f->Channels > maxChannels) continue;
        if (f->BitsPerSample < minBits || f->BitsPerSample > maxBits) continue;
        if (f->SampleRate < minRate || f->SampleRate > maxRate) continue;

        Oa2dpBuildWaveFormat(f, (PKSDATAFORMAT_WAVEFORMATEXTENSIBLE)ResultantFormat);
        *ResultantFormatLength = requiredSize;
        return STATUS_SUCCESS;
    }
    return STATUS_NO_MATCH;
} // DataRangeIntersection

//=============================================================================
#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS)
CMiniportWaveRT::GetDescription
(
    _Out_ PPCFILTER_DESCRIPTOR * OutFilterDescriptor
)
{
    PAGED_CODE();
    ASSERT(OutFilterDescriptor);
    *OutFilterDescriptor = &Oa2dpWaveMiniportFilterDescriptor;
    return STATUS_SUCCESS;
} // GetDescription

//=============================================================================
#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS)
CMiniportWaveRT::Init
(
    _In_  PUNKNOWN                UnknownAdapter_,
    _In_  PRESOURCELIST           ResourceList_,
    _In_  PPORTWAVERT             Port_
)
{
    UNREFERENCED_PARAMETER(UnknownAdapter_);
    UNREFERENCED_PARAMETER(ResourceList_);
    PAGED_CODE();
    ASSERT(Port_);

    // For KS event support.
    if (!NT_SUCCESS(Port_->QueryInterface(IID_IPortEvents, (PVOID *)&m_pPortEvents)))
    {
        m_pPortEvents = NULL;
    }
    return STATUS_SUCCESS;
} // Init

//=============================================================================
#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS)
CMiniportWaveRT::NewStream
(
    _Out_ PMINIPORTWAVERTSTREAM * OutStream,
    _In_  PPORTWAVERTSTREAM       OuterUnknown,
    _In_  ULONG                   Pin,
    _In_  BOOLEAN                 Capture,
    _In_  PKSDATAFORMAT           DataFormat
)
{
    PAGED_CODE();
    ASSERT(OutStream);
    ASSERT(DataFormat);

    NTSTATUS                    ntStatus;
    PCMiniportWaveRTStream      stream = NULL;
    OA2DP_FORMAT                format;

    *OutStream = NULL;

    if (Capture || Pin != OA2DP_WAVE_PIN_SINK)
    {
        return STATUS_INVALID_PARAMETER;
    }

    // Determine if the format is valid.
    ntStatus = IsFormatSupported(DataFormat, &format);

    // Instantiate a stream. Stream must be in NonPagedPool(Nx): the position
    // timer runs at DISPATCH_LEVEL.
    if (NT_SUCCESS(ntStatus))
    {
        stream = new (POOL_FLAG_NON_PAGED, OA2DP_AUDIO_TAG) CMiniportWaveRTStream(NULL);
        if (stream)
        {
            stream->AddRef();
            ntStatus = stream->Init(this, OuterUnknown, &format);
        }
        else
        {
            ntStatus = STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    if (NT_SUCCESS(ntStatus))
    {
        *OutStream = PMINIPORTWAVERTSTREAM(stream);
        (*OutStream)->AddRef();
    }

    // This is our private reference to the stream. The caller has its own.
    if (stream)
    {
        stream->Release();
    }

    return ntStatus;
} // NewStream

//=============================================================================
#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS)
CMiniportWaveRT::NonDelegatingQueryInterface
(
    _In_ REFIID  Interface,
    _COM_Outptr_ PVOID * Object
)
{
    PAGED_CODE();
    ASSERT(Object);

    if (IsEqualGUIDAligned(Interface, IID_IUnknown))
    {
        *Object = PVOID(PUNKNOWN(PMINIPORTWAVERT(this)));
    }
    else if (IsEqualGUIDAligned(Interface, IID_IMiniport))
    {
        *Object = PVOID(PMINIPORT(this));
    }
    else if (IsEqualGUIDAligned(Interface, IID_IMiniportWaveRT))
    {
        *Object = PVOID(PMINIPORTWAVERT(this));
    }
    else
    {
        *Object = NULL;
    }

    if (*Object)
    {
        // We reference the interface for the caller.
        PUNKNOWN(*Object)->AddRef();
        return STATUS_SUCCESS;
    }

    return STATUS_INVALID_PARAMETER;
} // NonDelegatingQueryInterface

//=============================================================================
#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS) CMiniportWaveRT::GetDeviceDescription(_Out_ PDEVICE_DESCRIPTION DmaDeviceDescription)
{
    PAGED_CODE ();

    ASSERT (DmaDeviceDescription);

    RtlZeroMemory (DmaDeviceDescription, sizeof (DEVICE_DESCRIPTION));

    DmaDeviceDescription->Master = TRUE;
    DmaDeviceDescription->ScatterGather = TRUE;
    DmaDeviceDescription->Dma32BitAddresses = TRUE;
    DmaDeviceDescription->InterfaceType = PCIBus;
    DmaDeviceDescription->MaximumLength = 0xFFFFFFFF;

    return STATUS_SUCCESS;
}

//=============================================================================
#pragma code_seg("PAGE")
NTSTATUS
CMiniportWaveRT::IsFormatSupported
(
    _In_ PKSDATAFORMAT  _pDataFormat,
    _Out_opt_ OA2DP_FORMAT *Format
)
{
    PAGED_CODE();

    PWAVEFORMATEX       pWaveFormat;
    BOOLEAN             isFloat;
    OA2DP_FORMAT        f;
    OA2DP_FORMAT_LIST   list;

    // KSDATAFORMAT VALIDATION
    if (_pDataFormat->FormatSize < sizeof(KSDATAFORMAT_WAVEFORMATEX)) { return STATUS_NO_MATCH; }
    if (!IsEqualGUIDAligned(_pDataFormat->MajorFormat, KSDATAFORMAT_TYPE_AUDIO)) { return STATUS_NO_MATCH; }
    if (!IsEqualGUIDAligned(_pDataFormat->Specifier, KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)) { return STATUS_NO_MATCH; }

    // WAVEFORMATEX VALIDATION
    pWaveFormat = reinterpret_cast<PWAVEFORMATEX>(_pDataFormat + 1);
    switch (pWaveFormat->wFormatTag)
    {
    case WAVE_FORMAT_PCM:
        isFloat = FALSE;
        break;
    case WAVE_FORMAT_IEEE_FLOAT:
        isFloat = TRUE;
        break;
    case WAVE_FORMAT_EXTENSIBLE:
    {
        // WAVEFORMATEXTENSIBLE VALIDATION
        if (_pDataFormat->FormatSize < sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE)) { return STATUS_NO_MATCH; }
        if (pWaveFormat->cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) { return STATUS_NO_MATCH; }
        PWAVEFORMATEXTENSIBLE pWaveFormatExt = reinterpret_cast<PWAVEFORMATEXTENSIBLE>(pWaveFormat);
        if (IsEqualGUIDAligned(pWaveFormatExt->SubFormat, KSDATAFORMAT_SUBTYPE_PCM)) { isFloat = FALSE; }
        else if (IsEqualGUIDAligned(pWaveFormatExt->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) { isFloat = TRUE; }
        else { return STATUS_NO_MATCH; }
        if (pWaveFormatExt->Samples.wValidBitsPerSample != pWaveFormat->wBitsPerSample) { return STATUS_NO_MATCH; }
        if (pWaveFormatExt->dwChannelMask != 0 &&
            pWaveFormatExt->dwChannelMask != ChannelMask(pWaveFormat->nChannels)) { return STATUS_NO_MATCH; }
        break;
    }
    default:
        return STATUS_NO_MATCH;
    }

    if (isFloat != (pWaveFormat->wBitsPerSample == 32)) { return STATUS_NO_MATCH; }
    if (pWaveFormat->nChannels == 0) { return STATUS_NO_MATCH; }
    if (pWaveFormat->nBlockAlign != pWaveFormat->nChannels * (pWaveFormat->wBitsPerSample / 8)) { return STATUS_NO_MATCH; }
    if (pWaveFormat->nAvgBytesPerSec != pWaveFormat->nSamplesPerSec * pWaveFormat->nBlockAlign) { return STATUS_NO_MATCH; }

    f.SampleRate = pWaveFormat->nSamplesPerSec;
    f.Channels = pWaveFormat->nChannels;
    f.BitsPerSample = pWaveFormat->wBitsPerSample;

    Oa2dpGetFormats(m_Device, &list);
    if (!Oa2dpFormatListed(&list, &f))
    {
        return STATUS_NO_MATCH;
    }
    if (Format)
    {
        *Format = f;
    }
    return STATUS_SUCCESS;
}

//=============================================================================
#pragma code_seg()
VOID
CMiniportWaveRT::FormatsChanged()
{
    if (m_pPortEvents != NULL)
    {
        // Same notification as SYSVAD's EvtFormatChangeHandler.
        m_pPortEvents->GenerateEventList(
            (GUID *)&KSEVENTSETID_PinCapsChange,
            KSEVENT_PINCAPS_FORMATCHANGE,
            TRUE,
            OA2DP_WAVE_PIN_SINK,
            FALSE,
            ULONG(-1));
    }
}

//=============================================================================
#pragma code_seg("PAGE")
static NTSTATUS
PropertyHandler_BasicSupport
(
    _In_ PPCPROPERTY_REQUEST PropertyRequest
)
{
    PAGED_CODE();

    ULONG flags = PropertyRequest->PropertyItem->Flags;

    if (PropertyRequest->ValueSize >= sizeof(KSPROPERTY_DESCRIPTION))
    {
        PKSPROPERTY_DESCRIPTION desc = (PKSPROPERTY_DESCRIPTION)PropertyRequest->Value;
        desc->AccessFlags = flags;
        desc->DescriptionSize = sizeof(KSPROPERTY_DESCRIPTION);
        desc->PropTypeSet.Set = KSPROPTYPESETID_General;
        desc->PropTypeSet.Id = VT_ILLEGAL;
        desc->PropTypeSet.Flags = 0;
        desc->MembersListCount = 0;
        desc->Reserved = 0;
        PropertyRequest->ValueSize = sizeof(KSPROPERTY_DESCRIPTION);
        return STATUS_SUCCESS;
    }
    if (PropertyRequest->ValueSize >= sizeof(ULONG))
    {
        *(PULONG)PropertyRequest->Value = flags;
        PropertyRequest->ValueSize = sizeof(ULONG);
        return STATUS_SUCCESS;
    }
    PropertyRequest->ValueSize = 0;
    return STATUS_BUFFER_TOO_SMALL;
}

// Both pin properties need a KSP_PIN that names the streaming pin.
#pragma code_seg("PAGE")
static NTSTATUS
GetStreamPin
(
    _In_  PPCPROPERTY_REQUEST PropertyRequest,
    _Out_ PKSP_PIN           *Pin
)
{
    PAGED_CODE();

    *Pin = NULL;
    if (PropertyRequest->InstanceSize < (sizeof(KSP_PIN) - RTL_SIZEOF_THROUGH_FIELD(KSP_PIN, Property)))
    {
        return STATUS_INVALID_PARAMETER;
    }
    *Pin = CONTAINING_RECORD(PropertyRequest->Instance, KSP_PIN, PinId);
    if ((*Pin)->PinId == OA2DP_WAVE_PIN_BRIDGE)
    {
        return STATUS_NOT_SUPPORTED;
    }
    if ((*Pin)->PinId != OA2DP_WAVE_PIN_SINK)
    {
        return STATUS_INVALID_PARAMETER;
    }
    return STATUS_SUCCESS;
}

//=============================================================================
#pragma code_seg("PAGE")
NTSTATUS
CMiniportWaveRT::PropertyHandlerProposedFormat
(
    _In_ PPCPROPERTY_REQUEST      PropertyRequest
)
{
    PKSP_PIN    kspPin = NULL;
    NTSTATUS    ntStatus;
    ULONG       cbMinSize = sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE);

    PAGED_CODE();

    ntStatus = GetStreamPin(PropertyRequest, &kspPin);
    if (!NT_SUCCESS(ntStatus))
    {
        return ntStatus;
    }

    if (PropertyRequest->Verb & KSPROPERTY_TYPE_BASICSUPPORT)
    {
        return PropertyHandler_BasicSupport(PropertyRequest);
    }

    if (PropertyRequest->Verb & KSPROPERTY_TYPE_GET)
    {
        // The default format: what the service prefers for the codec.
        OA2DP_FORMAT format;
        if (PropertyRequest->ValueSize == 0)
        {
            PropertyRequest->ValueSize = cbMinSize;
            return STATUS_BUFFER_OVERFLOW;
        }
        if (PropertyRequest->ValueSize < cbMinSize)
        {
            return STATUS_BUFFER_TOO_SMALL;
        }
        PreferredFormat(m_Device, &format);
        Oa2dpBuildWaveFormat(&format, (PKSDATAFORMAT_WAVEFORMATEXTENSIBLE)PropertyRequest->Value);
        PropertyRequest->ValueSize = cbMinSize;
        return STATUS_SUCCESS;
    }

    if (PropertyRequest->Verb & KSPROPERTY_TYPE_SET)
    {
        PKSDATAFORMAT pKsFormat = (PKSDATAFORMAT)PropertyRequest->Value;
        if (PropertyRequest->ValueSize < sizeof(KSDATAFORMAT_WAVEFORMATEX) ||
            pKsFormat->FormatSize > PropertyRequest->ValueSize)
        {
            return STATUS_BUFFER_TOO_SMALL;
        }
        return IsFormatSupported(pKsFormat, NULL);
    }

    return STATUS_INVALID_DEVICE_REQUEST;
} // PropertyHandlerProposedFormat

//=============================================================================
#pragma code_seg("PAGE")
NTSTATUS
CMiniportWaveRT::PropertyHandlerProposedFormat2
(
    _In_ PPCPROPERTY_REQUEST      PropertyRequest
)
/*++

  Returns the default format for the requested processing mode. We have one
  default for every mode (the service's preferred format); the attribute
  list the caller sent is copied back after it, as in SYSVAD.

--*/
{
    PKSP_PIN                kspPin = NULL;
    NTSTATUS                ntStatus;
    PKSMULTIPLE_ITEM        pKsItemsHeader;
    PBYTE                   pEnd;
    ULONG                   cbItemsList = 0;
    ULONG                   cbFormat = sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE);
    ULONG                   cbFormatAligned = (cbFormat + 7) & ~7UL;
    ULONG                   cbMinSize;
    OA2DP_FORMAT            format;

    PAGED_CODE();

    ntStatus = GetStreamPin(PropertyRequest, &kspPin);
    if (!NT_SUCCESS(ntStatus))
    {
        return ntStatus;
    }

    if (PropertyRequest->Verb & KSPROPERTY_TYPE_BASICSUPPORT)
    {
        return PropertyHandler_BasicSupport(PropertyRequest);
    }

    // Only GET is supported for this property
    if ((PropertyRequest->Verb & KSPROPERTY_TYPE_GET) == 0)
    {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    // Optional attribute list (signal processing mode) after the KSP_PIN.
    pKsItemsHeader = (PKSMULTIPLE_ITEM)(kspPin + 1);
    pEnd = ((PBYTE)PropertyRequest->Instance) + PropertyRequest->InstanceSize;
    if ((PBYTE)pKsItemsHeader + sizeof(KSMULTIPLE_ITEM) <= pEnd)
    {
        ULONG available = (ULONG)(pEnd - (PBYTE)pKsItemsHeader);
        if (pKsItemsHeader->Size < sizeof(KSMULTIPLE_ITEM) || pKsItemsHeader->Size > available)
        {
            return STATUS_INVALID_PARAMETER;
        }
        cbItemsList = pKsItemsHeader->Size;
    }

    cbMinSize = cbItemsList ? cbFormatAligned + cbItemsList : cbFormat;

    if (PropertyRequest->ValueSize == 0)
    {
        PropertyRequest->ValueSize = cbMinSize;
        return STATUS_BUFFER_OVERFLOW;
    }
    if (PropertyRequest->ValueSize < cbMinSize)
    {
        return STATUS_BUFFER_TOO_SMALL;
    }

    PreferredFormat(m_Device, &format);
    Oa2dpBuildWaveFormat(&format, (PKSDATAFORMAT_WAVEFORMATEXTENSIBLE)PropertyRequest->Value);
    if (cbItemsList)
    {
        // Copy back the attribute list.
        ((KSDATAFORMAT *)PropertyRequest->Value)->Flags = KSDATAFORMAT_ATTRIBUTES;
        RtlCopyMemory((PBYTE)PropertyRequest->Value + cbFormatAligned, pKsItemsHeader, cbItemsList);
    }
    PropertyRequest->ValueSize = cbMinSize;
    return STATUS_SUCCESS;
} // PropertyHandlerProposedFormat2

//=============================================================================
#pragma code_seg()
NTSTATUS
CMiniportWaveRT::EventHandlerPinCapsChange
(
    _In_  PPCEVENT_REQUEST EventRequest
)
{
    if (*EventRequest->EventItem->Set != KSEVENTSETID_PinCapsChange)
    {
        return STATUS_INVALID_PARAMETER;
    }

    switch (EventRequest->Verb)
    {
        // Do we support event handling?!?
    case PCEVENT_VERB_SUPPORT:
        break;

        // We should add the event now!
    case PCEVENT_VERB_ADD:
        if (EventRequest->EventEntry && m_pPortEvents)
        {
            switch (EventRequest->EventItem->Id)
            {
            case KSEVENT_PINCAPS_FORMATCHANGE:
                m_pPortEvents->AddEventToEventList(EventRequest->EventEntry);
                break;
            default:
                return STATUS_INVALID_PARAMETER;
            }
        }
        else
        {
            return STATUS_UNSUCCESSFUL;
        }
        break;

    case PCEVENT_VERB_REMOVE:
        // We cannot remove the event but we can stop generating the
        // events. However, it also doesn't hurt to always generate them ...
        break;

    default:
        return STATUS_INVALID_PARAMETER;
    }

    return STATUS_SUCCESS;
}

//=============================================================================
#pragma code_seg("PAGE")
NTSTATUS
PropertyHandler_WaveFilter
(
    _In_ PPCPROPERTY_REQUEST      PropertyRequest
)
/*++

Routine Description:

  Redirects general property request to miniport object

--*/
{
    PAGED_CODE();

    NTSTATUS            ntStatus = STATUS_INVALID_DEVICE_REQUEST;
    CMiniportWaveRT*    pWaveHelper = reinterpret_cast<CMiniportWaveRT*>(PropertyRequest->MajorTarget);

    if (pWaveHelper == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    pWaveHelper->AddRef();

    if (IsEqualGUIDAligned(*PropertyRequest->PropertyItem->Set, KSPROPSETID_Pin))
    {
        switch (PropertyRequest->PropertyItem->Id)
        {
            case KSPROPERTY_PIN_PROPOSEDATAFORMAT:
                ntStatus = pWaveHelper->PropertyHandlerProposedFormat(PropertyRequest);
                break;

            case KSPROPERTY_PIN_PROPOSEDATAFORMAT2:
                ntStatus = pWaveHelper->PropertyHandlerProposedFormat2(PropertyRequest);
                break;

            default:
                break;
        }
    }

    pWaveHelper->Release();

    return ntStatus;
} // PropertyHandler_WaveFilter

//=============================================================================
#pragma code_seg()
NTSTATUS
EventHandler_PinCapsChange
(
    _In_  PPCEVENT_REQUEST EventRequest
)
{
    CMiniportWaveRT* miniport = reinterpret_cast<CMiniportWaveRT*>(EventRequest->MajorTarget);
    return miniport->EventHandlerPinCapsChange(EventRequest);
}

//=============================================================================
#pragma code_seg("PAGE")
extern "C" VOID Oa2dpWaveFormatsChanged(_Inout_ OA2DP_AUDIO_DEVICE *dev)
{
    PAGED_CODE();
    Oa2dpLock(&dev->FilterLock);
    if (dev->WaveMiniport)
    {
        static_cast<CMiniportWaveRT *>(dev->WaveMiniport)->FormatsChanged();
    }
    Oa2dpUnlock(&dev->FilterLock);
}
#pragma code_seg()
