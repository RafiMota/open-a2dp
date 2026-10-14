/* SPDX-License-Identifier: MIT */
/* L2CAP channel management through Bluetooth Request Blocks (BRBs). */
#include "oa2dp.h"

static OA2DP_CHANNEL *ChannelByHandle(OA2DP_DEVICE_CONTEXT *dev, L2CAP_CHANNEL_HANDLE h)
{
    for (ULONG i = 0; i < Oa2dpChannelCount; i++)
        if (dev->Channels[i].State != ChannelClosed && dev->Channels[i].Handle == h)
            return &dev->Channels[i];
    return NULL;
}

/* ------------------------------------------------------------------------ */
/* Submission helpers                                                       */
/* ------------------------------------------------------------------------ */

NTSTATUS Oa2dpSubmitBrbSync(_In_ OA2DP_DEVICE_CONTEXT *dev, _In_ struct _BRB *brb, _In_ ULONG size)
{
    WDF_MEMORY_DESCRIPTOR desc;
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&desc, brb, size);
    return WdfIoTargetSendInternalIoctlOthersSynchronously(
        dev->IoTarget, NULL, IOCTL_INTERNAL_BTH_SUBMIT_BRB, &desc, NULL, NULL, NULL, NULL);
}

static NTSTATUS SubmitBrbAsync(_In_ OA2DP_DEVICE_CONTEXT *dev, _In_ WDFREQUEST request,
                               _In_ struct _BRB *brb, _In_ ULONG size,
                               _In_ PFN_WDF_REQUEST_COMPLETION_ROUTINE done, _In_ PVOID ctx)
{
    NTSTATUS status;
    WDFMEMORY mem;
    WDF_OBJECT_ATTRIBUTES attrs;

    WDF_OBJECT_ATTRIBUTES_INIT(&attrs);
    attrs.ParentObject = request;
    status = WdfMemoryCreatePreallocated(&attrs, brb, size, &mem);
    if (!NT_SUCCESS(status))
        return status;
    status = WdfIoTargetFormatRequestForInternalIoctlOthers(
        dev->IoTarget, request, IOCTL_INTERNAL_BTH_SUBMIT_BRB, mem, NULL, NULL, NULL, NULL, NULL);
    if (!NT_SUCCESS(status))
        return status;
    WdfRequestSetCompletionRoutine(request, done, ctx);
    if (!WdfRequestSend(request, dev->IoTarget, WDF_NO_SEND_OPTIONS))
        return WdfRequestGetStatus(request);
    return STATUS_PENDING;
}

/* ------------------------------------------------------------------------ */
/* Addresses                                                                */
/* ------------------------------------------------------------------------ */

NTSTATUS Oa2dpQueryLocalAddress(_In_ OA2DP_DEVICE_CONTEXT *dev)
{
    struct _BRB_GET_LOCAL_BD_ADDR *brb = (struct _BRB_GET_LOCAL_BD_ADDR *)
        dev->Profile.BthAllocateBrb(BRB_HCI_GET_LOCAL_BD_ADDR, OA2DP_POOL_TAG);
    NTSTATUS status;
    if (!brb)
        return STATUS_INSUFFICIENT_RESOURCES;
    status = Oa2dpSubmitBrbSync(dev, (struct _BRB *)brb, sizeof(*brb));
    if (NT_SUCCESS(status))
        dev->LocalAddress = brb->BtAddress;
    dev->Profile.BthFreeBrb((PBRB)brb);
    return status;
}

NTSTATUS Oa2dpQueryRemoteAddress(_In_ OA2DP_DEVICE_CONTEXT *dev)
{
    BTH_DEVICE_INFO info;
    WDF_MEMORY_DESCRIPTOR out;
    NTSTATUS status;
    RtlZeroMemory(&info, sizeof(info));
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&out, &info, sizeof(info));
    /* BTHENUM tells a profile driver which remote device it serves. */
    status = WdfIoTargetSendInternalIoctlSynchronously(
        dev->IoTarget, NULL, IOCTL_INTERNAL_BTHENUM_GET_DEVINFO, NULL, &out, NULL, NULL);
    if (NT_SUCCESS(status))
        dev->RemoteAddress = info.address;
    return status;
}

/* ------------------------------------------------------------------------ */
/* Indications (remote connect / disconnect)                                */
/* ------------------------------------------------------------------------ */

static EVT_WDF_WORKITEM CloseWorkItem;
static EVT_WDF_REQUEST_COMPLETION_ROUTINE OpenResponseDone;

