/* SPDX-License-Identifier: MIT */
/*
 * oa2dpaudio.sys — control interface for oa2dpsvc.exe (the audio half of
 * driver/common/oa2dp_ioctl.h; consumer: AudioEndpoint in
 * service/src/driver_io.cpp).
 *
 * PortCls owns the driver's dispatch table, so adapter.cpp routes IRPs here:
 * an IRP_MJ_CREATE with the reference string OA2DP_CONTROL_NAME, and every
 * later IRP whose FILE_OBJECT we tagged with &dev->ControlTag. Everything
 * else goes to PortCls unchanged.
 *
 * GUID_DEVINTERFACE_OA2DP_AUDIO is registered on our PDO (OA2DP\Render\<addr>)
 * rather than on a control device object: only a PDO interface carries the
 * instance ID — the headset's address — in its path, and that is how the
 * service picks the endpoint of a given headset.
 *
 * The device object's ACL must let the audio engine (LOCAL SERVICE) open the
 * KS filters, so the control path checks the caller itself: SYSTEM or an
 * elevated administrator, same as oa2dp.sys's SDDL.
 */
#include <ntifs.h>
#include <ntstrsafe.h>
/* Instantiate the GUIDs of oa2dp_ioctl.h and the DEVPKEYs in this unit. */
#include <initguid.h>
#include <devpkey.h>

#include "oa2dpaudio.h"

#define FORMATS_VALUE L"Oa2dpFormats"
#define NAME_VALUE L"Oa2dpName"

/* ---- small helpers --------------------------------------------------------- */

extern "C" VOID Oa2dpLock(_Inout_ PKEVENT lock)
{
    KeEnterCriticalRegion();
    KeWaitForSingleObject(lock, Executive, KernelMode, FALSE, NULL);
}

extern "C" VOID Oa2dpUnlock(_Inout_ PKEVENT lock)
{
    KeSetEvent(lock, IO_NO_INCREMENT, FALSE);
    KeLeaveCriticalRegion();
}

static BOOLEAN FormatValid(_In_ const OA2DP_FORMAT *f)
{
    return f->SampleRate >= 8000 && f->SampleRate <= 192000 && f->Channels >= 1 &&
           f->Channels <= OA2DP_RING_MAX_CHANNELS &&
           (f->BitsPerSample == 16 || f->BitsPerSample == 24 || f->BitsPerSample == 32);
}

static BOOLEAN ListValid(_In_ const OA2DP_FORMAT_LIST *list)
{
    if (list->Count == 0 || list->Count > OA2DP_MAX_FORMATS || list->Preferred >= list->Count)
        return FALSE;
    for (ULONG i = 0; i < list->Count; i++)
        if (!FormatValid(&list->Formats[i]))
            return FALSE;
    return TRUE;
}

static BOOLEAN SameFormat(_In_ const OA2DP_FORMAT *a, _In_ const OA2DP_FORMAT *b)
{
    return a->SampleRate == b->SampleRate && a->Channels == b->Channels &&
           a->BitsPerSample == b->BitsPerSample;
}

/* Same set of formats (the preferred index may differ). */
static BOOLEAN SameFormats(_In_ const OA2DP_FORMAT_LIST *a, _In_ const OA2DP_FORMAT_LIST *b)
{
    if (a->Count != b->Count)
        return FALSE;
    for (ULONG i = 0; i < a->Count; i++)
        if (!SameFormat(&a->Formats[i], &b->Formats[i]))
            return FALSE;
    return TRUE;
}

/* Until the service tells us the codec's formats: 48 and 44.1 kHz stereo
 * float, which every A2DP sink must accept for SBC. */
static VOID DefaultFormats(_Out_ OA2DP_FORMAT_LIST *list)
{
    RtlZeroMemory(list, sizeof(*list));
    list->Count = 2;
    list->Preferred = 0;
    list->Formats[0].SampleRate = 48000;
    list->Formats[0].Channels = 2;
    list->Formats[0].BitsPerSample = 32;
    list->Formats[1].SampleRate = 44100;
    list->Formats[1].Channels = 2;
    list->Formats[1].BitsPerSample = 32;
}

