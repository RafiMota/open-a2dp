/*++

Copyright (c) Microsoft Corporation All Rights Reserved

Module Name:

    adapter.cpp

Abstract:

    Setup and miniport installation.  No resources are used by sysvad.
    This sample is to demonstrate how to develop a full featured audio miniport driver.

--*/
/*
 * Derived from the SYSVAD sample (Windows-driver-samples/audio/sysvad,
 * commit 2dc3fd3a, MS-PL — see LICENSE.MS-PL in this folder): adapter.cpp
 * and the subdevice install/connect code of common.cpp. Modified for
 * OpenA2DP:
 *  - one render endpoint (wave + topology) per OA2DP\Render\<address> PDO;
 *    no adapter-common object, no KMDF, no PoFx, no registry knobs;
 *  - IRP_MJ_CREATE/CLEANUP/CLOSE/DEVICE_CONTROL are hooked so the control
 *    interface for oa2dpsvc.exe (control.cpp) can share PortCls's device
 *    object; everything else still goes to PortCls;
 *  - the filters are re-registered after a friendly-name change.
 */
// All the GUIDS for the port class and our interfaces end up in this object.
#include <initguid.h>
#include "driver.h"
#include <devpkey.h>
#include "minwavert.h"

#define OA2DP_MAX_MINIPORTS 4   // wave + topology, with room for a re-registration

typedef void (*fnPcDriverUnload) (PDRIVER_OBJECT);
static fnPcDriverUnload gPCDriverUnloadRoutine = NULL;
static PDRIVER_DISPATCH gPcDispatch[IRP_MJ_MAXIMUM_FUNCTION + 1];

extern "C" DRIVER_INITIALIZE DriverEntry;
extern "C" DRIVER_UNLOAD DriverUnload;
DRIVER_ADD_DEVICE AddDevice;
_Dispatch_type_(IRP_MJ_PNP)
DRIVER_DISPATCH PnpHandler;
_Dispatch_type_(IRP_MJ_CREATE)
_Dispatch_type_(IRP_MJ_CLEANUP)
_Dispatch_type_(IRP_MJ_CLOSE)
_Dispatch_type_(IRP_MJ_DEVICE_CONTROL)
DRIVER_DISPATCH Oa2dpDispatch;
static IO_WORKITEM_ROUTINE RebuildWork;

NTSTATUS StartDevice(_In_ PDEVICE_OBJECT, _In_ PIRP, _In_ PRESOURCELIST);

//=============================================================================
#pragma code_seg("PAGE")
extern "C"
void DriverUnload
(
    _In_ PDRIVER_OBJECT DriverObject
)
{
    PAGED_CODE();

    // Invoke the port unload.
    if (gPCDriverUnloadRoutine != NULL)
    {
        gPCDriverUnloadRoutine(DriverObject);
    }
}

//=============================================================================
#pragma code_seg("INIT")
extern "C" NTSTATUS
DriverEntry
(
    _In_  PDRIVER_OBJECT          DriverObject,
    _In_  PUNICODE_STRING         RegistryPathName
)
/*++

Routine Description:

  Installable driver initialization entry point.

--*/
{
    static const UCHAR hooked[] = { IRP_MJ_CREATE, IRP_MJ_CLEANUP, IRP_MJ_CLOSE, IRP_MJ_DEVICE_CONTROL };
    NTSTATUS ntStatus;

    // Tell the class driver to initialize the driver.
    ntStatus = PcInitializeAdapterDriver(DriverObject,
                                         RegistryPathName,
                                         (PDRIVER_ADD_DEVICE)AddDevice);
    if (!NT_SUCCESS(ntStatus))
    {
        return ntStatus;
    }

    // To intercept stop/remove/surprise-remove.
    DriverObject->MajorFunction[IRP_MJ_PNP] = PnpHandler;

    // PortCls owns the dispatch table; the control interface shares the FDO,
    // so we look at these IRPs first and pass the rest on.
    for (ULONG i = 0; i < RTL_NUMBER_OF(hooked); i++)
    {
        gPcDispatch[hooked[i]] = DriverObject->MajorFunction[hooked[i]];
        DriverObject->MajorFunction[hooked[i]] = Oa2dpDispatch;
    }

    // Hook the port class unload function
    gPCDriverUnloadRoutine = DriverObject->DriverUnload;
    DriverObject->DriverUnload = DriverUnload;

    return STATUS_SUCCESS;
} // DriverEntry

