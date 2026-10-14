# OpenA2DP — functional specification

Built under the rules in [CLEAN_ROOM.md](CLEAN_ROOM.md). Each requirement has an
ID so code, tests and issues can reference it. The **Status** column tracks the
current state of this repository (see [ROADMAP.md](ROADMAP.md)).

Legend: ✅ implemented and tested (unit tests or run on Windows) · 🟡 code
written and compiled where possible, not yet run end-to-end with a headset ·
⬜ not started.

## F1 — Driver replacement per device

| ID | Requirement | Status |
|----|-------------|--------|
| F1.1 | List paired Bluetooth audio devices (headphones/speakers exposing the A2DP Audio Sink service, UUID `0x110B`). | ✅ `oa2dpctl list` (verified on a real PC) |
| F1.2 | Show which driver currently serves each device: Windows inbox driver or OpenA2DP. | ✅ `oa2dpctl list` |
| F1.3 | Switch a single device to OpenA2DP, and back to the Windows driver, without affecting other devices. Requires administrator elevation. | 🟡 |
| F1.4 | Uninstalling OpenA2DP restores the Windows driver on every device. | 🟡 |
| F1.5 | Other profiles of the same device (HFP/HSP hands-free microphone, AVRCP media keys) keep working through Windows' own drivers. | 🟡 (by design: we bind only to the A2DP Sink service node) |

## F2 — Codecs (A2DP Source role)

| ID | Codec | Notes | Status |
|----|-------|-------|--------|
| F2.1 | SBC | Mandatory codec. All parameters configurable (F3). | ✅ caps, negotiation, framing, encoder (libsbc) |
| F2.2 | MPEG-2/4 AAC (AAC-LC) | Encoder: Windows Media Foundation AAC encoder (no extra licence bundled). LATM framing per A2DP. | ✅ caps, negotiation, LATM, encoder (Media Foundation) |
| F2.3 | aptX | Vendor `0x0000004F` / codec `0x0001`. No RTP header. | ✅ caps, encoder (libopenaptx) |
| F2.4 | aptX HD | Vendor `0x000000D7` / codec `0x0024`. 24-bit, RTP header. | ✅ caps, encoder (libopenaptx) |
| F2.5 | aptX Low Latency | Vendor `0x0000000A` / codec `0x0002`. Target ≈ 50 ms end-to-end. | ✅ caps, encoder (aptX bitstream) · 🟡 latency tuning |
| F2.6 | LDAC | Vendor `0x0000012D` / codec `0x00AA`. Quality presets HQ (990/909 kbps), SQ (660/606), MQ (330/303), plus Adaptive Bitrate. | ✅ caps, framing, encoder (libldac + libldacBT_abr) |
| F2.7 | FastStream | Vendor `0x0000000A` / codec `0x0001`. SBC-based, low latency. | ✅ caps, encoder (libsbc) |
| F2.8 | Pass-through of other codecs offered by the Bluetooth radio vendor (e.g. aptX Adaptive) | Only possible when an encoder is available; listed but disabled otherwise. | ⬜ |
| F2.9 | Automatic codec choice: pick the best codec both sides support, ordered by a user-editable priority list. | ✅ |

## F3 — Codec parameters

