/* SPDX-License-Identifier: MIT */
#include "oa2dp.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT, DriverEntry)
#pragma alloc_text(PAGE, Oa2dpEvtDeviceAdd)
#pragma alloc_text(PAGE, Oa2dpEvtPrepareHardware)
#pragma alloc_text(PAGE, Oa2dpEvtReleaseHardware)
#endif

NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, Oa2dpEvtDeviceAdd);
    config.DriverPoolTag = OA2DP_POOL_TAG;
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config,
                           WDF_NO_HANDLE);
}

NTSTATUS Oa2dpEvtDeviceAdd(_In_ WDFDRIVER Driver, _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    NTSTATUS status;
    WDFDEVICE device;
    WDF_OBJECT_ATTRIBUTES attrs;
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_IO_QUEUE_CONFIG qcfg;
    OA2DP_DEVICE_CONTEXT *dev;
    UNREFERENCED_PARAMETER(Driver);
    PAGED_CODE();

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = Oa2dpEvtPrepareHardware;
    pnp.EvtDeviceReleaseHardware = Oa2dpEvtReleaseHardware;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnp);

    /* Only the service (LocalSystem) and administrators may open us. */
    {
        DECLARE_CONST_UNICODE_STRING(sddl, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");
        status = WdfDeviceInitAssignSDDLString(DeviceInit, &sddl);
        if (!NT_SUCCESS(status))
            return status;
    }

    /* Every request carries a context for its BRB. */
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attrs, OA2DP_REQUEST_CONTEXT);
    WdfDeviceInitSetRequestAttributes(DeviceInit, &attrs);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attrs, OA2DP_DEVICE_CONTEXT);
    status = WdfDeviceCreate(&DeviceInit, &attrs, &device);
    if (!NT_SUCCESS(status))
        return status;

    dev = Oa2dpGetDevice(device);
    RtlZeroMemory(dev, sizeof(*dev));
    dev->Device = device;
    dev->IoTarget = WdfDeviceGetIoTarget(device);
    for (ULONG i = 0; i < Oa2dpChannelCount; i++) {
        dev->Channels[i].Dev = dev;
        dev->Channels[i].Kind = (OA2DP_CHANNEL_KIND)i;
    }

    WDF_OBJECT_ATTRIBUTES_INIT(&attrs);
    attrs.ParentObject = device;
    status = WdfSpinLockCreate(&attrs, &dev->Lock);
    if (!NT_SUCCESS(status))
        return status;

    /* PASSIVE_LEVEL dispatch: CLOSE_CHANNEL submits BRBs synchronously. */
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&qcfg, WdfIoQueueDispatchParallel);
    qcfg.EvtIoDeviceControl = Oa2dpEvtIoDeviceControl;
    WDF_OBJECT_ATTRIBUTES_INIT(&attrs);
    attrs.ExecutionLevel = WdfExecutionLevelPassive;
    status = WdfIoQueueCreate(device, &qcfg, &attrs, &dev->DefaultQueue);
    if (!NT_SUCCESS(status))
        return status;

    WDF_IO_QUEUE_CONFIG_INIT(&qcfg, WdfIoQueueDispatchManual);
    status = WdfIoQueueCreate(device, &qcfg, WDF_NO_OBJECT_ATTRIBUTES, &dev->EventQueue);
    if (!NT_SUCCESS(status))
        return status;

    return WdfDeviceCreateDeviceInterface(device, &GUID_DEVINTERFACE_OA2DP_TRANSPORT, NULL);
}

NTSTATUS Oa2dpEvtPrepareHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST Raw,
                                 _In_ WDFCMRESLIST Translated)
{
    OA2DP_DEVICE_CONTEXT *dev = Oa2dpGetDevice(Device);
    NTSTATUS status;
    UNREFERENCED_PARAMETER(Raw);
    UNREFERENCED_PARAMETER(Translated);
    PAGED_CODE();

    status = WdfFdoQueryForInterface(Device, &GUID_BTHDDI_PROFILE_DRIVER_INTERFACE,
                                     (PINTERFACE)&dev->Profile, sizeof(dev->Profile),
                                     BTHDDI_PROFILE_DRIVER_INTERFACE_VERSION_FOR_QI, NULL);
    if (!NT_SUCCESS(status))
        return status;

    status = Oa2dpQueryLocalAddress(dev);
    if (!NT_SUCCESS(status))
        return status;
    status = Oa2dpQueryRemoteAddress(dev);
    if (!NT_SUCCESS(status))
        return status;

    /* Accepting headset-initiated connections is best effort: if another
     * driver already owns PSM 0x19 we still work as connection initiator. */
    (void)Oa2dpRegisterServer(dev);

    return Oa2dpCreateAudioChild(dev);
}

