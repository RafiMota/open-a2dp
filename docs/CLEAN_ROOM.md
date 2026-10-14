# Clean-room provenance

OpenA2DP is an independent, clean-room implementation of a replacement
Bluetooth A2DP audio driver for Windows 10/11. It is **not** derived from,
affiliated with, or endorsed by any commercial product.

## Rules this project follows

1. **No inspection of proprietary implementations.** Nobody writing code for
   this repository may disassemble, decompile, unpack, or read the binaries,
   installers, INF files, registry layout, resources, or strings of any
   proprietary A2DP driver. That includes extracting an MSI "just to see the
   file list".
2. **Allowed inputs** for the functional specification (`docs/SPEC.md`):
   - Public marketing/product pages and press articles describing
     *user-visible behaviour* (which codecs, which settings).
   - Bluetooth SIG specifications (A2DP, AVDTP, AVRCP, GAVDP, Assigned Numbers).
   - Microsoft public documentation and samples (WDK docs, Bluetooth profile
     driver docs, SYSVAD/PortCls samples).
   - Open-source projects under their own licenses (BlueZ, PipeWire, AOSP
     Fluoride/Gabeldorsche, libsbc, libldac, libopenaptx, liblc3).
3. **Different names and UI.** The product name, wording, icons and layout are
   our own. Do not copy text, screenshots or UI arrangement.
4. **Record your sources.** Every pull request that implements protocol or
   codec behaviour must cite the public document it was built from.

## Specification provenance (initial version, 2026-10-14)

The feature list in `docs/SPEC.md` was compiled only from public descriptions
of what such drivers do for end users (codec list, configurable SBC bitpool and
channel mode, sample rate/bit depth choices, LDAC quality presets with adaptive
bitrate, aptX Low Latency, settings applied on reconnect, per-device driver
switching). Protocol details come from the Bluetooth SIG specifications and
open-source stacks listed above.

No proprietary installer was opened, extracted or analysed while writing this
repository.

If you have ever disassembled a proprietary A2DP driver, please do not
contribute code to the affected subsystem; you may still help with
documentation, testing and review of unrelated areas.
