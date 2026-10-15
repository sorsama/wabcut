# Releasing Wabcut

How a Wabcut release is versioned, built, packaged and checked before anyone else installs it. Building from source is
in [building.md](building.md); this page starts where a green `win-msvc` build ends.

## Platform status

| Platform | Build | Tests | Packages | Status |
|---|---|---|---|---|
| **Windows 10/11 x64** | `win-msvc` (MSVC 19.36+) | full suite, GPU and audio-device tests included | per-user MSI, portable ZIP | **Verified** on the development machine for every release candidate |
| **Linux x64** (Ubuntu 24.04, GCC 14) | `linux-gcc` | `ctest --preset linux-gcc` (no GPU / audio-device tests) | install tree, TGZ | **CI-ready, unverified**: `.github/workflows/unix-full.yml` builds and tests it; nobody has run the result locally |
| **macOS 15 arm64** (Homebrew LLVM 20) | `macos-clang` | `ctest --preset macos-clang` (no GPU / audio-device tests) | install tree, app bundle, TGZ | **CI-ready, unverified**: as Linux |
| Core (any OS, no Qt/FFmpeg) | `core` | `ctest --preset core` | — | Verified on Windows with MSVC and with clang-cl 21.1.8 (`/W4 /WX`, 678 tests); CI on all three |