extern "C" BOOLEAN Oa2dpFormatListed(_In_ const OA2DP_FORMAT_LIST *list, _In_ const OA2DP_FORMAT *f)
{
    for (ULONG i = 0; i < list->Count && i < OA2DP_MAX_FORMATS; i++)
        if (SameFormat(&list->Formats[i], f))
            return TRUE;
    return FALSE;
}

extern "C" VOID Oa2dpGetFormats(_In_ OA2DP_AUDIO_DEVICE *dev, _Out_ OA2DP_FORMAT_LIST *out)
{
    KIRQL irql;
    KeAcquireSpinLock(&dev->Lock, &irql);
    *out = dev->Formats;
    KeReleaseSpinLock(&dev->Lock, irql);
}

extern "C" ULONG Oa2dpLatencyUs(_In_ OA2DP_AUDIO_DEVICE *dev)
{
    KIRQL irql;
    ULONGLONG us;
    KeAcquireSpinLock(&dev->Lock, &irql);
    us = (ULONGLONG)dev->Latency.TransportUs + dev->Latency.DeviceUs;
    KeReleaseSpinLock(&dev->Lock, irql);
    return us > 10000000 ? 10000000 : (ULONG)us; /* cap at 10 s */
}

/* ---- persisted settings (device hardware key) ------------------------------ */

static VOID SaveValue(_In_ OA2DP_AUDIO_DEVICE *dev, _In_ PCWSTR name, _In_ ULONG type,
                      _In_reads_bytes_(size) const VOID *data, _In_ ULONG size)
{
    HANDLE key;
    UNICODE_STRING n;

    PAGED_CODE();
    if (!NT_SUCCESS(IoOpenDeviceRegistryKey(dev->Pdo, PLUGPLAY_REGKEY_DEVICE, KEY_WRITE, &key)))
        return;
    RtlInitUnicodeString(&n, name);
    (void)ZwSetValueKey(key, &n, 0, type, (PVOID)data, size);
    ZwClose(key);
}

static BOOLEAN LoadValue(_In_ OA2DP_AUDIO_DEVICE *dev, _In_ PCWSTR name, _In_ ULONG type,
                         _Out_writes_bytes_(size) PVOID data, _In_ ULONG size, _Out_ ULONG *got)
{
    HANDLE key;
    UNICODE_STRING n;
    ULONG need = FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION, Data) + size, len = 0;
    PKEY_VALUE_PARTIAL_INFORMATION info;
    BOOLEAN ok = FALSE;

    PAGED_CODE();
    *got = 0;
    if (!NT_SUCCESS(IoOpenDeviceRegistryKey(dev->Pdo, PLUGPLAY_REGKEY_DEVICE, KEY_READ, &key)))
        return FALSE;
    info = (PKEY_VALUE_PARTIAL_INFORMATION)ExAllocatePool2(POOL_FLAG_PAGED, need, OA2DP_AUDIO_TAG);
    if (info) {
        RtlInitUnicodeString(&n, name);
        if (NT_SUCCESS(ZwQueryValueKey(key, &n, KeyValuePartialInformation, info, need, &len)) &&
            info->Type == type && info->DataLength <= size) {
            RtlCopyMemory(data, info->Data, info->DataLength);
            *got = info->DataLength;
            ok = TRUE;
        }
        ExFreePoolWithTag(info, OA2DP_AUDIO_TAG);
    }
    ZwClose(key);
    return ok;
}

/* Last format list and name, so the endpoint looks right before the service
 * connects (and after a reboot). */