static void FillChannelConfig(struct _BRB_L2CA_OPEN_CHANNEL *brb, OA2DP_CHANNEL *ch, USHORT inMtu)
{
    brb->ChannelFlags = CF_ROLE_EITHER;
    brb->ConfigIn.Flags = CFG_MTU;
    brb->ConfigIn.Mtu.Max = inMtu;
    brb->ConfigIn.Mtu.Min = 48;
    brb->ConfigIn.Mtu.Preferred = inMtu;
    brb->ConfigOut.Flags = 0; /* accept whatever the headset can receive */
    brb->CallbackFlags = CALLBACK_DISCONNECT;
    brb->Callback = (PFNBTHPORT_INDICATION_CALLBACK)NULL; /* set by caller */
    brb->CallbackContext = ch;
    brb->ReferenceObject = WdfDeviceWdmGetDeviceObject(ch->Dev->Device);
    brb->IncomingQueueDepth = OA2DP_INCOMING_QUEUE_DEPTH;
}

static void IndicationCallback(_In_ PVOID Context, _In_ INDICATION_CODE Indication,
                               _In_ PINDICATION_PARAMETERS Params);

static void AcceptIncoming(OA2DP_DEVICE_CONTEXT *dev, PINDICATION_PARAMETERS p)
{
    OA2DP_CHANNEL *ch = NULL;
    struct _BRB_L2CA_OPEN_CHANNEL *brb;
    WDFREQUEST req;
    WDF_OBJECT_ATTRIBUTES attrs;

    WdfSpinLockAcquire(dev->Lock);
    /* First channel from the headset is signalling, the second is media. */
    if (dev->Channels[Oa2dpChannelSignaling].State == ChannelClosed)
        ch = &dev->Channels[Oa2dpChannelSignaling];
    else if (dev->Channels[Oa2dpChannelMedia].State == ChannelClosed)
        ch = &dev->Channels[Oa2dpChannelMedia];
    if (ch)
        ch->State = ChannelOpening;
    WdfSpinLockRelease(dev->Lock);

    brb = (struct _BRB_L2CA_OPEN_CHANNEL *)dev->Profile.BthAllocateBrb(
        BRB_L2CA_OPEN_CHANNEL_RESPONSE, OA2DP_POOL_TAG);
    if (!brb)
        goto fail_state;
    brb->BtAddress = p->BtAddress;
    brb->Psm = p->Parameters.Connect.Request.PSM;
    brb->ChannelHandle = p->ConnectionHandle;
    brb->Response = ch ? CONNECT_RSP_RESULT_SUCCESS : CONNECT_RSP_RESULT_NO_RESOURCES;
    if (ch) {
        FillChannelConfig(brb, ch, ch->Kind == Oa2dpChannelMedia ? OA2DP_DEFAULT_MEDIA_MTU
                                                                 : OA2DP_DEFAULT_SIGNALING_MTU);
        brb->Callback = IndicationCallback;
    }

    WDF_OBJECT_ATTRIBUTES_INIT(&attrs);
    attrs.ParentObject = dev->Device;
    if (!NT_SUCCESS(WdfRequestCreate(&attrs, dev->IoTarget, &req)))
        goto fail_brb;
    Oa2dpGetRequest(req)->Brb = (struct _BRB *)brb;
    Oa2dpGetRequest(req)->Channel = ch;
    if (SubmitBrbAsync(dev, req, (struct _BRB *)brb, sizeof(*brb), OpenResponseDone, dev) !=
        STATUS_PENDING) {
        WdfObjectDelete(req);
        goto fail_brb;
    }
    return;

fail_brb:
    dev->Profile.BthFreeBrb((PBRB)brb);
fail_state:
    if (ch) {
        WdfSpinLockAcquire(dev->Lock);
        ch->State = ChannelClosed;
        WdfSpinLockRelease(dev->Lock);
    }
}

