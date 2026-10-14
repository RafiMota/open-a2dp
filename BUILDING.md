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

Both drivers build with WDK 10.0.26100 (MSVC 2022, FindWDK); the
`drivers` job in `.github/workflows/ci.yml` does exactly this on
`windows-2022` and runs Inf2Cat. `oa2dpaudio.inf` needs Windows 10 2004 or
later.

### Without a local toolchain: CI artifacts

Every push builds two artifacts: `drivers-x64` (`oa2dp\` and `oa2dpaudio\`
folders with `.sys`, `.inf` and an unsigned `.cat`) and `user-mode-x64`
(`oa2dpsvc.exe`, `oa2dpctl.exe`, `oa2dpgui.exe`, codecs linked in).

```bash
gh run download <run-id> -D dist
```

### Test install (development machines only)

1. Turn **Secure Boot off** in the firmware setup (test-signed drivers do
   not load with it on). If BitLocker protects the system drive, suspend it
   first (`manage-bde -protectors -disable C: -RebootCount 2`) or have the
   recovery key at hand.
2. From an elevated prompt: `bcdedit /set testsigning on`, then reboot.
3. From an elevated PowerShell in the repository:
   `.\scripts\install-dev.ps1` — creates and trusts a local test
   certificate, signs the drivers and catalogs from `dist\`, adds both
   packages to the driver store, installs the service and tools to
   `%ProgramFiles%\OpenA2DP` and starts the service.
4. `oa2dpctl list`, then `oa2dpctl use <n>` (or the GUI) switches one
   headset to OpenA2DP. The service log is
   `%ProgramData%\OpenA2DP\oa2dpsvc.log`.

`.\scripts\install-dev.ps1 -Uninstall` restores Windows' driver on every
headset and removes the service, the packages and the certificate; then
`bcdedit /set testsigning off` and Secure Boot back on.

For end users the drivers must be **attestation-signed** through the
Microsoft Partner Center (needs an EV code-signing certificate). That is a
project-level step for releases.

## 5. Running the service by hand

```
oa2dpsvc.exe --console
```

Logs go to the console and `%ProgramData%\OpenA2DP\oa2dpsvc.log`.
