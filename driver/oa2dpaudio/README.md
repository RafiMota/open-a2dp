# oa2dpaudio.sys — virtual render endpoint

**Status: sources complete, consistent with the service; not yet compiled
or run.**

Each headset served by `oa2dp.sys` gets a child device `OA2DP\Render\<address>`.
This driver loads on it and shows Windows a normal "Headphones" render
endpoint. Audio the engine renders is copied into a shared
[`oa2dp_pcm_ring`](../../core/include/oa2dp/pcm_ring.h) that `oa2dpsvc.exe`
maps, encodes and sends. The consumer side is `AudioEndpoint` in
[`service/src/driver_io.cpp`](../../service/src/driver_io.cpp); the IOCTL
contract is [`oa2dp_ioctl.h`](../common/oa2dp_ioctl.h).

## Files

| File | Origin | What it does |
|------|--------|--------------|
| `adapter.cpp` | SYSVAD `adapter.cpp` + install code of `common.cpp` | DriverEntry, AddDevice, StartDevice, PnP; installs/removes the wave + topology subdevices; routes control IRPs; re-registers the filters after a name change |
| `minwavert.cpp/.h` | SYSVAD `minwavert.*` | WaveRT miniport: formats from the service, data intersection, `KSPROPERTY_PIN_PROPOSEDATAFORMAT(2)`, `KSEVENT_PINCAPS_FORMATCHANGE` |
| `minwavertstream.cpp/.h` | SYSVAD `minwavertstream.*` | Stream: buffer allocation, 1 ms position timer that pumps the ring, positions, packet notifications, presentation position, latency |
| `mintopo.cpp` | SYSVAD `mintopo.cpp`/`basetopo.cpp` | Two-pin topology filter (bridge → headphones) |
| `wavtable.h`, `toptable.h` | SYSVAD `bthhfpspeakerwavtable.h`, `speakertoptable.h` | Filter descriptors |
| `newdelete.cpp/.h` | SYSVAD `NewDelete.*` | Kernel `new`/`delete` |
| `control.cpp` | OpenA2DP | Control interface (`GUID_DEVINTERFACE_OA2DP_AUDIO`), IOCTLs, persisted settings |
| `ring.c`, `oa2dpaudio.h` | OpenA2DP | Ring producer and user mapping; shared state |
| `oa2dpaudio.inf` | OpenA2DP (layout as in SYSVAD's INF) | Matches `OA2DP\Render` |

SYSVAD files keep Microsoft's header plus a note on what changed
(Windows-driver-samples commit `2dc3fd3a`, MS-PL, text in
[`LICENSE.MS-PL`](LICENSE.MS-PL)). Removed from SYSVAD: every other endpoint
(mic arrays, HDMI, S/PDIF, USB/Bluetooth sideband, keyword spotter),
offload, loopback, audio modules, APO/effects, DRM, PoFx, ETW, tone
generator, file saving, KMDF.

## How the plan was implemented

1. **Simple render endpoint.** Wave filter: host pin (one instance) → DAC
   node → bridge pin; topology filter: bridge in → `KSNODETYPE_HEADPHONES`.
   Volume and mute are the audio engine's software endpoint volume.
2. **Formats** come from `IOCTL_OA2DP_AUDIO_SET_FORMATS`. The KS data
   ranges are only an envelope (8–192 kHz, 1–2 channels, float32 or 16/24-bit
   PCM); `IsFormatSupported`, `DataRangeIntersection`,
   `PROPOSEDATAFORMAT` (get = preferred, set = check) and
   `PROPOSEDATAFORMAT2` answer from the list. Float32 is offered first. A
   different set (not just a different preferred index) raises
   `KSEVENT_PINCAPS_FORMATCHANGE`, which makes the engine re-pick its format.
   The last list is stored in the device key (`Oa2dpFormats`) so the
   endpoint looks right before the service connects; the default is
   48 / 44.1 kHz stereo float.
3. **Pump.** The stream keeps positions in frames from the elapsed RUN time
   (no drift, never half a frame). A 1 ms high-resolution timer (SYSVAD's
   hardware emulation, here also in polled mode) calls `Oa2dpRingPump()`
   with the frames played since the last tick. Float32 is copied as is;
   16/24-bit PCM becomes 24-bit values in int32 (`OA2DP_RING_FMT_S32`).
   `OA2DP_RING_FLAG_RUNNING` follows `SetState(RUN)` / leaving RUN.
4. **Format change.** The ring is allocated once for 2 channels and never
   moves. A stream with another format rewrites only the header
   (rate, channels, sample format; positions keep counting), sets
   `OA2DP_RING_FLAG_FORMAT_CHANGED` and completes a pending
   `IOCTL_OA2DP_AUDIO_WAIT_FORMAT`. WAIT_FORMAT also completes on RUN
   (`Running = 1`) and when leaving RUN (`Running = 0`), which the service
   uses to connect on play and to suspend when idle. The latest state wins
   if the service has not picked up the previous one; a new control handle
   starts with the current state if a stream is running.
5. **Latency** = `TransportUs + DeviceUs` from `IOCTL_OA2DP_AUDIO_SET_LATENCY`:
   reported as `KSRTAUDIO_HWLATENCY.CodecDelay` (the engine adds it to the
   stream latency) and subtracted from the
   `KSPROPERTY_RTAUDIO_PRESENTATION_POSITION` the stream returns, so the
   audio clock shows what the listener hears.
6. **Control interface.** Deviation from the earlier plan (a KMDF
   miniport-mode control device): a control device object cannot carry a
   per-PDO device interface, and the service picks the endpoint of a headset
   by finding `#<address>#` in the interface path, which only a PDO
   interface has; in KMDF miniport mode the framework also does not dispatch
   I/O. So `GUID_DEVINTERFACE_OA2DP_AUDIO` is registered on our PDO with the
   reference string `Oa2dpControl`, and `adapter.cpp` hooks
   `IRP_MJ_CREATE/CLEANUP/CLOSE/DEVICE_CONTROL`: a create with that
   reference string, and later IRPs on the FILE_OBJECT we tagged, go to
   `control.cpp`; everything else goes to PortCls unchanged. The driver
   needs no KMDF.
7. **Friendly name.** `IOCTL_OA2DP_AUDIO_SET_NAME` stores the name
   (`Oa2dpName`), sets `DEVPKEY_Device_FriendlyName` on the PDO (Sound
   settings shows "Headphones (<name>)") and the KS interfaces' friendly
   name, then re-registers the filters once no stream is open so the
   endpoint builder picks the name up immediately.

`oa2dp.sys` now reports `UniqueID` for the child PDO; without it the PnP
manager would prefix the instance ID and the service would not find the
endpoint.

## Security

The device object must keep the standard audio ACL (the audio engine runs
as LOCAL SERVICE and opens the KS filters), so the control path checks the
caller itself: only SYSTEM or an elevated administrator may open it, and only
one handle at a time. The ring is mapped only into the process that sent
`MAP_RING`; it is unmapped on `UNMAP_RING`, on handle cleanup, and on device
removal (attaching to that process if needed) before the pages are freed.
The service validates the header against the mapping size
(`oa2dp_ring_validate`) before reading.

## Open points

- Not compiled yet. Building needs Visual Studio 2022 with the C++ tools,
  the Windows SDK and the WDK of the same version (see `BUILDING.md`).
- Whether `IoSetDevicePropertyData(DEVPKEY_Device_FriendlyName)` is
  accepted for the PDO has to be checked on a real system; a failure is
  logged and ignored (the endpoint then shows the INF description).
- When Windows plays while the headset is not streaming, the ring fills up
  (new audio is dropped and counted in `overruns`). The service starts
  reading that stale audio, adding up to ~340 ms of latency. The service
  should set `read_pos = write_pos` before its first read; that is a
  service-side change.
- Jack description (connected/disconnected state) is not reported; the
  endpoint is always shown as available while the device node exists.