extern "C" VOID Oa2dpLoadSettings(_Inout_ OA2DP_AUDIO_DEVICE *dev)
{
    OA2DP_FORMAT_LIST list;
    WCHAR name[RTL_NUMBER_OF_FIELD(OA2DP_AUDIO_DEVICE, Name)];
    ULONG got;
    KIRQL irql;

    PAGED_CODE();
    if (LoadValue(dev, FORMATS_VALUE, REG_BINARY, &list, sizeof(list), &got) &&
        got == sizeof(list) && ListValid(&list)) {
        KeAcquireSpinLock(&dev->Lock, &irql);
        dev->Formats = list;
        KeReleaseSpinLock(&dev->Lock, irql);
    }
    RtlZeroMemory(name, sizeof(name));
    if (LoadValue(dev, NAME_VALUE, REG_SZ, name, sizeof(name) - sizeof(WCHAR), &got)) {
        Oa2dpLock(&dev->ControlLock);
        RtlStringCchCopyW(dev->Name, RTL_NUMBER_OF(dev->Name), name);
        Oa2dpUnlock(&dev->ControlLock);
    }
}

/* ---- WAIT_FORMAT queue ------------------------------------------------------- */

static OA2DP_AUDIO_DEVICE *CsqDevice(_In_ PIO_CSQ csq)
{
    return CONTAINING_RECORD(csq, OA2DP_AUDIO_DEVICE, WaitCsq);
}

static IO_CSQ_INSERT_IRP CsqInsert;
static VOID CsqInsert(_In_ PIO_CSQ csq, _In_ PIRP irp)
{
    InsertTailList(&CsqDevice(csq)->WaitList, &irp->Tail.Overlay.ListEntry);
}

static IO_CSQ_REMOVE_IRP CsqRemove;
static VOID CsqRemove(_In_ PIO_CSQ csq, _In_ PIRP irp)
{
    UNREFERENCED_PARAMETER(csq);
    RemoveEntryList(&irp->Tail.Overlay.ListEntry);
}

/* PeekContext = FILE_OBJECT to match, or NULL for any. */
static IO_CSQ_PEEK_NEXT_IRP CsqPeekNext;
static PIRP CsqPeekNext(_In_ PIO_CSQ csq, _In_opt_ PIRP irp, _In_opt_ PVOID ctx)
{
    PLIST_ENTRY head = &CsqDevice(csq)->WaitList;
    PLIST_ENTRY e = irp ? irp->Tail.Overlay.ListEntry.Flink : head->Flink;

    for (; e != head; e = e->Flink) {
        PIRP next = CONTAINING_RECORD(e, IRP, Tail.Overlay.ListEntry);
        if (!ctx || IoGetCurrentIrpStackLocation(next)->FileObject == (PFILE_OBJECT)ctx)
            return next;
    }
    return NULL;
}

static IO_CSQ_ACQUIRE_LOCK CsqAcquire;
static VOID CsqAcquire(_In_ PIO_CSQ csq, _Out_ PKIRQL irql)
{
    KeAcquireSpinLock(&CsqDevice(csq)->WaitLock, irql);
}

static IO_CSQ_RELEASE_LOCK CsqRelease;
static VOID CsqRelease(_In_ PIO_CSQ csq, _In_ KIRQL irql)
{
    KeReleaseSpinLock(&CsqDevice(csq)->WaitLock, irql);
}

static IO_CSQ_COMPLETE_CANCELED_IRP CsqCompleteCanceled;
static VOID CsqCompleteCanceled(_In_ PIO_CSQ csq, _In_ PIRP irp)
{
    UNREFERENCED_PARAMETER(csq);
    irp->IoStatus.Status = STATUS_CANCELLED;
    irp->IoStatus.Information = 0;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
}

static VOID CompleteWait(_Inout_ PIRP irp, _In_ const OA2DP_FORMAT_CHANGE *ev)
{
    RtlCopyMemory(irp->AssociatedIrp.SystemBuffer, ev, sizeof(*ev));
    irp->IoStatus.Status = STATUS_SUCCESS;
    irp->IoStatus.Information = sizeof(*ev);
    IoCompleteRequest(irp, IO_NO_INCREMENT);
}

