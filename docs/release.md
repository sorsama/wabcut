# Releasing Wabcut

How a Wabcut release is versioned, built, packaged and checked before anyone else installs it. Building from source is
in [building.md](building.md); this page starts where a green `win-msvc` build ends.

## Platform status

| Platform | Build | Tests | Packages | Status |
|---|---|---|---|---|
| **Windows 10/11 x64** | `win-msvc` (MSVC 19.36+) | full suite, GPU and audio-device tests included | per-user MSI, portable ZIP | **Verified** on the development machine for every release candidate |
| **Linux x64** (Ubuntu 24.04, GCC 14) | `linux-gcc` | `ctest --preset linux-gcc` (no GPU / audio-device tests) | AppImage, tar.gz | **Preview**: `unix-full.yml` builds and tests it, `release.yml` packages, installs and starts it; nobody has used it on a real machine |
| **macOS 15+ arm64** (Homebrew LLVM 20) | `macos-clang` | `ctest --preset macos-clang` (no GPU / audio-device tests) | DMG (`Wabcut.app`) | **Preview**: as Linux |
| Core (any OS, no Qt/FFmpeg) | `core` | `ctest --preset core` | — | Verified on Windows with MSVC and with clang-cl 21.1.8 (`/W4 /WX`, 678 tests); CI on all three |