// disable prefast warning 28152 because
// DO_DEVICE_INITIALIZING is cleared in PcAddAdapterDevice
#pragma warning(disable:28152)

//=============================================================================
#pragma code_seg("PAGE")
NTSTATUS AddDevice
(
    _In_  PDRIVER_OBJECT    DriverObject,
    _In_  PDEVICE_OBJECT    PhysicalDeviceObject
)
/*++

Routine Description:

  The Plug & Play subsystem is handing us a brand new PDO (OA2DP\Render\<addr>,
  created by oa2dp.sys). PortCls creates the FDO; our OA2DP_AUDIO_DEVICE lives
  after PortCls's part of its extension.

--*/
{
    PAGED_CODE();

    return PcAddAdapterDevice(DriverObject,
                              PhysicalDeviceObject,
                              PCPFNSTARTDEVICE(StartDevice),
                              OA2DP_MAX_MINIPORTS,
                              PORT_CLASS_DEVICE_EXTENSION_SIZE + sizeof(OA2DP_AUDIO_DEVICE));
} // AddDevice

//=============================================================================
#pragma code_seg()
NTSTATUS
Oa2dpDispatch
(
    _In_ DEVICE_OBJECT *DeviceObject,
    _Inout_ IRP *Irp
)
{
    OA2DP_AUDIO_DEVICE *dev = OA2DP_DEVICE(DeviceObject);
    PIO_STACK_LOCATION  sp = IoGetCurrentIrpStackLocation(Irp);
    BOOLEAN             ours;

    ours = sp->MajorFunction == IRP_MJ_CREATE ? Oa2dpControlIsCreate(Irp) : Oa2dpControlOwns(dev, Irp);
    if (ours)
    {
        return Oa2dpControlDispatch(dev, Irp);
    }
    return gPcDispatch[sp->MajorFunction](DeviceObject, Irp);
}

//=============================================================================
// Best effort: name the KS interfaces too (legacy WinMM device names).
#pragma code_seg("PAGE")
static VOID
SetInterfaceName
(
    _In_ OA2DP_AUDIO_DEVICE *dev,
    _In_ PCWSTR              ReferenceString
)
{
    WCHAR           name[RTL_NUMBER_OF_FIELD(OA2DP_AUDIO_DEVICE, Name)];
    UNICODE_STRING  ref;
    UNICODE_STRING  link;
    size_t          len = 0;

    PAGED_CODE();

    Oa2dpLock(&dev->ControlLock);
    RtlCopyMemory(name, dev->Name, sizeof(name));
    Oa2dpUnlock(&dev->ControlLock);
    name[RTL_NUMBER_OF(name) - 1] = L'\0';
    if (!NT_SUCCESS(RtlStringCchLengthW(name, RTL_NUMBER_OF(name), &len)) || len == 0)
    {
        return;
    }

    RtlInitUnicodeString(&ref, ReferenceString);
    if (NT_SUCCESS(IoRegisterDeviceInterface(dev->Pdo, &KSCATEGORY_AUDIO, &ref, &link)))
    {
        (void)IoSetDeviceInterfacePropertyData(&link, &DEVPKEY_DeviceInterface_FriendlyName,
                                               LOCALE_NEUTRAL, PLUGPLAY_PROPERTY_PERSISTENT,
                                               DEVPROP_TYPE_STRING,
                                               (ULONG)((len + 1) * sizeof(WCHAR)), name);
        RtlFreeUnicodeString(&link);
    }
}

//=============================================================================
#pragma code_seg("PAGE")
static NTSTATUS
InstallSubdevice
(
    _In_  OA2DP_AUDIO_DEVICE   *dev,
    _In_  PCWSTR                Name,
    _In_  REFGUID               PortClassId,
    _In_  PUNKNOWN              Miniport,
    _Out_ PUNKNOWN             *OutPort
)
/*++

Routine Description:

    This function creates and registers a subdevice consisting of a port
    driver, a minport driver and a set of resources bound together.

--*/
{
    PAGED_CODE();

    NTSTATUS    ntStatus;
    PPORT       port = NULL;

    *OutPort = NULL;

    SetInterfaceName(dev, Name);

    // Create the port driver object
    ntStatus = PcNewPort(&port, PortClassId);

    // Init the port driver and miniport in one go.
    if (NT_SUCCESS(ntStatus))
    {
#pragma warning(push)
        // IPort::Init's annotation on ResourceList requires it to be non-NULL.  However,
        // for dynamic devices, we may no longer have the resource list and this should
        // still succeed.
#pragma warning(disable:6387)
        ntStatus = port->Init(dev->Fdo, NULL, Miniport, NULL, NULL);
#pragma warning (pop)
    }

    // Register the subdevice (port/miniport combination).
    if (NT_SUCCESS(ntStatus))
    {
        ntStatus = PcRegisterSubdevice(dev->Fdo, (PWSTR)Name, port);
    }

    if (NT_SUCCESS(ntStatus))
    {
        *OutPort = port;    // keep our reference
        port = NULL;
    }

    SAFE_RELEASE(port);
    return ntStatus;
} // InstallSubdevice

