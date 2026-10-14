# Building

## 1. Dependencies

```bash
sh scripts/fetch-deps.sh   # libsbc, libopenaptx, libldac, FindWDK -> third_party/
```

Any adapter whose library is missing is simply left out of the build.

## 2. User-mode parts with Zig (fastest, verified)

[Zig](https://ziglang.org/download/) ships clang plus MinGW-w64 headers, so it
builds the Windows user-mode binaries from any OS:

```bash
ZIG=/path/to/zig sh scripts/zig-build.sh
```

Output in `build/zig/`: `oa2dp_tests.exe` (host), `test_encoders.exe`,
`oa2dpsvc.exe`, `oa2dpctl.exe`, `oa2dpgui.exe` (keep it next to `oa2dpctl.exe`,
which it starts elevated to switch drivers and save settings). This is the configuration used to verify the
current code (zig 0.15.2).

Tip on Windows: if the zig path is very long, call it through its 8.3 short
path or move it to e.g. `C:\zig` — zig re-spawns itself and Windows' 260-char
path limit applies.

## 3. CMake (Visual Studio or clang-cl)

```bash
cmake -S . -B build/vs -G "Visual Studio 17 2022" -A x64
cmake --build build/vs --config Release
ctest --test-dir build/vs -C Release
```

Not yet verified with MSVC. Known gaps: libsbc's headers use POSIX
`ssize_t` (patched with a define in CMakeLists.txt); if MSVC rejects other
GCC-isms in libsbc, use `-T ClangCL`.

Core tests only (Linux/macOS, CI):

```bash
cmake -S . -B build/host && cmake --build build/host && ctest --test-dir build/host
```

## 4. Drivers (Visual Studio + WDK)

Requirements: Visual Studio 2022, Windows SDK and **WDK** of the same
version, Spectre-mitigated libraries.

```bash
cmake -S . -B build/drv -G "Visual Studio 17 2022" -A x64 -DOA2DP_BUILD_DRIVERS=ON
cmake --build build/drv --config Release --target oa2dp oa2dpaudio
```

Neither driver has been compiled yet; expect a first round of fixes.
`oa2dpaudio.inf` needs Windows 10 2004 or later.

### Test signing (development machines only)

```
bcdedit /set testsigning on        (reboot)
makecert / New-SelfSignedCertificate -> sign oa2dp.sys and oa2dp.cat
pnputil /add-driver driver\oa2dp\oa2dp.inf
pnputil /add-driver driver\oa2dpaudio\oa2dpaudio.inf
oa2dpctl use <dev>
```

For end users the drivers must be **attestation-signed** through the
Microsoft Partner Center (needs an EV code-signing certificate). That is a
project-level step for releases.

## 5. Running the service by hand

```
oa2dpsvc.exe --console
```

Logs go to the console and `%ProgramData%\OpenA2DP\oa2dpsvc.log`.