static void OpenResponseDone(_In_ WDFREQUEST Request, _In_ WDFIOTARGET Target,
                             _In_ PWDF_REQUEST_COMPLETION_PARAMS Params, _In_ WDFCONTEXT Context)
{
    OA2DP_DEVICE_CONTEXT *dev = (OA2DP_DEVICE_CONTEXT *)Context;
    OA2DP_REQUEST_CONTEXT *rc = Oa2dpGetRequest(Request);
    struct _BRB_L2CA_OPEN_CHANNEL *brb = (struct _BRB_L2CA_OPEN_CHANNEL *)rc->Brb;
    OA2DP_CHANNEL *ch = rc->Channel;
    UNREFERENCED_PARAMETER(Target);

    if (ch) {
        WdfSpinLockAcquire(dev->Lock);
        if (NT_SUCCESS(Params->IoStatus.Status)) {
            ch->State = ChannelOpen;
            ch->Handle = brb->ChannelHandle;
            ch->OutMtu = brb->OutResults.Params.Mtu;
            ch->InMtu = brb->InResults.Params.Mtu;
        } else {
            ch->State = ChannelClosed;
        }
        WdfSpinLockRelease(dev->Lock);
        if (NT_SUCCESS(Params->IoStatus.Status))
            Oa2dpPostEvent(dev, Oa2dpEventChannelOpened, ch);
    }
    dev->Profile.BthFreeBrb((PBRB)brb);
    WdfObjectDelete(Request);
}

static void IndicationCallback(_In_ PVOID Context, _In_ INDICATION_CODE Indication,
                               _In_ PINDICATION_PARAMETERS Params)
{
    /* Context is the device for server indications and the channel for
     * per-channel indications; both start with a pointer we can test. */
    switch (Indication) {
    case IndicationRemoteConnect: {
        OA2DP_DEVICE_CONTEXT *dev = (OA2DP_DEVICE_CONTEXT *)Context;
        if (Params->Parameters.Connect.Request.PSM == OA2DP_AVDTP_PSM &&
            Params->BtAddress == dev->RemoteAddress)
            AcceptIncoming(dev, Params);
        break;
    }
    case IndicationRemoteDisconnect: {
        OA2DP_CHANNEL *ch = (OA2DP_CHANNEL *)Context;
        OA2DP_DEVICE_CONTEXT *dev = ch->Dev;
        WDF_WORKITEM_CONFIG cfg;
        WDF_OBJECT_ATTRIBUTES attrs;
        WDFWORKITEM wi;
        WdfSpinLockAcquire(dev->Lock);
        if (ch->State == ChannelOpen)
            ch->State = ChannelClosing;
        WdfSpinLockRelease(dev->Lock);
        /* Closing the handle needs PASSIVE_LEVEL; do it from a work item. */
        WDF_WORKITEM_CONFIG_INIT(&cfg, CloseWorkItem);
        WDF_OBJECT_ATTRIBUTES_INIT(&attrs);
        attrs.ParentObject = dev->Device;
        WDF_OBJECT_ATTRIBUTES_SET_CONTEXT_TYPE(&attrs, OA2DP_WORKITEM_CONTEXT);
        if (NT_SUCCESS(WdfWorkItemCreate(&cfg, &attrs, &wi))) {
            Oa2dpGetWorkItem(wi)->Channel = ch;
            WdfWorkItemEnqueue(wi);
        }
        break;
    }
    default:
        break;
    }
}

static VOID CloseWorkItem(_In_ WDFWORKITEM WorkItem)
{
    OA2DP_CHANNEL *ch = Oa2dpGetWorkItem(WorkItem)->Channel;
    (void)Oa2dpCloseChannel(ch->Dev, ch->Kind);
    WdfObjectDelete(WorkItem);
}

/* ------------------------------------------------------------------------ */
/* Server registration                                                      */
/* ------------------------------------------------------------------------ */

NTSTATUS Oa2dpRegisterServer(_In_ OA2DP_DEVICE_CONTEXT *dev)
{
    NTSTATUS status;
    struct _BRB_PSM *psm = (struct _BRB_PSM *)dev->Profile.BthAllocateBrb(BRB_REGISTER_PSM,
                                                                          OA2DP_POOL_TAG);
    if (!psm)
        return STATUS_INSUFFICIENT_RESOURCES;
    psm->Psm = OA2DP_AVDTP_PSM;
    status = Oa2dpSubmitBrbSync(dev, (struct _BRB *)psm, sizeof(*psm));
    dev->Profile.BthFreeBrb((PBRB)psm);
    if (!NT_SUCCESS(status))
        return status;
    dev->PsmRegistered = TRUE;

    struct _BRB_L2CA_REGISTER_SERVER *srv = (struct _BRB_L2CA_REGISTER_SERVER *)
        dev->Profile.BthAllocateBrb(BRB_L2CA_REGISTER_SERVER, OA2DP_POOL_TAG);
    if (!srv)
        return STATUS_INSUFFICIENT_RESOURCES;
    srv->BtAddress = dev->RemoteAddress; /* only our headset */
    srv->PSM = 0;                        /* every PSM we registered */
    srv->IndicationCallback = IndicationCallback;
    srv->IndicationCallbackContext = dev;
    srv->IndicationFlags = 0;
    srv->ReferenceObject = WdfDeviceWdmGetDeviceObject(dev->Device);
    status = Oa2dpSubmitBrbSync(dev, (struct _BRB *)srv, sizeof(*srv));
    if (NT_SUCCESS(status))
        dev->ServerHandle = srv->ServerHandle;
    dev->Profile.BthFreeBrb((PBRB)srv);
    return status;
}

