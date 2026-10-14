# Architecture

```
 Windows audio engine (audiodg)
            │  PCM (WaveRT buffer)
            ▼
 ┌──────────────────────────┐      shared PCM ring       ┌───────────────────────────┐
 │ oa2dpaudio.sys           │ ─────────────────────────▶ │ oa2dpsvc.exe (service)     │
 │ PortCls WaveRT miniport  │ ◀──── format / latency ─── │  core/  AVDTP + A2DP caps  │
 │ child PDO per headset    │                            │  codecs/ SBC AAC aptX LDAC │
 └──────────────────────────┘                            │  adaptive bitrate          │
            ▲ child PDO                                  └─────────────┬─────────────┘
 ┌──────────────────────────┐  IOCTL: open/send/recv L2CAP           │
 │ oa2dp.sys (KMDF)         │ ◀──────────────────────────────────────┘
 │ Bluetooth profile driver │
 │ BRB → L2CAP PSM 0x19     │
 └──────────────────────────┘
            ▲ installed on BTHENUM\{0000110b-…} (A2DP Audio Sink service node)
 bthport.sys  (Microsoft Bluetooth stack, unchanged)
```

## Why this split

* **Kernel code stays tiny.** `oa2dp.sys` only moves L2CAP bytes; it never
  parses AVDTP or runs codecs. Bugs in codec libraries crash a service that
  restarts, not the machine.
* **Codec libraries are user-mode.** libsbc, libldac, libopenaptx and Media
  Foundation (AAC) all run in user mode, need floating point, and have
  licences that are easier to honour in a separate DLL.
* **The protocol core is portable C** (`core/`), with no Windows headers. It is
  unit-tested on any OS and can be fuzzed.

## Components

### `core/` — portable protocol library (C99)
| File | Responsibility |
|------|----------------|
| `avdtp.c` | Signalling encode/decode, fragmentation/reassembly, transaction labels, stream-endpoint state machine, INT and ACP roles. Transport-agnostic: the caller supplies a `send` callback and feeds received L2CAP SDUs. |
| `a2dp_codecs.c` | Codec Information Elements for SBC, AAC, aptX, aptX HD, aptX LL, FastStream, LDAC: parse, build, intersect, choose best configuration from user preferences. |
| `media.c` | RTP header and per-codec media payload headers (fills packets up to the MTU, fragments oversized frames); MPEG-4 LATM `AudioMuxElement` writer for AAC; SBC frame length / bitrate formulas; adaptive-bitrate controller driven by transmit queue depth. |
| `pcm_ring.c` | Lock-free single-producer/single-consumer PCM ring shared by the audio driver and the service; float → 24-bit conversion, downmix. |
| `settings.c` | Per-device settings model and its `key=value` text serialisation (stored as a registry REG_SZ by the service). |

### `codecs/` — encoder adapters
A common `oa2dp_encoder` vtable (`init`, `encode`, `frame_info`,
`set_quality`, `destroy`). Each adapter is compiled only when its library is
present (`OA2DP_WITH_LIBSBC`, `OA2DP_WITH_LIBLDAC`, `OA2DP_WITH_OPENAPTX`,
`OA2DP_WITH_MFAAC`).

### `driver/oa2dp` — KMDF Bluetooth profile driver
* Matches the A2DP Audio Sink service node of a paired device. The manager
  selects this driver for that node only (F1.3), so HFP and AVRCP nodes stay
  on Windows drivers.
* Queries the local Bluetooth interface (`GUID_BTHDDI_PROFILE_DRIVER_INTERFACE`),
  allocates BRBs, opens outgoing L2CAP channels to PSM `0x0019`, and registers
  an L2CAP server so the headset can connect to us.
* Exposes `GUID_DEVINTERFACE_OA2DP` with buffered IOCTLs (see
  `driver/common/oa2dp_ioctl.h`).
* Creates a child PDO `OA2DP\Render` so `oa2dpaudio.sys` loads per headset.

### `driver/oa2dpaudio` — virtual render endpoint
* PortCls WaveRT miniport derived from Microsoft's SYSVAD sample (MS-PL):
  one wave filter (host render pin → DAC → bridge) and one topology filter
  (bridge → headphones), one stream at a time.
* A 1 ms timer copies newly played frames from the WaveRT cyclic buffer into
  an MDL-backed ring shared with the service (float32 as is, 16/24-bit PCM
  as 24-bit values in int32).
* Format list, latency and friendly name are set by the service through
  private IOCTLs after codec negotiation; a format-list change raises
  `KSEVENT_PINCAPS_FORMATCHANGE`, a new stream format rewrites the ring
  header in place and completes `IOCTL_OA2DP_AUDIO_WAIT_FORMAT`.
* `GUID_DEVINTERFACE_OA2DP_AUDIO` is registered on the PDO (so its path
  carries the headset address) and served by hooking PortCls's
  create/cleanup/close/device-control dispatch; only SYSTEM and
  administrators may open it.

### `service/` — `oa2dpsvc.exe`
1. Watches for `GUID_DEVINTERFACE_OA2DP` arrivals (`CM_Register_Notification`).
2. Loads the device's settings, opens the signalling channel, runs
   DISCOVER → GET_ALL_CAPABILITIES → choose codec → SET_CONFIGURATION →
   OPEN, then opens the media channel.
3. When the audio endpoint starts: START, then the encode loop
   (read PCM → convert/resample → encode → packetise → send).
4. Handles SUSPEND/RECONFIGURE on format changes and DELAYREPORT updates.

### `tools/common`, `tools/oa2dpctl`, `tools/oa2dpgui`
`tools/common/manage.cpp` lists devices (SetupAPI + `BluetoothGetDeviceInfo`),
shows the bound driver, switches drivers (`DiInstallDevice`), and reads and
writes the settings and status keys below. `oa2dpctl` is the command line on
top of it. `oa2dpgui` is a plain Win32 window (comctl32 v6 via an embedded
manifest, no other runtime) on the same library; it runs as the user and, for
anything that writes HKLM or switches a driver, starts `oa2dpctl.exe` with the
`runas` verb (or does it in-process when already elevated). It polls the
status key once a second and rescans devices on `WM_DEVICECHANGE`.

## Data formats

* Device key: Bluetooth address as 12 upper-case hex digits, e.g. `001A7DDA7113`.
* Settings live in `HKLM\SOFTWARE\OpenA2DP\Devices\<address>`, value
  `Settings` (REG_SZ) in the format produced by `oa2dp_settings_format()`.