static VOID FailWaits(_Inout_ OA2DP_AUDIO_DEVICE *dev, _In_opt_ PFILE_OBJECT fo, _In_ NTSTATUS status)
{
    PIRP irp;
    while ((irp = IoCsqRemoveNextIrp(&dev->WaitCsq, fo)) != NULL) {
        irp->IoStatus.Status = status;
        irp->IoStatus.Information = 0;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
    }
}

/* Called by the WaveRT stream on a new format and on RUN / leaving RUN. The
 * latest state wins if the service has not picked up the previous one. */
extern "C" VOID Oa2dpReportStream(_Inout_ OA2DP_AUDIO_DEVICE *dev, _In_ const OA2DP_FORMAT *format,
                                  _In_ BOOLEAN running)
{
    OA2DP_FORMAT_CHANGE ev;
    KIRQL irql;
    PIRP irp;

    RtlZeroMemory(&ev, sizeof(ev));
    ev.Format = *format;
    ev.Running = running ? 1 : 0;
    KeAcquireSpinLock(&dev->Lock, &irql);
    dev->Current = *format;
    dev->Running = running;
    irp = IoCsqRemoveNextIrp(&dev->WaitCsq, NULL);
    if (!irp) {
        dev->Event = ev;
        dev->EventPending = TRUE;
    }
    KeReleaseSpinLock(&dev->Lock, irql);
    if (irp)
        CompleteWait(irp, &ev);
}

/* ---- lifetime ---------------------------------------------------------------- */

extern "C" VOID Oa2dpControlInit(_Inout_ OA2DP_AUDIO_DEVICE *dev)
{
    KeInitializeSpinLock(&dev->Lock);
    KeInitializeEvent(&dev->ControlLock, SynchronizationEvent, TRUE);
    KeInitializeSpinLock(&dev->WaitLock);
    InitializeListHead(&dev->WaitList);
    (void)IoCsqInitialize(&dev->WaitCsq, CsqInsert, CsqRemove, CsqPeekNext, CsqAcquire,
                          CsqRelease, CsqCompleteCanceled);
    DefaultFormats(&dev->Formats);
    dev->Current = dev->Formats.Formats[dev->Formats.Preferred];
}

extern "C" NTSTATUS Oa2dpControlStart(_Inout_ OA2DP_AUDIO_DEVICE *dev)
{
    NTSTATUS status;
    UNICODE_STRING ref;

    PAGED_CODE();
    if (!dev->ControlLink.Buffer) {
        RtlInitUnicodeString(&ref, OA2DP_CONTROL_NAME);
        status = IoRegisterDeviceInterface(dev->Pdo, &GUID_DEVINTERFACE_OA2DP_AUDIO, &ref,
                                           &dev->ControlLink);
        if (!NT_SUCCESS(status))
            return status;
    }
    return IoSetDeviceInterfaceState(&dev->ControlLink, TRUE);
}

/* Stop / surprise removal: no new opens; pending waits fail so the service's
 * audio thread notices. */
extern "C" VOID Oa2dpControlStop(_Inout_ OA2DP_AUDIO_DEVICE *dev)
{
    PAGED_CODE();
    if (dev->ControlLink.Buffer)
        (void)IoSetDeviceInterfaceState(&dev->ControlLink, FALSE);
    FailWaits(dev, NULL, STATUS_DELETE_PENDING);
}

/* IRP_MN_REMOVE_DEVICE: the ring goes away even if the service still holds
 * its handle (the device object outlives us until that handle closes). */
extern "C" VOID Oa2dpControlRemove(_Inout_ OA2DP_AUDIO_DEVICE *dev)
{
    PAGED_CODE();
    Oa2dpControlStop(dev);
    Oa2dpLock(&dev->ControlLock);
    dev->Removed = TRUE;
    Oa2dpRingDestroy(&dev->Ring);
    Oa2dpUnlock(&dev->ControlLock);
    if (dev->ControlLink.Buffer) {
        RtlFreeUnicodeString(&dev->ControlLink);
        RtlZeroMemory(&dev->ControlLink, sizeof(dev->ControlLink));
    }
}

/* ---- dispatch ---------------------------------------------------------------- */

