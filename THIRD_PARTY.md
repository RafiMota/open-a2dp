# Third-party components

Fetched by `scripts/fetch-deps.sh`, not committed. Verify the licence of the
exact revision you ship.

| Component | Used for | Licence | Notes |
|-----------|----------|---------|-------|
| [libsbc](https://git.kernel.org/pub/scm/bluetooth/sbc.git) (BlueZ) | SBC, FastStream | LGPL-2.1-or-later | Built as `sbc.dll` so users can replace it. |
| [libopenaptx](https://github.com/pali/libopenaptx) | aptX, aptX HD, aptX LL | GPL-3.0-or-later (per its README) | Any distributed build that includes it must be offered under GPL-3.0-or-later. Our MIT code is compatible with that. Builds without it simply lack aptX. |
| [libldac](https://android.googlesource.com/platform/external/libldac) (AOSP) | LDAC + adaptive bitrate | Apache-2.0 | Keep its NOTICE file in releases. |
| Media Foundation AAC encoder | AAC | Part of Windows | Nothing is redistributed. Absent on Windows "N" editions without the Media Feature Pack. |
| [FindWDK](https://github.com/SergiusTheBest/FindWDK) | Building drivers with CMake | BSD-3-Clause | Build-time only. |
| [SYSVAD sample](https://github.com/microsoft/Windows-driver-samples/tree/main/audio/sysvad) (commit 2dc3fd3a) | Base of `oa2dpaudio.sys` (adapter, WaveRT miniport/stream, topology, tables, new/delete) | MS-PL | Committed in `driver/oa2dpaudio/` with Microsoft's headers kept and a modification note; licence text in `driver/oa2dpaudio/LICENSE.MS-PL`. Binary distributions must be under terms compatible with MS-PL. |

## Patents

Some codecs may be covered by patents in some countries (AAC is licensed
through Via LA; aptX and LDAC are owned by Qualcomm and Sony). Using the
operating system's own AAC encoder avoids redistributing an AAC
implementation. Distributors are responsible for checking the situation in
their jurisdiction; this file is not legal advice.