"Verified" means built, the whole test suite run and the packages installed and smoke-tested on that platform for the
release. "CI-ready" means the workflow exists and is expected to pass, but no result has been seen yet; the
[portability notes](#portability) list what is known to differ.

## Versioning

- The version is `project(Wabcut VERSION x.y.z)` in the root `CMakeLists.txt`: semantic versioning, `0.y.z` until the
  file format and the command protocol are declared stable. A minor bump for new features, a patch bump for fixes only.
- Executables carry it in their Windows version information (`FileVersion x.y.z.<build>`, `ProductVersion x.y.z
  (<commit>)`, `ProductName Wabcut`; `cmake/packaging/version.rc.in`). `<build>` is `WABCUT_BUILD_NUMBER` (CI passes its
  run number; 0 locally) and `<commit>` the short commit at configure time.
- `wabcut-cli --version` prints it; crash reports and the diagnostic summary carry `x.y.z (<commit>)`.
- The MSI's `ProductVersion` is `x.y.z`. Its `UpgradeCode` (`9D0324E6-4508-405F-B844-7C96EBEA28D6`) never changes: a newer
  MSI replaces an installed one (major upgrade), an older one refuses to downgrade.

## Packages

`cpack --config build/win-msvc/CPackConfig.cmake` writes to `build/win-msvc/package/`:

| Package | What it is |
|---|---|
| `wabcut-<version>-windows-x64.msi` | The installer: per user, no administrator rights |
| `wabcut-<version>-windows-x64-portable.zip` | The same files in one folder: unzip anywhere and run `bin\wabcut.exe` |

Both contain the install tree of `cmake --install` (building.md, "Install tree and ZIP"): the app, `wabcut-cli`, the AI
helpers `wabcut-infer` (whisper.cpp, with its CPU and Vulkan compute modules and the Vulkan loader) and `wabcut-embed`
(ONNX Runtime), Qt and its plugins and QML modules, the FFmpeg/x264/x265/dav1d/libvpl DLLs, the MSVC runtime, the
in-app docs (`bin/docs/`), `LICENSE.txt`, `THIRD-PARTY-NOTICES.txt` and `licenses/`. The local assistant's llama-server
and every model are downloads, on request, never part of a package.

### The MSI

- **Per user.** Installs to `%LOCALAPPDATA%\Programs\Wabcut` without an elevation prompt (Windows Installer per-user
  package: `Scope="perUser"`, Summary Information word count bit "no elevation"). The folder page lets the user choose
  another folder; the installer remembers it (`HKCU\Software\Wabcut\Installer`, `InstallRoot`) so that repair and
  uninstall find the files there. Windows Installer itself does not keep a chosen folder.
- **Start menu**: a "Wabcut" entry. **Settings > Apps**: "Wabcut" with its icon, version and uninstaller.
- **File association (opt-in).** The options page has an unticked *Open .wabcut project files with Wabcut* checkbox. Ticked,
  it registers `.wabcut` and the `Wabcut.Project` ProgID for this user (`HKCU\Software\Classes`); unticked, nothing is
  written. Silent installs opt in with `WABCUT_ASSOCIATE=1`. The app opens a project passed on its command line.
- **Uninstall removes program files only**: exactly the files, the Start menu entry and the registry values the package
  installed (Windows Installer component tracking; the package has no wildcard file removal). Projects, autosaves,
  proxies, caches, downloaded models, crash reports, settings and the AI keys in Windows Credential Manager stay. Files
  a user put into the program folder stay too.
- **Silent and managed installs**: `msiexec /i wabcut-<version>-windows-x64.msi /qn [INSTALL_ROOT="D:\Apps\Wabcut"]
  [WABCUT_ASSOCIATE=1]`; uninstall with `msiexec /x <msi or product code> /qn` or from Settings > Apps.
- **Size** (0.1.0, 2026-10-12): the MSI is 81.4 MiB and the portable ZIP 100.6 MiB, for 1,527 files and 264.8 MiB
  installed (building.md lists the tree).
- **Not signed yet.** SmartScreen warns about an unknown publisher until the project has a code-signing certificate;
  signing is the first open item of the [release checklist](#release-checklist).

The installer pages use the stock WixUI_InstallDir dialogs with Wabcut's banner and side art
(`cmake/packaging/wix/Make-InstallerArt.ps1` draws them in design language v2) and one added options page
(`cmake/packaging/wix/WabcutOptions.wxs`).

### Why WiX, and which WiX

CPack's WiX generator builds the MSI with the **WiX Toolset 5.0.2** .NET tool. It installs per user with the .NET SDK that
Visual Studio and the GitHub runners already have, so no administrator install is needed:

```powershell
./cmake/packaging/Install-WixToolset.ps1     # dotnet tool install wix 5.0.2 + UI/Util extensions, for this user
cmake --preset win-msvc                      # "Wabcut: MSI installer with WiX 5.0.2 (...)"
```

NSIS was the alternative; it would be a separately downloaded binary and a script-defined installer, where Windows
Installer gives per-user installs, rollback, exact uninstall and IT deployment natively. WiX 5.0.2 is pinned on purpose:
WiX 6 added the Open Source Maintenance Fee to its EULA and WiX 7 refuses to run until every build account accepts it
(`wix eula accept wix7`). Moving past 5.x is a project decision, not a build-script one. Without WiX, cpack still writes
the portable ZIP and the `install.package` test reports the MSI part as skipped.

## Third-party notices

`THIRD-PARTY-NOTICES.txt` is generated at configure time (`cmake/THIRD-PARTY-NOTICES.txt.in`): every component with the
version this package was built with (read from vcpkg's SPDX files), its license and its license file in `licenses/`,
FFmpeg's exact configure flags (from the built `ffmpeg -buildconf`), what is downloaded on request instead of shipped
(models, the llama.cpp runtime, the optional CUDA runtime) and, appended at install time, the list of runtime libraries
the tree really contains. The root `THIRD-PARTY-NOTICES.md` covers the bundled fonts and icons for the source tree.

Wabcut is GPL-3.0-or-later and links GPL FFmpeg, x264 and x265: binaries must come with the corresponding source. A
release is tagged in git, and the tag plus `vcpkg.json`'s baseline pin every source that went into it.

## Crash reports

Wabcut handles crashes **locally**. Nothing is uploaded, ever: there is no crash server, and Windows Error Reporting is
switched off for the process.

- On a crash (access violation, stack overflow, abort, an uncaught exception, an invalid C runtime parameter; on
  macOS/Linux a fatal signal) Wabcut writes into `%LOCALAPPDATA%\Wabcut\Wabcut\crashes\` (macOS
  `~/Library/Application Support/Wabcut/Wabcut/crashes`, Linux `~/.local/share/Wabcut/Wabcut/crashes`):
  - `wabcut-<UTC time>-<pid>.dmp` (Windows): a minidump with stacks, threads and modules only, stack memory filtered to
    what unwinding needs and module paths cut to file names (`MiniDumpFilterMemory | MiniDumpFilterModulePaths`); no heap.
    On macOS/Linux a `.txt` backtrace instead.
  - `wabcut-<UTC time>-<pid>.json`: what happened, the version, the session's diagnostics and its last 200 log lines.
    Log lines are redacted as they are recorded: absolute paths become `<path>` with their extension, API keys, tokens
    and long secrets `<redacted>`, e-mail addresses `<email>`, and the values of environment variables named like keys,
    tokens or passwords are removed wherever they appear.
- The writer runs on its own thread so even a stack overflow is recorded; everything it writes is prepared beforehand.
- A run that ends without a clean exit and without a report (killed, power loss, a hang ended from the task manager)
  leaves its session file, and is noticed too.
- **Next start**: *Wabcut closed unexpectedly* says when and what, leads to the **Recovery dashboard** (every saved,
  autosaved and backup version of the projects), opens the crash reports folder, and **Copy diagnostic summary** copies
  only the version, OS, CPU thread count, GPU and driver, graphics API and decode backends - shown in the dialog exactly as
  copied. Dismissed, it does not come back for that run. Reports stay until the user deletes them (the newest 10 are
  kept).

Code: `src/app/src/crash/` (`CrashHandler`, `CrashReports`, `SystemInfo`), the `CrashNotice` bridge and
`qml/dialogs/CrashNoticeDialog.qml`. Test: `tests/app/test_crash_safety.cpp` crashes a helper process four ways.

## First-run performance check

The first start offers a short check, once, after the Home screen and the welcome (never over them, the tour, the
crash notice or Recovery). It is optional: *Not now* (or Escape) is remembered, *Stop* ends it early with what was
measured, and Settings > Media & cache runs it again. Nothing leaves the computer.

Within a ten-second budget (each step that no longer fits is skipped and says so) it measures:

| What | How |
|---|---|
| Decoding of H.264, HEVC and ProRes 422 Proxy | a 24-frame 1080p30 sample of each is written with the export path (`ExportJob` over the synthetic test pattern; hardware encoders first), then decoded front to back with the hardware backend `VideoDecoder` picks and in software: frames per second and the backend |
| Export encoders | what an H.264 and an HEVC export would use (`probeVideoEncoder`: each hardware candidate test-encodes, then the software encoder) |
| GPU | the adapters (`gpuVendors`) and the app's graphics API and adapter (RendererInfo) |
| Audio output | whether an output device exists, and the default one's name |
| Processor | thread count |

The recommendation (`editor::recommend`, `src/editor/include/wabcut/editor/PerformanceProbe.h`) takes the slower of
H.264 and HEVC at its best backend, as a multiple of real time:

| Decoding 1080p | Make proxies | Proxy size | Preview quality |
|---|---|---|---|
| 2x real time or faster | Auto (media that needs them) | 1/2 | Auto |
| 1x to 2x | Auto | 1/4 | 1/2 |
| slower than real time | Always | 1/4 | 1/4 |
| not measured | unchanged defaults | | |

Proxies are all-intra H.264 when the GPU both encodes and decodes H.264 (small and quick to make), ProRes Proxy
otherwise. The summary lists each measurement with a green or amber dot, the recommended settings in words and one
sentence per decision; *Use these settings* writes them to the Media & cache settings (MediaReadiness:
`proxies/policy`, `proxies/format`, `proxies/divisor`, `preview/quality`), *Keep my settings* leaves them. The report is
kept (`performance/report`, JSON) and shown again from Settings until the next run.

Code: `src/editor/src/PerformanceProbe.cpp`, the `PerformanceCheck` bridge and `qml/dialogs/FirstRunCheckDialog.qml`.
Tests: `tests/editor/test_performance_probe.cpp` (the table, the JSON, a real run, cancel, an exhausted budget) and
`tests/app/test_first_run_check.cpp` (the bridge, the offer in the window, run, apply).

## Portability

Code is written for all three platforms; only Windows is run. What is known (audit of 2026-10-12, I.7):

- Every Win32, D3D11/DXGI, WinHTTP and MSVC-CRT use is guarded with a working POSIX branch.
- Decode: D3D11VA/CUDA/QSV on Windows, VideoToolbox on macOS, VAAPI/CUDA on Linux, software everywhere.
  Zero-copy GPU decode is Windows-only (D3D11); macOS and Linux read decoded frames back (docs/zero-copy.md).
- Rendering: QRhi on D3D11 (D3D12 opt-in), Metal, Vulkan then OpenGL, with a software fallback.
- Keys: Windows Credential Manager, macOS Keychain, libsecret (QtKeychain); in-memory for the session without one.
- Audio: miniaudio's WASAPI, Core Audio, PulseAudio/ALSA/JACK, with a null device fallback.
- Helpers are found next to the executable (`Contents/MacOS` in the macOS bundle; the install puts them there).
- Toolchains: GCC 14 and LLVM 20 (libc++ 20) are what CI uses. Apple Clang from Xcode 16 lacks parts of C++20/23 the code
  uses (`std::jthread`, floating-point `std::from_chars`), so the macOS jobs use Homebrew LLVM; the configure step stops
  with a clear message on an older standard library. The macOS build finds LLVM's `libc++` through an rpath into the
  Homebrew prefix: a bundle for other Macs must carry it (not done; no macOS package is produced yet).
- Clang front end: the core builds with clang-cl 21 under `/W4 /WX` (building.md, "clang-cl"), which found a 128-bit
  division that needs compiler-rt and a few unused functions and fields. Off Windows the presets add `-Wshadow
  -Wconversion -Wpedantic`, which clang-cl's `/W4` does not enable: those builds are still unverified.
- Not available off Windows yet: the managed local assistant (llama-server builds are Windows x64 only), the CLI's model
  download (`wabcut-cli transcribe --download`; the app downloads through Qt), DirectML for smart search (CPU instead),
  Collect Files' font gathering.

## Release checklist

1. `git merge master`; confirm `docs/internal/development-status.md` is current.
2. Bump `project(Wabcut VERSION ...)`; update the "What's new" doc (`docs/roadmap.md`).
3. Clean configure and build: `cmake --preset win-msvc`, `cmake --build --preset win-msvc -j 2`.
4. Full suite in label groups (building.md, "Tests and labels"), including `-L install` (install tree + packages) and
   `-L perf` on a quiet machine. Record the counts in `docs/internal/development-status.md`.
5. `cpack --config build/win-msvc/CPackConfig.cmake`; check `install.package`'s report of sizes and contents.
6. Read the generated `THIRD-PARTY-NOTICES.txt`: new components, versions, the runtime library list.
7. On a clean Windows account (or VM): install the MSI without admin rights, open a project by double-click (with the
   association ticked), export, uninstall, and confirm the projects and settings are still there.
8. Sign the MSI and the executables (not yet available: needs a code-signing certificate).
9. Tag `vX.Y.Z`, attach the MSI, the portable ZIP and the source archive; CI's `unix-full` results for that commit go
   into the release notes as "unverified" builds.