extern "C" BOOLEAN Oa2dpControlIsCreate(_In_ PIRP irp)
{
    PFILE_OBJECT fo = IoGetCurrentIrpStackLocation(irp)->FileObject;
    UNICODE_STRING name, ref;

    if (!fo || fo->RelatedFileObject || !fo->FileName.Buffer)
        return FALSE;
    name = fo->FileName;
    if (name.Length >= sizeof(WCHAR) && name.Buffer[0] == L'\\') {
        name.Buffer++;
        name.Length -= sizeof(WCHAR);
        name.MaximumLength -= sizeof(WCHAR);
    }
    RtlInitUnicodeString(&ref, OA2DP_CONTROL_NAME);
    return RtlEqualUnicodeString(&name, &ref, TRUE);
}

extern "C" BOOLEAN Oa2dpControlOwns(_In_ OA2DP_AUDIO_DEVICE *dev, _In_ PIRP irp)
{
    PFILE_OBJECT fo = IoGetCurrentIrpStackLocation(irp)->FileObject;
    return fo && fo->FsContext == &dev->ControlTag;
}

static BOOLEAN CallerIsPrivileged(_In_ PIRP irp, _In_ PIO_STACK_LOCATION sp)
{
    PIO_SECURITY_CONTEXT sc = sp->Parameters.Create.SecurityContext;
    PSECURITY_SUBJECT_CONTEXT subject;
    PACCESS_TOKEN token;
    BOOLEAN ok = FALSE;

    if (irp->RequestorMode == KernelMode)
        return TRUE;
    if (!sc || !sc->AccessState)
        return FALSE;
    subject = &sc->AccessState->SubjectSecurityContext;
    SeLockSubjectContext(subject);
    token = SeQuerySubjectContextToken(subject);
    if (token) {
        if (SeTokenIsAdmin(token)) {
            ok = TRUE;
        } else {
            PTOKEN_USER user = NULL;
            if (NT_SUCCESS(SeQueryInformationToken(token, TokenUser, (PVOID *)&user)) && user) {
                ok = RtlEqualSid(user->User.Sid, SeExports->SeLocalSystemSid);
                ExFreePool(user);
            }
        }
    }
    SeUnlockSubjectContext(subject);
    return ok;
}

static NTSTATUS ControlCreate(_Inout_ OA2DP_AUDIO_DEVICE *dev, _In_ PIRP irp, _In_ PIO_STACK_LOCATION sp)
{
    NTSTATUS status = STATUS_SUCCESS;
    KIRQL irql;

    if (!dev->Initialized || dev->Removed)
        return STATUS_DELETE_PENDING;
    if (!dev->Started)
        return STATUS_DEVICE_NOT_READY;
    if (!CallerIsPrivileged(irp, sp))
        return STATUS_ACCESS_DENIED;

    Oa2dpLock(&dev->ControlLock);
    if (dev->ControlFile) {
        status = STATUS_SHARING_VIOLATION; /* one service instance */
    } else {
        dev->ControlFile = sp->FileObject;
        sp->FileObject->FsContext = &dev->ControlTag;
        /* A new consumer starts from the current state, so a service that
         * (re)starts while Windows is already playing still connects. */
        KeAcquireSpinLock(&dev->Lock, &irql);
        dev->EventPending = dev->Running;
        if (dev->Running) {
            RtlZeroMemory(&dev->Event, sizeof(dev->Event));
            dev->Event.Format = dev->Current;
            dev->Event.Running = 1;
        }
        KeReleaseSpinLock(&dev->Lock, irql);
    }
    Oa2dpUnlock(&dev->ControlLock);
    return status;
}

static VOID ControlCleanup(_Inout_ OA2DP_AUDIO_DEVICE *dev, _In_ PFILE_OBJECT fo)
{
    FailWaits(dev, fo, STATUS_CANCELLED);
    Oa2dpLock(&dev->ControlLock);
    if (dev->ControlFile == fo) {
        Oa2dpRingUnmapUser(&dev->Ring); /* we are in the service's context */
        dev->ControlFile = NULL;
    }
    Oa2dpUnlock(&dev->ControlLock);
}

