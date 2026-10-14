# OpenA2DP

Open-source replacement for the Windows 10/11 Bluetooth A2DP audio driver,
with high-quality codecs and full control over codec parameters.

**Status: pre-alpha.** The protocol core and every encoder work and are
tested; the kernel drivers are written but have not been built with the WDK
or run against a headset yet. See [docs/ROADMAP.md](docs/ROADMAP.md).

| | |
|---|---|
| Codecs | SBC (incl. Dual Channel "XQ"), AAC, aptX, aptX HD, aptX Low Latency, LDAC (HQ/SQ/MQ/adaptive), FastStream |
| Settings | per device: codec priority, sample rate, bit depth, channel mode, SBC bitpool/blocks/subbands/allocation, AAC bitrate, LDAC quality, adaptive bitrate, follow the player's sample rate |
| Scope | replaces only the A2DP part of one headset at a time; hands-free mic (HFP) and media keys (AVRCP) stay on Windows' drivers |
| Provenance | clean-room: built from Bluetooth SIG specs, public docs and open-source stacks — see [docs/CLEAN_ROOM.md](docs/CLEAN_ROOM.md) |

## How it fits together

```
Windows audio ─▶ oa2dpaudio.sys ─(shared PCM ring)─▶ oa2dpsvc.exe ─▶ oa2dp.sys ─▶ bthport.sys ─▶ headset
                 virtual endpoint                   AVDTP + codecs    L2CAP only
```

Details: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) · Requirements and their
status: [docs/SPEC.md](docs/SPEC.md).

## Repository layout

| Path | What |
|------|------|
| `core/` | Portable C99: AVDTP 1.3 signalling, A2DP codec capabilities and negotiation, RTP/LATM packetisation, adaptive bitrate, PCM ring, settings. 377 unit checks. |
| `codecs/` | Encoder adapters: libsbc, libopenaptx, libldac, Media Foundation AAC. |
| `service/` | `oa2dpsvc.exe`, the Windows service that runs one session per headset. |
| `tools/oa2dpctl/` | Command-line manager: list headsets, switch driver, edit settings, live status. |
| `tools/oa2dpgui/` | The same as a small Win32 window: headset list, driver switch, codec settings with live SBC bitrate, status bar. |
| `tools/common/` | Device and registry logic shared by both tools. |
| `driver/oa2dp/` | KMDF Bluetooth profile driver (L2CAP transport) + INF. |
| `driver/oa2dpaudio/` | Virtual render endpoint: PortCls/WaveRT miniport derived from SYSVAD (MS-PL), ring producer, control IOCTLs. Builds in CI; not yet run. |

## Quick start (developers)

```bash
sh scripts/fetch-deps.sh
ZIG=/path/to/zig sh scripts/zig-build.sh
```

That builds and runs the unit tests and the encoder smoke test, and produces
`oa2dpsvc.exe`, `oa2dpctl.exe` and `oa2dpgui.exe` — no Visual Studio needed. Drivers need
Visual Studio + WDK: see [BUILDING.md](BUILDING.md).

```
oa2dpctl list
oa2dpctl set 0 codecs=ldac,aptx_hd,aac,sbc ldac_quality=hq sample_rate=96000
oa2dpctl set 0 codecs=sbc channel_mode=dual sbc_bitpool=47
oa2dpctl sbc-bitrate dual 47 44100
oa2dpctl use 0          # switch headset 0 to OpenA2DP (admin, needs drivers)
oa2dpctl restore all    # back to Windows' driver
```

## Contributing

Read [CONTRIBUTING.md](CONTRIBUTING.md) and the clean-room rules in
[docs/CLEAN_ROOM.md](docs/CLEAN_ROOM.md) first. The most valuable help right
now: building the drivers with the WDK, and testing with real headsets.

## Licence

MIT for this repository's code ([LICENSE](LICENSE)). Codec libraries keep
their own licences — **builds that include libopenaptx (aptX) must be
distributed under GPL-3.0-or-later terms**. See [THIRD_PARTY.md](THIRD_PARTY.md).

---

## Português (resumo)

OpenA2DP é um substituto de código aberto para o driver de áudio Bluetooth
A2DP do Windows 10/11, com codecs de alta qualidade (SBC/SBC XQ, AAC, aptX,
aptX HD, aptX LL, LDAC, FastStream) e ajuste fino por dispositivo. Foi feito
em modelo *clean room*: só especificações do Bluetooth SIG, documentação
pública e projetos open source — nenhum binário proprietário foi aberto.

Estado atual: pré-alfa. O núcleo do protocolo e todos os codificadores
funcionam e têm testes; os drivers de kernel estão escritos mas ainda não
foram compilados com o WDK nem testados com um fone. Veja o
[roadmap](docs/ROADMAP.md) e como contribuir em [CONTRIBUTING.md](CONTRIBUTING.md).