//=============================================================================
#pragma code_seg("PAGE")
static VOID
UnregisterSubdevice
(
    _In_     OA2DP_AUDIO_DEVICE *dev,
    _In_opt_ PUNKNOWN            UnknownPort
)
{
    PAGED_CODE();

    PUNREGISTERSUBDEVICE unregisterSubdevice = NULL;

    if (NULL == UnknownPort)
    {
        return;
    }

    if (NT_SUCCESS(UnknownPort->QueryInterface(IID_IUnregisterSubdevice, (PVOID *)&unregisterSubdevice)))
    {
        (void)unregisterSubdevice->UnregisterSubdevice(dev->Fdo, UnknownPort);
        unregisterSubdevice->Release();
    }
}

//=============================================================================
// FilterLock held.
#pragma code_seg("PAGE")
static VOID
RemoveFilters
(
    _Inout_ OA2DP_AUDIO_DEVICE *dev
)
{
    PAGED_CODE();

    PUNREGISTERPHYSICALCONNECTION unregisterPhysicalConnection = NULL;
    PUNKNOWN wavePort = static_cast<PUNKNOWN>(dev->WavePort);
    PUNKNOWN topoPort = static_cast<PUNKNOWN>(dev->TopoPort);

    if (topoPort && wavePort &&
        NT_SUCCESS(topoPort->QueryInterface(IID_IUnregisterPhysicalConnection,
                                            (PVOID *)&unregisterPhysicalConnection)))
    {
        (void)unregisterPhysicalConnection->UnregisterPhysicalConnection(
            dev->Fdo, wavePort, OA2DP_WAVE_PIN_BRIDGE, topoPort, OA2DP_TOPO_PIN_IN);
        unregisterPhysicalConnection->Release();
    }

    UnregisterSubdevice(dev, wavePort);
    UnregisterSubdevice(dev, topoPort);
    SAFE_RELEASE(wavePort);
    SAFE_RELEASE(topoPort);
    dev->WavePort = NULL;
    dev->TopoPort = NULL;
    if (dev->WaveMiniport)
    {
        static_cast<CMiniportWaveRT *>(dev->WaveMiniport)->Release();
        dev->WaveMiniport = NULL;
    }
}

//=============================================================================
// FilterLock held.
#pragma code_seg("PAGE")
static NTSTATUS
InstallFilters
(
    _Inout_ OA2DP_AUDIO_DEVICE *dev
)
{
    PAGED_CODE();

    NTSTATUS            ntStatus;
    PUNKNOWN            miniTopo = NULL;
    PUNKNOWN            topoPort = NULL;
    PUNKNOWN            wavePort = NULL;
    CMiniportWaveRT    *miniWave = NULL;

    if (dev->WavePort != NULL)
    {
        return STATUS_SUCCESS;
    }

    // Topology first (the wave filter's bridge pin connects to it).
    ntStatus = CreateMiniportTopology(&miniTopo);
    if (NT_SUCCESS(ntStatus))
    {
        ntStatus = InstallSubdevice(dev, OA2DP_TOPO_NAME, CLSID_PortTopology, miniTopo, &topoPort);
    }
    if (NT_SUCCESS(ntStatus))
    {
        ntStatus = CreateMiniportWaveRT(&miniWave, dev);
    }
    if (NT_SUCCESS(ntStatus))
    {
        ntStatus = InstallSubdevice(dev, OA2DP_WAVE_NAME, CLSID_PortWaveRT,
                                    PUNKNOWN(PMINIPORTWAVERT(miniWave)), &wavePort);
    }

    // register wave <=> topology connections
    if (NT_SUCCESS(ntStatus))
    {
        ntStatus = PcRegisterPhysicalConnection(dev->Fdo,
                                                wavePort, OA2DP_WAVE_PIN_BRIDGE,
                                                topoPort, OA2DP_TOPO_PIN_IN);
    }

    if (NT_SUCCESS(ntStatus))
    {
        dev->TopoPort = topoPort;
        dev->WavePort = wavePort;
        dev->WaveMiniport = miniWave;   // keeps our reference
        topoPort = NULL;
        wavePort = NULL;
        miniWave = NULL;
    }
    else
    {
        UnregisterSubdevice(dev, wavePort);
        UnregisterSubdevice(dev, topoPort);
    }

    SAFE_RELEASE(miniTopo);
    SAFE_RELEASE(miniWave);
    SAFE_RELEASE(wavePort);
    SAFE_RELEASE(topoPort);
    return ntStatus;
}