static NTSTATUS SetFormats(_Inout_ OA2DP_AUDIO_DEVICE *dev, _In_ const OA2DP_FORMAT_LIST *in)
{
    OA2DP_FORMAT_LIST list = *in;
    BOOLEAN changed;
    KIRQL irql;

    if (!ListValid(&list))
        return STATUS_INVALID_PARAMETER;
    for (ULONG i = list.Count; i < OA2DP_MAX_FORMATS; i++)
        RtlZeroMemory(&list.Formats[i], sizeof(list.Formats[i]));
    KeAcquireSpinLock(&dev->Lock, &irql);
    changed = !SameFormats(&dev->Formats, &list);
    dev->Formats = list;
    KeReleaseSpinLock(&dev->Lock, irql);
    SaveValue(dev, FORMATS_VALUE, REG_BINARY, &list, sizeof(list));
    /* Only a different set makes the engine re-pick its format; a new
     * preferred index alone (after a RECONFIGURE we did because Windows chose
     * that rate) must not bounce the stream. */
    if (changed)
        Oa2dpWaveFormatsChanged(dev);
    return STATUS_SUCCESS;
}

static NTSTATUS SetName(_Inout_ OA2DP_AUDIO_DEVICE *dev, _In_ const OA2DP_ENDPOINT_NAME *in)
{
    OA2DP_ENDPOINT_NAME n = *in;
    size_t len = 0;
    NTSTATUS status;

    n.Name[RTL_NUMBER_OF(n.Name) - 1] = L'\0';
    if (!NT_SUCCESS(RtlStringCchLengthW(n.Name, RTL_NUMBER_OF(n.Name), &len)) || len == 0)
        return STATUS_INVALID_PARAMETER;

    Oa2dpLock(&dev->ControlLock);
    if (wcscmp(dev->Name, n.Name) == 0) {
        Oa2dpUnlock(&dev->ControlLock);
        return STATUS_SUCCESS;
    }
    RtlStringCchCopyW(dev->Name, RTL_NUMBER_OF(dev->Name), n.Name);
    Oa2dpUnlock(&dev->ControlLock);

    SaveValue(dev, NAME_VALUE, REG_SZ, n.Name, (ULONG)((len + 1) * sizeof(WCHAR)));
    /* Sound settings shows "<endpoint> (<device friendly name>)". */
    status = IoSetDevicePropertyData(dev->Pdo, &DEVPKEY_Device_FriendlyName, LOCALE_NEUTRAL,
                                     PLUGPLAY_PROPERTY_PERSISTENT, DEVPROP_TYPE_STRING,
                                     (ULONG)((len + 1) * sizeof(WCHAR)), n.Name);
    if (!NT_SUCCESS(status))
        KdPrintEx((DPFLTR_IHVAUDIO_ID, DPFLTR_WARNING_LEVEL,
                   "oa2dpaudio: setting the friendly name failed 0x%08X\n", status));
    /* The endpoint builder reads names when the KS interfaces arrive, so
     * re-register the filters as soon as no stream is open. */
    Oa2dpLock(&dev->FilterLock);
    dev->NameChanged = TRUE;
    Oa2dpUnlock(&dev->FilterLock);
    Oa2dpScheduleRebuild(dev);
    return STATUS_SUCCESS;
}

static NTSTATUS WaitFormat(_Inout_ OA2DP_AUDIO_DEVICE *dev, _Inout_ PIRP irp, _In_ ULONG outLen)
{
    OA2DP_FORMAT_CHANGE ev;
    KIRQL irql;

    if (outLen < sizeof(ev)) {
        irp->IoStatus.Status = STATUS_BUFFER_TOO_SMALL;
        irp->IoStatus.Information = 0;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
        return STATUS_BUFFER_TOO_SMALL;
    }
    KeAcquireSpinLock(&dev->Lock, &irql);
    if (dev->EventPending) {
        ev = dev->Event;
        dev->EventPending = FALSE;
        KeReleaseSpinLock(&dev->Lock, irql);
        CompleteWait(irp, &ev);
        return STATUS_SUCCESS;
    }
    /* Marks the IRP pending (or completes it at once if already cancelled). */
    IoCsqInsertIrp(&dev->WaitCsq, irp, NULL);
    KeReleaseSpinLock(&dev->Lock, irql);
    return STATUS_PENDING;
}