NTSTATUS Oa2dpEvtReleaseHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST Translated)
{
    OA2DP_DEVICE_CONTEXT *dev = Oa2dpGetDevice(Device);
    UNREFERENCED_PARAMETER(Translated);
    PAGED_CODE();
    Oa2dpCloseAllChannels(dev);
    Oa2dpUnregisterServer(dev);
    Oa2dpPostEvent(dev, Oa2dpEventDeviceGone, NULL);
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------------ */

VOID Oa2dpPostEvent(_In_ OA2DP_DEVICE_CONTEXT *dev, _In_ ULONG type, _In_ OA2DP_CHANNEL *ch)
{
    WDFREQUEST req;
    OA2DP_EVENT *ev;
    /* Events with nobody waiting are dropped; the service re-reads state
     * with GET_INFO / OPEN_CHANNEL after it re-arms WAIT_EVENT. */
    if (!NT_SUCCESS(WdfIoQueueRetrieveNextRequest(dev->EventQueue, &req)))
        return;
    if (!NT_SUCCESS(WdfRequestRetrieveOutputBuffer(req, sizeof(*ev), (PVOID *)&ev, NULL))) {
        WdfRequestComplete(req, STATUS_BUFFER_TOO_SMALL);
        return;
    }
    RtlZeroMemory(ev, sizeof(*ev));
    ev->Type = type;
    if (ch) {
        ev->Kind = ch->Kind;
        ev->Channel.Kind = ch->Kind;
        ev->Channel.OutMtu = ch->OutMtu;
        ev->Channel.InMtu = ch->InMtu;
    }
    WdfRequestCompleteWithInformation(req, STATUS_SUCCESS, sizeof(*ev));
}

NTSTATUS Oa2dpCreateAudioChild(_In_ OA2DP_DEVICE_CONTEXT *dev)
{
    NTSTATUS status;
    PWDFDEVICE_INIT init;
    WDFDEVICE child;
    WCHAR instance[24];
    UNICODE_STRING id;
    DECLARE_CONST_UNICODE_STRING(deviceId, L"OA2DP\\Render");
    DECLARE_CONST_UNICODE_STRING(hardwareId, L"OA2DP\\Render");

    if (dev->AudioChild)
        return STATUS_SUCCESS;

    init = WdfPdoInitAllocate(dev->Device);
    if (!init)
        return STATUS_INSUFFICIENT_RESOURCES;

    status = WdfPdoInitAssignDeviceID(init, &deviceId);
    if (NT_SUCCESS(status))
        status = WdfPdoInitAddHardwareID(init, &hardwareId);
    if (NT_SUCCESS(status)) {
        /* One child per headset: instance ID = Bluetooth address. */
        status = RtlStringCchPrintfW(instance, ARRAYSIZE(instance), L"%012I64X",
                                     (ULONGLONG)dev->RemoteAddress);
    }
    if (NT_SUCCESS(status)) {
        RtlInitUnicodeString(&id, instance);
        status = WdfPdoInitAssignInstanceID(init, &id);
    }
    if (!NT_SUCCESS(status)) {
        WdfDeviceInitFree(init);
        return status;
    }

    status = WdfDeviceCreate(&init, WDF_NO_OBJECT_ATTRIBUTES, &child);
    if (!NT_SUCCESS(status)) {
        if (init)
            WdfDeviceInitFree(init);
        return status;
    }
    {
        /* The address is unique system-wide: without UniqueID the PnP manager
         * prefixes the instance ID with parent data, and the service (which
         * looks for "#<address>#" in oa2dpaudio's interface path) would not
         * find the endpoint. */
        WDF_DEVICE_PNP_CAPABILITIES caps;
        WDF_DEVICE_PNP_CAPABILITIES_INIT(&caps);
        caps.UniqueID = WdfTrue;
        WdfDeviceSetPnpCapabilities(child, &caps);
    }
    status = WdfFdoAddStaticChild(dev->Device, child);
    if (NT_SUCCESS(status))
        dev->AudioChild = child;
    return status;
}

/* ------------------------------------------------------------------------ */

VOID Oa2dpEvtIoDeviceControl(_In_ WDFQUEUE Queue, _In_ WDFREQUEST Request,
                             _In_ size_t OutLen, _In_ size_t InLen, _In_ ULONG Code)
{
    OA2DP_DEVICE_CONTEXT *dev = Oa2dpGetDevice(WdfIoQueueGetDevice(Queue));
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR info = 0;
    UNREFERENCED_PARAMETER(OutLen);
    UNREFERENCED_PARAMETER(InLen);

    switch (Code) {
    case IOCTL_OA2DP_GET_INFO: {
        OA2DP_DEVICE_INFO *out;
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*out), (PVOID *)&out, NULL);
        if (NT_SUCCESS(status)) {
            RtlZeroMemory(out, sizeof(*out));
            out->Version = OA2DP_IOCTL_VERSION;
            out->RemoteAddress = dev->RemoteAddress;
            out->LocalAddress = dev->LocalAddress;
            info = sizeof(*out);
        }
        break;
    }

    case IOCTL_OA2DP_OPEN_CHANNEL: {
        OA2DP_OPEN_CHANNEL_IN *in;
        status = WdfRequestRetrieveInputBuffer(Request, sizeof(*in), (PVOID *)&in, NULL);
        if (NT_SUCCESS(status) && in->Kind >= Oa2dpChannelCount)
            status = STATUS_INVALID_PARAMETER;
        if (NT_SUCCESS(status)) {
            Oa2dpOpenChannel(dev, Request, in); /* completes the request */
            return;
        }
        break;
    }

    case IOCTL_OA2DP_CLOSE_CHANNEL: {
        OA2DP_CLOSE_CHANNEL_IN *in;
        status = WdfRequestRetrieveInputBuffer(Request, sizeof(*in), (PVOID *)&in, NULL);
        if (NT_SUCCESS(status))
            status = in->Kind < Oa2dpChannelCount
                         ? Oa2dpCloseChannel(dev, (OA2DP_CHANNEL_KIND)in->Kind)
                         : STATUS_INVALID_PARAMETER;
        break;
    }

    case IOCTL_OA2DP_SEND:
    case IOCTL_OA2DP_RECV: {
        OA2DP_SEND_IN *in;
        status = WdfRequestRetrieveInputBuffer(Request, sizeof(*in), (PVOID *)&in, NULL);
        if (NT_SUCCESS(status) && in->Kind >= Oa2dpChannelCount)
            status = STATUS_INVALID_PARAMETER;
        if (NT_SUCCESS(status)) {
            Oa2dpTransfer(dev, Request, (OA2DP_CHANNEL_KIND)in->Kind, Code == IOCTL_OA2DP_SEND);
            return;
        }
        break;
    }

    case IOCTL_OA2DP_WAIT_EVENT:
        status = WdfRequestForwardToIoQueue(Request, dev->EventQueue);
        if (NT_SUCCESS(status))
            return;
        break;

    case IOCTL_OA2DP_GET_TX_STATE: {
        OA2DP_TX_STATE *out;
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*out), (PVOID *)&out, NULL);
        if (NT_SUCCESS(status)) {
            out->QueuedPackets = (ULONG)InterlockedCompareExchange(&dev->TxQueuedPackets, 0, 0);
            out->QueuedBytes = (ULONG)InterlockedCompareExchange(&dev->TxQueuedBytes, 0, 0);
            out->SentPackets = (ULONGLONG)InterlockedCompareExchange64(&dev->TxSent, 0, 0);
            out->FailedPackets = (ULONGLONG)InterlockedCompareExchange64(&dev->TxFailed, 0, 0);
            info = sizeof(*out);
        }
        break;
    }

    default:
        break;
    }
    WdfRequestCompleteWithInformation(Request, status, info);
}