//=============================================================================
#pragma code_seg()
static VOID
RebuildWork
(
    _In_     PDEVICE_OBJECT DeviceObject,
    _In_opt_ PVOID          Context
)
{
    OA2DP_AUDIO_DEVICE *dev = (OA2DP_AUDIO_DEVICE *)Context;
    KIRQL               irql;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (dev == NULL)
    {
        return;
    }

    KeAcquireSpinLock(&dev->Lock, &irql);
    dev->RebuildQueued = 0;     // a later change queues us again
    KeReleaseSpinLock(&dev->Lock, irql);

    Oa2dpLock(&dev->FilterLock);
    if (dev->Started && !dev->Removed && dev->NameChanged && dev->Streams == 0 && dev->WavePort)
    {
        // The endpoint builder reads the device's friendly name when the KS
        // interfaces arrive; re-registering makes a new name show at once.
        RemoveFilters(dev);
        dev->NameChanged = FALSE;
        NTSTATUS ntStatus = InstallFilters(dev);
        if (!NT_SUCCESS(ntStatus))
        {
            KdPrintEx((DPFLTR_IHVAUDIO_ID, DPFLTR_ERROR_LEVEL,
                       "oa2dpaudio: re-registering the filters failed 0x%08X\n", ntStatus));
        }
    }
    Oa2dpUnlock(&dev->FilterLock);

    InterlockedDecrement(&dev->WorkQueued);
}

#pragma code_seg()
extern "C" VOID Oa2dpScheduleRebuild(_Inout_ OA2DP_AUDIO_DEVICE *dev)
{
    KIRQL   irql;
    BOOLEAN queue = FALSE;


    KeAcquireSpinLock(&dev->Lock, &irql);
    if (dev->Initialized && !dev->Removed && dev->WorkItem && !dev->RebuildQueued)
    {
        dev->RebuildQueued = 1;
        InterlockedIncrement(&dev->WorkQueued);
        queue = TRUE;
    }
    KeReleaseSpinLock(&dev->Lock, irql);

    if (queue)
    {
        IoQueueWorkItem(dev->WorkItem, RebuildWork, DelayedWorkQueue, dev);
    }
}