static NTSTATUS ControlIoctl(_Inout_ OA2DP_AUDIO_DEVICE *dev, _Inout_ PIRP irp, _In_ PIO_STACK_LOCATION sp)
{
    ULONG code = sp->Parameters.DeviceIoControl.IoControlCode;
    ULONG inLen = sp->Parameters.DeviceIoControl.InputBufferLength;
    ULONG outLen = sp->Parameters.DeviceIoControl.OutputBufferLength;
    PVOID buf = irp->AssociatedIrp.SystemBuffer;
    NTSTATUS status;
    ULONG_PTR info = 0;
    KIRQL irql;

    if (dev->Removed) {
        status = STATUS_DELETE_PENDING;
        goto done;
    }
    switch (code) {
    case IOCTL_OA2DP_AUDIO_MAP_RING: {
        OA2DP_RING_MAPPING m;
        if (outLen < sizeof(m) || !buf) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        Oa2dpLock(&dev->ControlLock);
        status = dev->Removed ? STATUS_DELETE_PENDING : Oa2dpRingMapUser(&dev->Ring, &m);
        Oa2dpUnlock(&dev->ControlLock);
        if (NT_SUCCESS(status)) {
            RtlCopyMemory(buf, &m, sizeof(m));
            info = sizeof(m);
        }
        break;
    }
    case IOCTL_OA2DP_AUDIO_UNMAP_RING:
        Oa2dpLock(&dev->ControlLock);
        Oa2dpRingUnmapUser(&dev->Ring);
        Oa2dpUnlock(&dev->ControlLock);
        status = STATUS_SUCCESS;
        break;
    case IOCTL_OA2DP_AUDIO_SET_FORMATS:
        if (inLen < sizeof(OA2DP_FORMAT_LIST) || !buf) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        status = SetFormats(dev, (const OA2DP_FORMAT_LIST *)buf);
        break;
    case IOCTL_OA2DP_AUDIO_SET_LATENCY:
        if (inLen < sizeof(OA2DP_LATENCY) || !buf) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        KeAcquireSpinLock(&dev->Lock, &irql);
        dev->Latency = *(const OA2DP_LATENCY *)buf;
        KeReleaseSpinLock(&dev->Lock, irql);
        status = STATUS_SUCCESS;
        break;
    case IOCTL_OA2DP_AUDIO_WAIT_FORMAT:
        return WaitFormat(dev, irp, buf ? outLen : 0);
    case IOCTL_OA2DP_AUDIO_SET_NAME:
        if (inLen < sizeof(OA2DP_ENDPOINT_NAME) || !buf) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        status = SetName(dev, (const OA2DP_ENDPOINT_NAME *)buf);
        break;
    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }
done:
    irp->IoStatus.Status = status;
    irp->IoStatus.Information = info;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return status;
}

extern "C" NTSTATUS Oa2dpControlDispatch(_Inout_ OA2DP_AUDIO_DEVICE *dev, _Inout_ PIRP irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(irp);
    NTSTATUS status = STATUS_SUCCESS;

    switch (sp->MajorFunction) {
    case IRP_MJ_CREATE:
        status = ControlCreate(dev, irp, sp);
        break;
    case IRP_MJ_CLEANUP:
        ControlCleanup(dev, sp->FileObject);
        break;
    case IRP_MJ_CLOSE:
        break;
    case IRP_MJ_DEVICE_CONTROL:
        return ControlIoctl(dev, irp, sp);
    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }
    irp->IoStatus.Status = status;
    irp->IoStatus.Information = 0;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return status;
}