VOID Oa2dpUnregisterServer(_In_ OA2DP_DEVICE_CONTEXT *dev)
{
    if (dev->ServerHandle) {
        struct _BRB_L2CA_UNREGISTER_SERVER *b = (struct _BRB_L2CA_UNREGISTER_SERVER *)
            dev->Profile.BthAllocateBrb(BRB_L2CA_UNREGISTER_SERVER, OA2DP_POOL_TAG);
        if (b) {
            b->BtAddress = dev->RemoteAddress;
            b->Psm = 0;
            b->ServerHandle = dev->ServerHandle;
            (void)Oa2dpSubmitBrbSync(dev, (struct _BRB *)b, sizeof(*b));
            dev->Profile.BthFreeBrb((PBRB)b);
        }
        dev->ServerHandle = NULL;
    }
    if (dev->PsmRegistered) {
        struct _BRB_PSM *psm = (struct _BRB_PSM *)dev->Profile.BthAllocateBrb(
            BRB_UNREGISTER_PSM, OA2DP_POOL_TAG);
        if (psm) {
            psm->Psm = OA2DP_AVDTP_PSM;
            (void)Oa2dpSubmitBrbSync(dev, (struct _BRB *)psm, sizeof(*psm));
            dev->Profile.BthFreeBrb((PBRB)psm);
        }
        dev->PsmRegistered = FALSE;
    }
}

/* ------------------------------------------------------------------------ */
/* Outgoing open / close                                                    */
/* ------------------------------------------------------------------------ */

static EVT_WDF_REQUEST_COMPLETION_ROUTINE OpenDone;

VOID Oa2dpOpenChannel(_In_ OA2DP_DEVICE_CONTEXT *dev, _In_ WDFREQUEST request,
                      _In_ const OA2DP_OPEN_CHANNEL_IN *in)
{
    OA2DP_CHANNEL *ch = &dev->Channels[in->Kind];
    struct _BRB_L2CA_OPEN_CHANNEL *brb;
    WDFREQUEST lower;
    WDF_OBJECT_ATTRIBUTES attrs;
    NTSTATUS status;
    USHORT mtu = in->InMtu ? in->InMtu
                           : (in->Kind == Oa2dpChannelMedia ? OA2DP_DEFAULT_MEDIA_MTU
                                                            : OA2DP_DEFAULT_SIGNALING_MTU);

    WdfSpinLockAcquire(dev->Lock);
    if (ch->State != ChannelClosed) {
        WdfSpinLockRelease(dev->Lock);
        WdfRequestComplete(request, STATUS_DEVICE_BUSY);
        return;
    }
    ch->State = ChannelOpening;
    ch->OpenRequest = request;
    WdfSpinLockRelease(dev->Lock);

    brb = (struct _BRB_L2CA_OPEN_CHANNEL *)dev->Profile.BthAllocateBrb(BRB_L2CA_OPEN_CHANNEL,
                                                                       OA2DP_POOL_TAG);
    if (!brb) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto fail;
    }
    brb->BtAddress = dev->RemoteAddress;
    brb->Psm = OA2DP_AVDTP_PSM;
    FillChannelConfig(brb, ch, mtu);
    brb->Callback = IndicationCallback;

    WDF_OBJECT_ATTRIBUTES_INIT(&attrs);
    attrs.ParentObject = dev->Device;
    status = WdfRequestCreate(&attrs, dev->IoTarget, &lower);
    if (!NT_SUCCESS(status)) {
        dev->Profile.BthFreeBrb((PBRB)brb);
        goto fail;
    }
    Oa2dpGetRequest(lower)->Brb = (struct _BRB *)brb;
    Oa2dpGetRequest(lower)->Channel = ch;
    status = SubmitBrbAsync(dev, lower, (struct _BRB *)brb, sizeof(*brb), OpenDone, dev);
    if (status != STATUS_PENDING) {
        WdfObjectDelete(lower);
        dev->Profile.BthFreeBrb((PBRB)brb);
        goto fail;
    }
    return;