//=============================================================================
#pragma code_seg("PAGE")
static NTSTATUS
InitDevice
(
    _Inout_ OA2DP_AUDIO_DEVICE *dev,
    _In_    PDEVICE_OBJECT      DeviceObject
)
{
    PAGED_CODE();

    NTSTATUS            ntStatus;
    OA2DP_FORMAT_LIST   list;
    const OA2DP_FORMAT *f;

    dev->Fdo = DeviceObject;
    ntStatus = PcGetPhysicalDeviceObject(DeviceObject, &dev->Pdo);
    if (!NT_SUCCESS(ntStatus))
    {
        return ntStatus;
    }

    Oa2dpControlInit(dev);
    KeInitializeEvent(&dev->FilterLock, SynchronizationEvent, TRUE);

    dev->WorkItem = IoAllocateWorkItem(DeviceObject);
    if (dev->WorkItem == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // Last formats and name the service gave us, if any.
    Oa2dpLoadSettings(dev);
    Oa2dpGetFormats(dev, &list);
    f = &list.Formats[list.Preferred];
    dev->Current = *f;

    ntStatus = Oa2dpRingCreate(&dev->Ring, f->SampleRate, f->Channels,
                               f->BitsPerSample == 32 ? OA2DP_RING_FMT_FLOAT32 : OA2DP_RING_FMT_S32);
    if (!NT_SUCCESS(ntStatus))
    {
        IoFreeWorkItem(dev->WorkItem);
        dev->WorkItem = NULL;
        return ntStatus;
    }

    dev->Initialized = TRUE;
    return STATUS_SUCCESS;
}

//=============================================================================
#pragma code_seg("PAGE")
NTSTATUS
StartDevice
(
    _In_  PDEVICE_OBJECT          DeviceObject,
    _In_  PIRP                    Irp,
    _In_  PRESOURCELIST           ResourceList
)
/*++

Routine Description:

  This function is called by the operating system when the device is
  started. It is responsible for starting the miniports.

--*/
{
    UNREFERENCED_PARAMETER(Irp);
    UNREFERENCED_PARAMETER(ResourceList);

    PAGED_CODE();

    ASSERT(DeviceObject);

    NTSTATUS            ntStatus = STATUS_SUCCESS;
    OA2DP_AUDIO_DEVICE *dev = OA2DP_DEVICE(DeviceObject);

    if (!dev->Initialized)
    {
        ntStatus = InitDevice(dev, DeviceObject);
        if (!NT_SUCCESS(ntStatus))
        {
            return ntStatus;
        }
    }

    // Install wave+topology filters for the render endpoint.
    Oa2dpLock(&dev->FilterLock);
    ntStatus = InstallFilters(dev);
    Oa2dpUnlock(&dev->FilterLock);

    if (NT_SUCCESS(ntStatus))
    {
        ntStatus = Oa2dpControlStart(dev);
    }

    if (NT_SUCCESS(ntStatus))
    {
        dev->Started = TRUE;
    }
    else
    {
        Oa2dpLock(&dev->FilterLock);
        RemoveFilters(dev);
        Oa2dpUnlock(&dev->FilterLock);
    }

    return ntStatus;
} // StartDevice

//=============================================================================
#pragma code_seg("PAGE")
static VOID
StopDevice
(
    _Inout_ OA2DP_AUDIO_DEVICE *dev
)
{
    PAGED_CODE();

    dev->Started = FALSE;
    Oa2dpControlStop(dev);
    Oa2dpLock(&dev->FilterLock);
    RemoveFilters(dev);
    Oa2dpUnlock(&dev->FilterLock);
}

#pragma code_seg()
static VOID
RemoveDevice
(
    _Inout_ OA2DP_AUDIO_DEVICE *dev
)
{

    KIRQL irql;
    LARGE_INTEGER wait;

    // No new work items; wait for a queued one to finish.
    KeAcquireSpinLock(&dev->Lock, &irql);
    dev->Removed = TRUE;
    KeReleaseSpinLock(&dev->Lock, irql);
    wait.QuadPart = -10 * OA2DP_HNS_PER_MS;
    while (InterlockedCompareExchange(&dev->WorkQueued, 0, 0) != 0)
    {
        KeDelayExecutionThread(KernelMode, FALSE, &wait);
    }

    // Unmaps the service's view of the ring (even from another process) and
    // frees it.
    Oa2dpControlRemove(dev);

    if (dev->WorkItem)
    {
        IoFreeWorkItem(dev->WorkItem);
        dev->WorkItem = NULL;
    }
}

//=============================================================================
#pragma code_seg("PAGE")
NTSTATUS
PnpHandler
(
    _In_ DEVICE_OBJECT *_DeviceObject,
    _Inout_ IRP *_Irp
)
/*++

Routine Description:

  Handles PnP IRPs

--*/
{
    IO_STACK_LOCATION      *stack;
    OA2DP_AUDIO_DEVICE     *dev;

    // This method will be called in IRQL PASSIVE_LEVEL
#pragma warning(suppress: 28118)
    PAGED_CODE();

    ASSERT(_DeviceObject);
    ASSERT(_Irp);

    stack = IoGetCurrentIrpStackLocation(_Irp);
    dev = OA2DP_DEVICE(_DeviceObject);

    if (dev->Initialized)
    {
        switch (stack->MinorFunction)
        {
        case IRP_MN_STOP_DEVICE:
        case IRP_MN_SURPRISE_REMOVAL:
            StopDevice(dev);
            break;

        case IRP_MN_REMOVE_DEVICE:
            StopDevice(dev);
            RemoveDevice(dev);
            break;

        default:
            break;
        }
    }

    return PcDispatchIrp(_DeviceObject, _Irp);
}
#pragma code_seg()
