/*++

Copyright (c) Microsoft Corporation All Rights Reserved

Module Name:

    mintopo.cpp

Abstract:

    Implementation of topology miniport.

--*/
/*
 * Derived from the SYSVAD sample (Windows-driver-samples/audio/sysvad,
 * commit 2dc3fd3a, MS-PL — see LICENSE.MS-PL in this folder): mintopo.cpp
 * and basetopo.cpp. Modified for OpenA2DP: a fixed two-pin topology
 * (wave bridge in, headphones out) with no nodes; volume and mute are left
 * to the audio engine's software endpoint volume.
 */
#include "driver.h"
#include "toptable.h"

class CMiniportTopology : public IMiniportTopology, public CUnknown
{
public:
    DECLARE_STD_UNKNOWN();
    DEFINE_STD_CONSTRUCTOR(CMiniportTopology);
    ~CMiniportTopology() {}

    IMP_IMiniportTopology;
};

#pragma code_seg("PAGE")
NTSTATUS CreateMiniportTopology(_Out_ PUNKNOWN *Out)
{
    PAGED_CODE();

    CMiniportTopology *obj = new (POOL_FLAG_NON_PAGED, OA2DP_AUDIO_TAG) CMiniportTopology(NULL);
    if (obj == NULL)
    {
        *Out = NULL;
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    obj->AddRef();
    *Out = PUNKNOWN(PMINIPORTTOPOLOGY(obj));
    return STATUS_SUCCESS;
}

#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS)
CMiniportTopology::NonDelegatingQueryInterface
(
    _In_ REFIID Interface,
    _COM_Outptr_ PVOID *Object
)
{
    PAGED_CODE();
    ASSERT(Object);

    if (IsEqualGUIDAligned(Interface, IID_IUnknown))
    {
        *Object = PVOID(PUNKNOWN(PMINIPORTTOPOLOGY(this)));
    }
    else if (IsEqualGUIDAligned(Interface, IID_IMiniport))
    {
        *Object = PVOID(PMINIPORT(this));
    }
    else if (IsEqualGUIDAligned(Interface, IID_IMiniportTopology))
    {
        *Object = PVOID(PMINIPORTTOPOLOGY(this));
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
}

#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS)
CMiniportTopology::Init
(
    _In_ PUNKNOWN       UnknownAdapter,
    _In_ PRESOURCELIST  ResourceList,
    _In_ PPORTTOPOLOGY  Port
)
{
    UNREFERENCED_PARAMETER(UnknownAdapter);
    UNREFERENCED_PARAMETER(ResourceList);
    UNREFERENCED_PARAMETER(Port);
    PAGED_CODE();
    return STATUS_SUCCESS;
}

#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS)
CMiniportTopology::GetDescription
(
    _Out_ PPCFILTER_DESCRIPTOR *OutFilterDescriptor
)
{
    PAGED_CODE();
    ASSERT(OutFilterDescriptor);
    *OutFilterDescriptor = &Oa2dpTopoMiniportFilterDescriptor;
    return STATUS_SUCCESS;
}

#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS)
CMiniportTopology::DataRangeIntersection
(
    _In_        ULONG           PinId,
    _In_        PKSDATARANGE    ClientDataRange,
    _In_        PKSDATARANGE    MyDataRange,
    _In_        ULONG           OutputBufferLength,
    _Out_writes_bytes_to_opt_(OutputBufferLength, *ResultantFormatLength)
                PVOID           ResultantFormat,
    _Out_       PULONG          ResultantFormatLength
)
{
    UNREFERENCED_PARAMETER(PinId);
    UNREFERENCED_PARAMETER(ClientDataRange);
    UNREFERENCED_PARAMETER(MyDataRange);
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(ResultantFormat);
    UNREFERENCED_PARAMETER(ResultantFormatLength);
    PAGED_CODE();
    // Bridge pins only: let the class handler do it.
    return STATUS_NOT_IMPLEMENTED;
}
#pragma code_seg()