fail:
    WdfSpinLockAcquire(dev->Lock);
    ch->State = ChannelClosed;
    ch->OpenRequest = NULL;
    WdfSpinLockRelease(dev->Lock);
    WdfRequestComplete(request, status);
}

static void OpenDone(_In_ WDFREQUEST Lower, _In_ WDFIOTARGET Target,
                     _In_ PWDF_REQUEST_COMPLETION_PARAMS Params, _In_ WDFCONTEXT Context)
{
    OA2DP_DEVICE_CONTEXT *dev = (OA2DP_DEVICE_CONTEXT *)Context;
    OA2DP_REQUEST_CONTEXT *rc = Oa2dpGetRequest(Lower);
    struct _BRB_L2CA_OPEN_CHANNEL *brb = (struct _BRB_L2CA_OPEN_CHANNEL *)rc->Brb;
    OA2DP_CHANNEL *ch = rc->Channel;
    NTSTATUS status = Params->IoStatus.Status;
    WDFREQUEST user;
    OA2DP_CHANNEL_INFO *out = NULL;
    ULONG_PTR info = 0;
    UNREFERENCED_PARAMETER(Target);

    WdfSpinLockAcquire(dev->Lock);
    user = ch->OpenRequest;
    ch->OpenRequest = NULL;
    if (NT_SUCCESS(status)) {
        ch->State = ChannelOpen;
        ch->Handle = brb->ChannelHandle;
        ch->OutMtu = brb->OutResults.Params.Mtu;
        ch->InMtu = brb->InResults.Params.Mtu;
    } else {
        ch->State = ChannelClosed;
    }
    WdfSpinLockRelease(dev->Lock);

    if (user) {
        if (NT_SUCCESS(status) &&
            NT_SUCCESS(WdfRequestRetrieveOutputBuffer(user, sizeof(*out), (PVOID *)&out, NULL))) {
            out->Kind = ch->Kind;
            out->OutMtu = ch->OutMtu;
            out->InMtu = ch->InMtu;
            info = sizeof(*out);
        }
        WdfRequestCompleteWithInformation(user, status, info);
    }
    dev->Profile.BthFreeBrb((PBRB)brb);
    WdfObjectDelete(Lower);
}

NTSTATUS Oa2dpCloseChannel(_In_ OA2DP_DEVICE_CONTEXT *dev, _In_ OA2DP_CHANNEL_KIND kind)
{
    OA2DP_CHANNEL *ch = &dev->Channels[kind];
    struct _BRB_L2CA_CLOSE_CHANNEL *brb;
    L2CAP_CHANNEL_HANDLE handle;
    NTSTATUS status;

    WdfSpinLockAcquire(dev->Lock);
    if (ch->State != ChannelOpen && ch->State != ChannelClosing) {
        WdfSpinLockRelease(dev->Lock);
        return STATUS_SUCCESS;
    }
    handle = ch->Handle;
    ch->State = ChannelClosing;
    WdfSpinLockRelease(dev->Lock);

    brb = (struct _BRB_L2CA_CLOSE_CHANNEL *)dev->Profile.BthAllocateBrb(BRB_L2CA_CLOSE_CHANNEL,
                                                                        OA2DP_POOL_TAG);
    if (!brb)
        return STATUS_INSUFFICIENT_RESOURCES;
    brb->BtAddress = dev->RemoteAddress;
    brb->ChannelHandle = handle;
    status = Oa2dpSubmitBrbSync(dev, (struct _BRB *)brb, sizeof(*brb));
    dev->Profile.BthFreeBrb((PBRB)brb);

    WdfSpinLockAcquire(dev->Lock);
    ch->State = ChannelClosed;
    ch->Handle = NULL;
    WdfSpinLockRelease(dev->Lock);
    Oa2dpPostEvent(dev, Oa2dpEventChannelClosed, ch);
    return status;
}

VOID Oa2dpCloseAllChannels(_In_ OA2DP_DEVICE_CONTEXT *dev)
{
    /* Media first: AVDTP closes the transport before signalling. */
    (void)Oa2dpCloseChannel(dev, Oa2dpChannelMedia);
    (void)Oa2dpCloseChannel(dev, Oa2dpChannelSignaling);
}

/* ------------------------------------------------------------------------ */
/* Data transfer                                                            */
/* ------------------------------------------------------------------------ */

static EVT_WDF_REQUEST_COMPLETION_ROUTINE TransferDone;