| ID | Requirement | Status |
|----|-------------|--------|
| F3.1 | SBC: bitpool (2–250, bounded by headset's max), block length (4/8/12/16), sub-bands (4/8), allocation (Loudness/SNR). | ✅ |
| F3.2 | SBC channel mode: Mono, Dual Channel, Stereo, Joint Stereo. "Dual Channel" high-bitrate mode (a.k.a. SBC XQ) supported. | ✅ |
| F3.3 | Show the resulting bitrate for the chosen SBC parameters. | ✅ |
| F3.4 | Sampling frequency: any rate both sides support (16/32/44.1/48/88.2/96 kHz, depending on codec). | ✅ |
| F3.5 | Bit depth 16 or 24 bit where the codec supports it (aptX HD, LDAC). | ✅ encoders take 24-bit input |
| F3.6 | Mono or stereo output. | ✅ |
| F3.7 | AAC: bitrate and VBR on/off within the headset's advertised max. | ✅ bitrate · ⬜ VBR (Media Foundation encoder is CBR-only) |
| F3.8 | LDAC: HQ / SQ / MQ / Adaptive. | ✅ |
| F3.9 | Adaptive bitrate on poor links: lower SBC bitpool / LDAC quality / AAC bitrate when the transmit queue grows, raise it again when it drains. | ✅ controller · 🟡 integration |
| F3.10 | Settings are stored per device and applied when the device (re)connects. | ✅ format · 🟡 registry backend (built, not run) |

## F4 — Audio path

| ID | Requirement | Status |
|----|-------------|--------|
| F4.1 | Each connected headset appears as a Windows render endpoint with its own friendly name. | 🟡 driver written (`oa2dpaudio.sys`: endpoint per `OA2DP\Render\<address>`, name from `IOCTL_OA2DP_AUDIO_SET_NAME`) · not yet built or run |
| F4.2 | The endpoint advertises the formats the negotiated codec accepts; Windows mixes into one of them. | 🟡 service side · 🟡 driver side written (format list from `IOCTL_OA2DP_AUDIO_SET_FORMATS`, float32 preferred), not yet built or run |
| F4.3 | Dynamic sample-rate switching: when the endpoint format changes (e.g. exclusive-mode player at 96 kHz) the stream is suspended, reconfigured (AVDTP RECONFIGURE) and restarted, avoiding resampling. | ✅ protocol · 🟡 integration |
| F4.4 | Accurate latency reporting to Windows, including AVDTP Delay Reporting from the headset (for lip-sync). | ✅ protocol · 🟡 integration |
| F4.5 | Headset-initiated connections (headset connects to the PC) as well as PC-initiated ones. | 🟡 |

## F5 — Protocol conformance (Bluetooth SIG)

| ID | Requirement | Status |
|----|-------------|--------|
| F5.1 | AVDTP 1.3 signalling: DISCOVER, GET_CAPABILITIES, GET_ALL_CAPABILITIES, SET_CONFIGURATION, GET_CONFIGURATION, RECONFIGURE, OPEN, START, CLOSE, SUSPEND, ABORT, SECURITY_CONTROL (reject), DELAYREPORT, GENERAL_REJECT. | ✅ |
| F5.2 | Signalling fragmentation (single/start/continue/end packets) and reassembly. | ✅ |
| F5.3 | Stream endpoint state machine (IDLE → CONFIGURED → OPEN → STREAMING → CLOSING / ABORTING). | ✅ |
| F5.4 | Act as INT (we start) and ACP (headset starts) for every procedure. | ✅ |
| F5.5 | RTP media packetisation per codec, respecting the media channel MTU. | ✅ |

## F6 — Tools

| ID | Requirement | Status |
|----|-------------|--------|
| F6.1 | Graphical manager: device list, driver switch, codec settings, live status (codec, rate, bitrate). | 🟡 `oa2dpgui`: device list and settings editor verified on a real PC; driver switch and saving run `oa2dpctl` elevated (switch needs the driver package); live status untested until the service runs |
| F6.2 | Command-line tool `oa2dpctl` with the same functions (scriptable). | ✅ list/status/sbc-bitrate · 🟡 use/restore/set (need the driver package) |
| F6.3 | Installer (WiX/MSI) that installs drivers, service and tools, and fully reverts on uninstall. | ⬜ |
| F6.4 | Diagnostic log (ETW + text) for bug reports. | 🟡 |

## Non-goals (handled by Windows)

- Hands-free (HFP/HSP) microphone: stays on Windows' hands-free driver.
- AVRCP play/pause/next keys: stay on Windows' AVRCP transport.
- LE Audio (LC3 over BLE isochronous channels): different stack, out of scope.

## Platform

- Windows 10 1809+ and Windows 11, x64 and ARM64.
- Any Bluetooth radio that uses the Microsoft Bluetooth stack (`bthport.sys`).
- Kernel drivers must be signed (attestation signing through the Microsoft
  Partner Center) before end users can install them; developers use test
  signing (`bcdedit /set testsigning on`).
