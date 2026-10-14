# Roadmap

What exists today (2026-10-14) and what is left, in order. Requirement IDs
refer to [SPEC.md](SPEC.md).

## Done

- **Protocol core** (`core/`): AVDTP 1.3 signalling with fragmentation, INT and
  ACP roles, full stream-endpoint state machine, delay reporting,
  reconfigure; codec capability parsing/building/negotiation for SBC, AAC,
  aptX, aptX HD, aptX LL, FastStream, LDAC; RTP/LATM packetisation and
  fragmentation; SBC bitrate maths; adaptive-bitrate controller; shared PCM
  ring; settings format. 377 unit checks, clean under UBSan.
- **Encoders** (`codecs/`): SBC, FastStream (libsbc), aptX/HD/LL
  (libopenaptx), LDAC + ABR (libldac), AAC (Media Foundation) — all
  smoke-tested on Windows with expected bitrates.
- **Service** (`service/`): device arrival/removal, per-headset session,
  INT/ACP configuration with codec fallback, delay reports, dynamic sample
  rate, idle suspend, adaptive bitrate, live status in the registry, log
  file. Compiles and links; not yet run against the drivers.
- **oa2dpctl**: list/status/sbc-bitrate verified on a real PC;
  use/restore/get/set/reset implemented.
- **oa2dpgui** (F6.1): Win32 manager with the device list and driver switch,
  per-headset codec order and settings, live SBC bitrate, and a status bar
  fed by the service. Shares `tools/common` with oa2dpctl; privileged actions
  run oa2dpctl elevated. Lists the paired headsets on a real PC; switching and
  live status wait for the drivers.
- **oa2dp.sys**: KMDF profile driver source + INF (L2CAP open/accept/close,
  send/receive, TX queue stats, audio child PDO with a unique instance ID).
  Not yet compiled.
- **oa2dpaudio.sys** (F4.1, F4.2): PortCls/WaveRT render endpoint derived
  from SYSVAD's simple render endpoint, INF for `OA2DP\Render`, CMake target.
  Formats from the service, ring pump, WAIT_FORMAT on stream start/stop and
  format change, latency in `GetHWLatency` and the presentation position,
  friendly name, control interface on the PDO. Not yet compiled.

## Next

1. **Build `oa2dp.sys` and `oa2dpaudio.sys` with the WDK** (needs Visual
   Studio 2022 + Windows SDK + WDK of the same version) and fix whatever the
   compiler, Driver Verifier and Static Driver Verifier report. Test-sign,
   bind to one headset, run `oa2dpsvc --console`, confirm
   DISCOVER/GET_CAPABILITIES against a real headset (signalling works before
   any audio exists), then check that the endpoint appears with the
   headset's name and that the service maps the ring.
2. **End-to-end audio** with SBC, then each codec; measure latency, tune
   `kAbr*` constants and the aptX LL path (F2.5, F3.9, F4.4). The service
   should drop stale ring contents when it starts streaming (see
   `driver/oa2dpaudio/README.md`, "Open points").
3. **Headset-initiated connections**: check whether a profile driver can
   register PSM 0x19 next to Windows' stack (F4.5). If not, the service's
   ACL-connected polling already covers reconnection.
4. **Installer** (WiX): driver package, service, oa2dpctl, oa2dpgui (Start
   menu shortcut), uninstall that runs
   `oa2dpctl restore all` first (F1.4, F6.3).
5. **Release engineering**: attestation signing, ARM64 build, CI on
   `windows-latest`.

## Later / research

- AAC VBR and higher bitrates through an alternative AAC encoder (F3.7).
- Other vendor codecs where an open encoder exists (F2.8): LC3plus HR, Opus.
- A2DP sink role (PC receives audio from a phone).
- AVRCP absolute volume from the endpoint volume slider.
- Fuzzing `oa2dp_avdtp_input` and `oa2dp_codec_parse` (libFuzzer harness).