"Verified" means built, the whole test suite run and the packages installed and smoke-tested on that platform for the
release. "Preview" means the CI workflows build, test, package and smoke-test it on GitHub's runners (no GPU, no
audio device), but nobody has edited with it on a real machine; the [portability notes](#portability) list what is
known to differ.

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
- **Signed once the project has a certificate.** The release workflow signs the MSI and every program file it ships
  when a certificate or Azure Trusted Signing is configured ([Code signing](#code-signing)); until then the packages
  are unsigned and SmartScreen warns about an unknown publisher.

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

## Making a release

`.github/workflows/release.yml` builds every package from one commit, checks it, and publishes the GitHub release:

1. Bump `project(Wabcut VERSION ...)`, add a `## x.y.z (date)` section to `docs/release-notes-<x.y>.md` (the release
   text quotes it, `cmake/packaging/release_notes.py`) and move the version to "Shipped" in `docs/roadmap.md`. Merge to
   `main`.
2. Tag the commit: `git tag vX.Y.Z && git push origin vX.Y.Z`, or **Actions > release > Run workflow** with the tag,
   which creates it on the chosen branch. A tag that does not match the version stops the run. Tag once the merge's
   windows-full and unix-full runs on `main` have passed their "Save the compiler cache" step (about an hour on Windows):
   the release then compiles almost nothing. A tag pushed earlier works too, but compiles everything the merge changed.
3. The workflow builds the shipped programs without the test suites (windows-full and unix-full run those for every
   commit), from the vcpkg packages and compiler cache those jobs saved on `main`
   ([CI caches](building.md#ci-caches)), and checks each package before anything is published:

   | Platform | Build | Packages | Checked by |
   |---|---|---|---|
   | Windows x64 | `win-msvc` | MSI, portable ZIP | `PackageSmokeTest.ps1` (the `install.package` test): the ZIP unpacked and run, the MSI installed per user, run and uninstalled |
   | Linux x64 | `linux-gcc` (local AI off) | AppImage, tar.gz | `InstallSmokeTest.cmake` (the `install.smoke` test); the AppImage started under Xvfb until it draws its first frame |
   | macOS arm64 | `macos-clang` (local AI off) | DMG | `InstallSmokeTest.cmake`; `finish_bundle.py`'s check; the finished bundle run with Homebrew's LLVM moved away (CLI render; the app's start is best effort without a GPU) |

4. The publish job creates the release "Wabcut x.y.z", marked latest (not a pre-release, even at 0.x: the README's
   Download link is `releases/latest`, which skips pre-releases), with the packages and `SHA256SUMS.txt`. Windows is required. A Linux or macOS package that failed its checks is left out, and the release
   text says so. Running the workflow again for the same tag replaces the files.
5. With the `WABCUT_UPDATE_SIGNING_KEY` secret set, the publish job also writes and signs `wabcut-update.json`, the
   version manifest the in-app update check reads ([Updates](#updates)), and checks it against the public key built
   into the app before it is attached.

Without a tag (Run workflow with an empty tag, or a pull request that changes the packaging) the packages are built,
checked and kept as workflow artifacts, and nothing is published; the `platforms` choice builds one platform only.

### Code signing

Windows SmartScreen warns about an unknown publisher when a downloaded installer is not
Authenticode-signed, and most people stop there. The release workflow signs as soon as the repository is set up for it,
and builds unsigned packages, as before, while it is not. No certificate, key or password is ever in the repository.

What gets signed, in `cpack` (`cmake/packaging/SignPackage.cmake.in`, run as CPack's pre- and post-build scripts when
`WABCUT_SIGN_RELEASE=1`): every `.exe` and `.dll` of the staged install tree before packaging, so the portable ZIP and
the MSI carry signed programs, then the MSI itself. Files that already carry a valid signature (Qt's libraries,
Microsoft's runtime and Direct3D DLLs) keep their vendor's. Every signature is SHA-256 with an RFC 3161 timestamp, so it
stays valid after the certificate expires, and `signtool verify /pa` checks each one; the workflow's *Check the
signatures* step then checks the MSI and the unpacked ZIP's programs once more.

`tools/sign-release.ps1` does the signing and works the same on a developer machine:

```powershell
pwsh tools/sign-release.ps1 -Path build\win-msvc\bin -WhatIf                       # what would be signed
pwsh tools/sign-release.ps1 -Path package.msi -Thumbprint <thumbprint>               # a certificate in the store
$env:WABCUT_SIGN_RELEASE = '1'; cpack --config build\win-msvc\CPackConfig.cmake      # sign while packaging
```

**Setting it up (the owner, once).** Pick one:

1. **Azure Trusted Signing** (cheapest for an individual, no hardware token, works on GitHub's runners):
   1. In the Azure portal create a *Trusted Signing account* and complete *identity validation* (individual developers
      are accepted; it takes a few days), then a *certificate profile* of type Public Trust.
   2. Create an app registration (Microsoft Entra ID > App registrations) with a client secret, and give it the
      *Trusted Signing Certificate Profile Signer* role on the signing account.
   3. In the GitHub repository, Settings > Secrets and variables > Actions:
      - secrets `AZURE_TENANT_ID`, `AZURE_CLIENT_ID`, `AZURE_CLIENT_SECRET` (the app registration's);
      - variables `WABCUT_TRUSTED_SIGNING_ENDPOINT` (the account's region endpoint, e.g.
        `https://wus2.codesigning.azure.net`), `WABCUT_TRUSTED_SIGNING_ACCOUNT` (the account name),
        `WABCUT_TRUSTED_SIGNING_PROFILE` (the certificate profile name), `WABCUT_SIGN_PUBLISHER` (the name the
        certificate is issued to, exactly as Windows shows it: the update check accepts an installer signed by that name
        only), and optionally `WABCUT_TRUSTED_SIGNING_CLIENT_VERSION` to pin the signing client's NuGet version.
2. **An OV or EV certificate as a PFX file** (from a certificate authority; EV certificates on a hardware token cannot be
   used on GitHub's runners - sign locally with `-Thumbprint` then): secrets `WABCUT_SIGN_PFX_BASE64` (the file in
   base64: `[Convert]::ToBase64String([IO.File]::ReadAllBytes('cert.pfx'))`) and `WABCUT_SIGN_PFX_PASSWORD`; optionally
   the variable `WABCUT_SIGN_PUBLISHER`. The workflow imports it into the runner's user store for the job only.

Then run **Actions > release > Run workflow** without a tag: the summary says "Packages signed with ..." and the
artifacts' MSI shows the publisher in its Properties > Digital Signatures. A new certificate still meets SmartScreen
warnings for a while, until downloads build its reputation; EV certificates start with more.

### Linux packages

- **AppImage** (`wabcut-<version>-linux-x64.AppImage`): the install tree under `usr/`, `AppRun` starting
  `usr/bin/wabcut`, and the launcher entry and icon (`cmake/packaging/linux/make-appdir.sh`). Built with appimagetool
  1.9.1 and the type 2 runtime 20251108, both pinned by SHA-256 in the workflow; the runtime needs no FUSE library.
- **tar.gz** (`wabcut-<version>-linux-x64.tar.gz`): the same tree in a `wabcut-<version>-linux-x64/` folder: `bin/`
  (the app, `wabcut-cli`, the helpers, ONNX Runtime, `qt.conf`, the in-app docs), `lib/` (Qt and ICU), `plugins/`,
  `qml/`, `share/applications/wabcut.desktop`, `share/icons/hicolor/*/apps/wabcut.png` and `share/doc/Wabcut/`
  (license, notices, licenses).
- Qt is deployed by `qt_deploy_qml_imports` and `qt_deploy_runtime_dependencies` (cmake/WabcutInstall.cmake). The
  distribution provides the rest: glibc 2.39 or newer (built on Ubuntu 24.04), X11/xcb, the GL and Vulkan loaders,
  libva, PulseAudio or ALSA, libsecret and fontconfig.
- The executables are stripped (the AppImage, the tar.gz and the macOS bundle): their RelWithDebInfo debug
  information would make the Linux tree about 2.4 GB. A crash report's backtrace then shows addresses, not names.

### macOS package

- **DMG** (`wabcut-<version>-macos-arm64.dmg`): `Wabcut.app` and a link to Applications. In the bundle:
  `Contents/MacOS` (the app, `wabcut-cli` and the helpers, which the app starts from its own folder),
  `Contents/Frameworks` (Qt, LLVM's libc++, libc++abi and libunwind, ONNX Runtime), `Contents/PlugIns`, and
  `Contents/Resources` (QML modules, the in-app docs, the icon, `LICENSE.txt`, `THIRD-PARTY-NOTICES.txt`, `licenses/`).
  `Info.plist` (`cmake/packaging/macos/Info.plist.in`) has the bundle id `io.github.sorsama.wabcut` and the
  microphone reason for voice-over recording.
- **Signed ad hoc, not notarized.** Apple silicon runs only signed code, so `finish_bundle.py` signs every file ad hoc.
  Without a Developer ID signature and Apple's notarization, Gatekeeper refuses the first start of a downloaded copy
  until the user allows it (System Settings > Privacy & Security > Open Anyway). Notarizing needs a paid Apple
  Developer account; it is an open item of the [release checklist](#release-checklist).

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
| GPU | the adapters (`gpuAdapters`), the one Wabcut uses and why (`selectedGpu`: Settings > Graphics card, the Windows Graphics setting, or the high-performance GPU; [performance.md](performance.md#which-gpu-wabcut-uses)) and the app's graphics API and adapter (RendererInfo) |
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

## Updates

Wabcut can tell when a newer release is out and, on Windows, install it. It is **opt-in**: nothing is asked of the
internet until the user turns on *Check for updates automatically* (Settings > General > Updates, off by default) or
chooses **Help > Check for Updates...** (also in Settings and the command palette). An automatic check runs once a day
at most, shortly after start; a newer version shows a toast with *Details*, and *Skip This Version* keeps automatic
checks quiet about it.

**The check.** A plain GET of `https://github.com/sorsama/wabcut/releases/latest/download/wabcut-update.json` with the
user agent `Wabcut/<version>`; nothing about the user or the projects is sent. The manifest is
`{ format, version, key, payload, signature }`: `payload` is the release's JSON (version, date, notes in Markdown, the
release page, the MSI's address, size, SHA-256, whether it is Authenticode-signed and by whom), signed as bytes with
ECDSA P-256 over SHA-256. Wabcut verifies the signature with the public key built into it (CMake cache variable
`WABCUT_UPDATE_PUBLIC_KEY`, base64 SubjectPublicKeyInfo; several keys separated by `;` let a new key take over) through
Windows CNG, and refuses a manifest that is unsigned, signed by another key, changed, incomplete or that points
anywhere but `https://`. Then it compares versions: only a newer one is offered. A build with an empty
`WABCUT_UPDATE_PUBLIC_KEY` never checks. `WABCUT_UPDATE_RELEASES_URL` moves the whole thing for a fork. A release
published without its manifest (no signing secret, below) is still found: on a 404 the check asks where
`.../releases/latest` redirects (`.../releases/tag/v0.5.1`) and offers that version, unverified, with its fixed release
page only, as on Linux and macOS; nothing is downloaded or installed from it.

**The install (Windows, installed with the MSI).** *Download and Install* fetches the MSI into
`%LOCALAPPDATA%\Wabcut\Wabcut\updates`, hashing as it goes; a file that is larger, smaller or hashes differently than
the manifest says is deleted, and so is one the manifest calls signed whose Authenticode signature is not trusted or
names another publisher (WinVerifyTrust). The verified MSI then installs **after Wabcut closes, with the user's
consent**: *Install When I Quit*, or *Quit and Install*, which closes the window the normal way, so unsaved changes and
running exports ask first (the button waits while background work runs). Once the process has ended, a hidden
PowerShell waits for it, runs `msiexec /i <msi> /passive /norestart` - a major upgrade into the same folder
(`INSTALL_ROOT`), keeping the `.wabcut` association - and starts Wabcut again. Never during an export: the installer is
only started after the event loop has returned from a normal close. A quit given up after *Quit and Install* (Cancel at
"Save changes?", Keep Wabcut Open) calls the install off again, and says so.

The verified MSI cannot change on its way to msiexec: from the moment it passes its checks Wabcut keeps it open without
write or delete sharing, checks its size, SHA-256 and (when signed) its Authenticode signer against the manifest again
right before it starts the installer (still holding it), and the PowerShell that starts msiexec checks them once more
after Wabcut has gone. A verified download is kept: Later, a quit given up or a new check of the same release finds it
ready (Help > Check for Updates... does not even ask the server again while it is ready); only a new download replaces
it.

**Elsewhere.** The portable ZIP, Linux and macOS show the new version and open its release page (the ZIP cannot be
upgraded by the MSI). On Linux and macOS nothing verifies the manifest yet, so the dialog shows only the version and
links to the fixed release page of that version, never to an address from the manifest.

**The keys (the owner, once).** The key pair was made with `tools/update-keys.py generate --out <file outside every
repository>`; its public half is `WABCUT_UPDATE_PUBLIC_KEY`'s default in `src/app/CMakeLists.txt`. Put the private key
(the PEM text) into the repository secret `WABCUT_UPDATE_SIGNING_KEY`; the publish job then writes and signs
`wabcut-update.json` for each release (`tools/update-keys.py manifest`, with the MSI's size and SHA-256 and the
signing result of the Windows job) and verifies it against the built-in key before attaching it. Keep an offline copy
of the private key: a lost key means a new key pair and an app release that trusts both, before the old one can go. A
leaked key is replaced the same way, as fast as possible.

Code: `src/app/src/update/` (`UpdateManifest`, `UpdateCrypto`), the `Updates` bridge (`src/app/src/bridge/UpdateBridge.*`),
`qml/dialogs/UpdateDialog.qml`, `tools/update-keys.py`. Tests: `tests/app/test_updates.cpp`, against a loopback fake
server with a test key (a newer version offered and installed through an injected launcher, the same version not
offered, a tampered manifest, a wrong key, a tampered installer, an Authenticode check, the daily limit, the dialog) and
a manifest signed by `tools/update-keys.py` (`tests/app/data/updates/`).

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
  Homebrew prefix; for the DMG, `cmake/packaging/macos/finish_bundle.py` copies it into the bundle. That `libc++` is
  built for the macOS of Homebrew's bottle (the runner's, 15) and calls `os_sync_wait_on_address` (14.4+), so the bundle
  asks for macOS 15 (`LSMinimumSystemVersion`) although the code targets 13.0; 0.5.2 asked for 13 and stopped at launch
  on 13 and 14. `finish_bundle.py` fails the release when a file in the bundle needs a newer macOS than the bundle asks
  for. A `libc++` built from LLVM's sources for 13.0 would bring 13 and 14 back.
- Linux packages: Qt's generic deployment copies the QML plugins the app imports but not every Qt library they link,
  and Qt's xcb platform plugin links xcb helpers a stock desktop lacks (`libxcb-cursor0` and others), so 0.5.2's AppImage
  and tar.gz did not start on a desktop. `cmake/packaging/linux/BundleLibraries.cmake` bundles everything that is not
  part of the operating system (its list: the C/C++ runtimes, the GPU drivers, X11, Wayland, fonts, GLib, GTK, D-Bus,
  ALSA/PulseAudio) and `install.smoke` fails when a binary would load anything else from outside the tree. The release
  starts the AppImage without the Qt kit's environment and without those xcb packages. The app prefers X11 (XWayland on a
  Wayland desktop) and falls back to the bundled Wayland platform plugin. The bundled libva (vcpkg's) looks for VA-API
  drivers only in the build machine's folder, so the app and the CLI set `LIBVA_DRIVERS_PATH` to the distribution's
  `dri` folders at start, unless it is set (`media::useSystemVaapiDrivers()`).
- OpenGL (Linux without a Vulkan driver) links the stages of a program only when the uniform blocks they share are
  declared the same; Vulkan, D3D and Metal do not check. Every shader declares a block whole (`render.shader_blocks`).
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
8. Check that the release run signed the Windows packages ([Code signing](#code-signing); needs the certificate
   set up) and that `wabcut-update.json` is attached ([Updates](#updates); needs `WABCUT_UPDATE_SIGNING_KEY`). Sign and
   notarize the macOS bundle with a Developer ID (not yet available: needs an Apple Developer account).
9. Once `main`'s CI has saved its caches for the merge, tag `vX.Y.Z` and push the tag: the release workflow builds,
   checks and publishes every package ([Making a release](#making-a-release)).