VOID Oa2dpTransfer(_In_ OA2DP_DEVICE_CONTEXT *dev, _In_ WDFREQUEST request,
                   _In_ OA2DP_CHANNEL_KIND kind, _In_ BOOLEAN send)
{
    OA2DP_CHANNEL *ch = &dev->Channels[kind];
    struct _BRB_L2CA_ACL_TRANSFER *brb;
    PMDL mdl;
    NTSTATUS status;
    L2CAP_CHANNEL_HANDLE handle;
    USHORT outMtu;
    ULONG len;

    status = WdfRequestRetrieveOutputWdmMdl(request, &mdl);
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(request, status);
        return;
    }
    len = MmGetMdlByteCount(mdl);

    WdfSpinLockAcquire(dev->Lock);
    handle = ch->Handle;
    outMtu = ch->OutMtu;
    status = ch->State == ChannelOpen ? STATUS_SUCCESS : STATUS_CONNECTION_DISCONNECTED;
    WdfSpinLockRelease(dev->Lock);
    if (NT_SUCCESS(status) && send && len > outMtu)
        status = STATUS_INVALID_BUFFER_SIZE;
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(request, status);
        return;
    }

    brb = (struct _BRB_L2CA_ACL_TRANSFER *)dev->Profile.BthAllocateBrb(BRB_L2CA_ACL_TRANSFER,
                                                                       OA2DP_POOL_TAG);
    if (!brb) {
        WdfRequestComplete(request, STATUS_INSUFFICIENT_RESOURCES);
        return;
    }
    brb->BtAddress = dev->RemoteAddress;
    brb->ChannelHandle = handle;
    brb->TransferFlags = send ? ACL_TRANSFER_DIRECTION_OUT
                              : (ACL_TRANSFER_DIRECTION_IN | ACL_SHORT_TRANSFER_OK);
    brb->BufferMDL = mdl;
    brb->Buffer = NULL;
    brb->BufferSize = len;
    brb->Timeout = 0;

    OA2DP_REQUEST_CONTEXT *rc = Oa2dpGetRequest(request);
    rc->Brb = (struct _BRB *)brb;
    rc->Channel = ch;
    rc->Bytes = len;

    if (send && kind == Oa2dpChannelMedia) {
        InterlockedIncrement(&dev->TxQueuedPackets);
        InterlockedExchangeAdd(&dev->TxQueuedBytes, (LONG)len);
    }
    status = SubmitBrbAsync(dev, request, (struct _BRB *)brb, sizeof(*brb), TransferDone, dev);
    if (status != STATUS_PENDING) {
        if (send && kind == Oa2dpChannelMedia) {
            InterlockedDecrement(&dev->TxQueuedPackets);
            InterlockedExchangeAdd(&dev->TxQueuedBytes, -(LONG)len);
        }
        dev->Profile.BthFreeBrb((PBRB)brb);
        WdfRequestComplete(request, status);
    }
}

static void TransferDone(_In_ WDFREQUEST Request, _In_ WDFIOTARGET Target,
                         _In_ PWDF_REQUEST_COMPLETION_PARAMS Params, _In_ WDFCONTEXT Context)
{
    OA2DP_DEVICE_CONTEXT *dev = (OA2DP_DEVICE_CONTEXT *)Context;
    OA2DP_REQUEST_CONTEXT *rc = Oa2dpGetRequest(Request);
    struct _BRB_L2CA_ACL_TRANSFER *brb = (struct _BRB_L2CA_ACL_TRANSFER *)rc->Brb;
    NTSTATUS status = Params->IoStatus.Status;
    BOOLEAN send = (brb->TransferFlags & ACL_TRANSFER_DIRECTION_IN) == 0;
    ULONG bytes = NT_SUCCESS(status) ? (ULONG)brb->BufferSize : 0;
    UNREFERENCED_PARAMETER(Target);

    if (send && rc->Channel->Kind == Oa2dpChannelMedia) {
        InterlockedDecrement(&dev->TxQueuedPackets);
        InterlockedExchangeAdd(&dev->TxQueuedBytes, -(LONG)rc->Bytes);
        if (NT_SUCCESS(status))
            InterlockedIncrement64(&dev->TxSent);
        else
            InterlockedIncrement64(&dev->TxFailed);
    }
    dev->Profile.BthFreeBrb((PBRB)brb);
    rc->Brb = NULL;
    WdfRequestCompleteWithInformation(Request, status, bytes);
}
