# Building Wabcut

Two builds share one CMake project:

| Build | What it builds | Needs | Preset |
|---|---|---|---|
| **Core** | The Qt-free modules `src/model`, `src/command` (with its MCP server), `src/keymap`, `src/interchange`, `src/ai`, and their tests | A C++23 compiler, CMake 3.28+, Ninja. No Qt, FFmpeg, miniaudio or vcpkg. | `core` (any OS) |
| **Full** | Everything: media, audio, render, playback, export, the CLI, the Qt Quick app, all tests, the install tree, the MSI installer and the portable ZIP | The above plus vcpkg and Qt 6.11 (and WiX 5.0.2 for the MSI) | `win-msvc` (supported), `linux-gcc` / `macos-clang` (experimental, CI-ready but unverified: [release.md](release.md#platform-status)) |

Quick start on Windows (PowerShell, from the repository root):

```powershell
# Core: compiler, CMake and Ninja only
. .\tools\dev-env.ps1 -Core
cmake --preset core
cmake --build --preset core
ctest --preset core

# Full
. .\tools\dev-env.ps1
cmake --preset win-msvc
cmake --build --preset win-msvc
ctest --preset win-msvc
```

On Linux or macOS the core build is the same three `cmake` / `ctest` commands, with a C++23 compiler, CMake and
Ninja on `PATH`.

## Pinned versions

The full build is reproducible from these pins. Update this table with any of them (see
[Updating the pins](#updating-the-pins)).

| Component | Version | Where it is pinned |
|---|---|---|
| Qt | **6.11.3** (MSVC 2022 64-bit kit, plus the Qt Shader Tools add-on) | `WABCUT_TESTED_QT_VERSION` in `CMakeLists.txt`, `$wabcutTestedQt` in `tools/dev-env.ps1`, `QT_VERSION` in `.github/workflows/*.yml` |
| vcpkg baseline | **`e456309491fd875487bf02b8a0dca76c1b1b00cc`** (2026-10-10) | `builtin-baseline` in `vcpkg.json` |
| vcpkg tool | **2026-09-26** (`2026-09-26-51bf87ca6e9bf3e622d84ff323bd202ab1ca0c0b`) | `scripts/vcpkg-tool-metadata.txt` of the vcpkg checkout at the baseline. `bootstrap-vcpkg` downloads exactly this tool. |
| vcpkg triplet (Windows) | `x64-windows-release`: dynamic CRT and DLLs, release-only | `cmake/triplets/x64-windows-release.cmake` |
| CMake | minimum **3.28**; developed with 4.3.1 (bundled with Visual Studio) | `cmake_minimum_required`, `CMakePresets.json` |
| Ninja | developed with 1.13.2 (bundled with Visual Studio) | — |
| MSVC | developed with 14.51 (cl 19.51.36248, Visual Studio 2026 18.10); minimum 19.36 (VS 2022 17.6) | `wabcut_check_toolchain()` |

Dependency versions at the baseline (the vcpkg port version is the upstream version):

| Package | Version | License | Notes |
|---|---|---|---|
| FFmpeg | 9.0.2 | GPL (GPL build: `--enable-gpl`) | features in `vcpkg.json`; the exact configure line ends up in `THIRD-PARTY-NOTICES.txt` |
| x264 | 0.165.3222 | GPL-2.0-or-later | linked by FFmpeg |
| x265 | 4.3 | GPL-2.0-or-later | linked by FFmpeg |
| dav1d | 1.5.4 | BSD-2-Clause | linked by FFmpeg |
| libvpl | 2.17.0 | MIT | FFmpeg `qsv` |
| SVT-AV1 | 4.1.0 | BSD-3-Clause-Clear + AOMedia Patent License 1.0 | FFmpeg `svt-av1` (AV1 software encoder; hardware AV1 preferred when present) |
| fastfeat | 391d5e9 | BSD-3-Clause | linked by SVT-AV1 |
| libvpx | 1.16.0 | BSD-3-Clause (with a patent grant) AND ISC | FFmpeg `vpx` (VP9 for WebM exports) |
| Opus | 1.6.1 | BSD-3-Clause | FFmpeg `opus` (WebM audio) |
| AMF headers / nv-codec-headers | 1.5.3 / 13.1.15.0 | MIT | header-only |
| miniaudio | 0.11.25 | Unlicense or MIT-0 | |
| nlohmann_json | 3.12.0 | MIT | also fetched by the core build |
| pugixml | 1.16 | MIT | also fetched by the core build |
| zlib | 1.3.2 | Zlib | also fetched by the core build |
| spdlog / fmt | 1.17.0 / 12.2.0 | MIT | declared in `vcpkg.json` |
| Catch2 | 3.16.0 | BSL-1.0 | tests only; also fetched by the core build |


> **Debug info (2026-10-12):** the `win-msvc` preset embeds debug info in the object files (`/Z7`,
> `CMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded`) instead of a shared PDB server. Several worktrees build at once on
> the shared machine, and `mspdbsrv` grew to 7-8.5 GB serving them; with `/Z7` there is no PDB server, per-compiler
> memory is unchanged (measured), objects are about 31 % larger. Reconfigure once (`cmake --preset win-msvc`); the
> next build is a full rebuild.

## Prerequisites (Windows)

1. **Visual Studio 2022 17.6+ or Visual Studio 2026** (any edition, or Build Tools) with the workload
   *Desktop development with C++*, including *MSVC x64/x86 build tools* and *C++ CMake tools for Windows*
   (which provides CMake and Ninja). `dev-env.ps1` finds it with `vswhere`.
2. **vcpkg**, for the full build only. Clone it and bootstrap the tool that the baseline pins:

   ```powershell
   git clone https://github.com/microsoft/vcpkg G:\dev\vcpkg
   git -C G:\dev\vcpkg checkout e456309491fd875487bf02b8a0dca76c1b1b00cc
   G:\dev\vcpkg\bootstrap-vcpkg.bat -disableMetrics
   ```

   Manifest mode takes every port version from `builtin-baseline`, so a checkout that is newer than the
   baseline also works, as long as it contains the baseline commit. Checking out the baseline also pins the vcpkg
   tool and the `vcpkg.cmake` toolchain script. The first configure builds FFmpeg, x264 and x265 from source,
   which takes an hour or more. Later configures reuse the binary cache (`VCPKG_DEFAULT_BINARY_CACHE`).
3. **Qt 6.11.3**, for the full build only. Use the **Qt Online Installer**
   (<https://www.qt.io/download-qt-installer-oss>) and select *Qt → Qt 6.11.3 → MSVC 2022 64-bit* and
   *Qt 6.11.3 → Additional Libraries → Qt Shader Tools*. Qt Svg, Qt Quick and Qt Quick Controls are part of the
   base kit. The default location `C:\Qt\6.11.3\msvc2022_64` is what `dev-env.ps1` expects. The installer also
   has a command-line mode (`qt-online-installer-windows-x64-<version>.exe --help`; `search 6.11.3` lists the
   package IDs).

   **aqtinstall:** the latest release, 3.3.0, cannot install Qt 6.11. Since 6.11, Qt's online repository keeps
   each kit in its own folder with its own `Updates.xml`, and 3.3.0 cannot read that layout. The fix
   ([miurahr/aqtinstall#1000](https://github.com/miurahr/aqtinstall/pull/1000)) is merged on master but not
   released. CI therefore installs aqtinstall from a pinned master commit. Locally, use the Online Installer,
   or install the same commit:
   `pip install "aqtinstall @ git+https://github.com/miurahr/aqtinstall.git@076e1659807d0b362a3ed684d54c2e9c775eb9c7"`.

## The developer environment: `tools/dev-env.ps1`

Dot-source it in every new PowerShell before configuring or building:

```powershell
. .\tools\dev-env.ps1          # full build: MSVC + CMake + Ninja + vcpkg + Qt
. .\tools\dev-env.ps1 -Core    # core build: MSVC + CMake + Ninja only
```

The script does the following:

- It enters the Visual Studio x64 developer shell if `cl.exe` is not on `PATH`. If that install has no
  bundled CMake or Ninja, it borrows them from another Visual Studio install.
- It checks that `cl`, `cmake` (3.28 or newer) and `ninja` are present.
- In full mode, it also checks that `VCPKG_ROOT` is a bootstrapped vcpkg checkout and creates the binary cache
  directory.
- In full mode, it also checks that `WABCUT_QT_DIR` is a Qt 6 MSVC kit. It reads the version with
  `qtpaths --qt-version`: older than 6.11 is an error, and anything other than 6.11.3 gets a warning. It also
  checks that Quick, QuickControls2, ShaderTools and Svg are installed, then puts the Qt `bin` directory on
  `PATH`.

Every failure stops with a `dev-env: <what is wrong and how to fix it> (see docs/building.md)` message rather
than failing later inside CMake.

Overrides (set them before dot-sourcing):

| Variable | Default | Meaning |
|---|---|---|
| `VCPKG_ROOT` | `G:\dev\vcpkg` | vcpkg checkout |
| `VCPKG_DEFAULT_BINARY_CACHE` | `G:\dev\vcpkg-cache` | vcpkg binary cache (shared between build trees and worktrees) |
| `WABCUT_QT_DIR` | `C:\Qt\6.11.3\msvc2022_64` | Qt kit; the presets pass it as `CMAKE_PREFIX_PATH` |
| `WABCUT_DEPS_READY_MARKER` | `G:\dev\vcpkg-prebuild\READY` | Shared-machine gate. If the marker's directory exists, the script waits up to 120 min for the marker file, so that parallel configures do not rebuild FFmpeg while the lead fills the binary cache. Without that directory it does nothing. |

To cap build parallelism on a shared machine, set `$env:CMAKE_BUILD_PARALLEL_LEVEL = 3` or pass `-j 3` to
`cmake --build`.

## Presets

| Configure / build preset | OS | Type | Notes |
|---|---|---|---|
| `core` | any | RelWithDebInfo | `WABCUT_CORE_ONLY=ON`, `WABCUT_FETCH_DEPENDENCIES=ON`, no toolchain file |
| `win-msvc` | Windows | RelWithDebInfo | the supported full build: vcpkg `x64-windows-release`, Qt from `WABCUT_QT_DIR` |
| `win-msvc-debug` | Windows | Debug | opt-in, see [Debug builds](#debug-builds) |
| `win-msvc-asan` | Windows | RelWithDebInfo | core only, AddressSanitizer + libFuzzer, see [Sanitizers and fuzzing](#sanitizers-and-fuzzing) |
| `linux-gcc` | Linux | RelWithDebInfo | experimental |
| `macos-clang` | macOS | RelWithDebInfo | experimental, deployment target 13.0 |

Every preset builds with Ninja into `build/<preset>` and turns on `WABCUT_WARNINGS_AS_ERRORS`.

| Test preset | Runs |
|---|---|
| `core` | all core tests |
| `win-msvc` | every test, including GPU, audio-device and the install smoke test |
| `win-msvc-ci` | `win-msvc` without the `gpu` and `audio-device` labels (what CI runs) |
| `win-msvc-debug` | every test of the Debug tree |
| `win-msvc-asan` | the core suites and the fuzz regression runs under ASan, one at a time, without `perf` |
| `linux-gcc`, `macos-clang` | all but `gpu` and `audio-device` |

## Core-only build

`-DWABCUT_CORE_ONLY=ON` (the `core` preset) builds only the Qt-free modules: `model`, `command` (with the MCP
server library), `keymap`, `interchange` and `ai`. It also builds the test suites of `model`, `keymap`,
`interchange` and `ai`, and the fuzz harnesses (`tests/fuzz`). The `command` suite is left out, because its stdio
tests spawn `wabcut-cli`, which links the FFmpeg media layer. `-DWABCUT_CORE_COMMAND_TESTS=ON` builds it without
them (the `win-msvc-asan` preset does). It does not look for Qt, FFmpeg or miniaudio. The pure-C++ libraries it needs (nlohmann_json,
pugixml and zlib, plus Catch2 for the tests) come from:

1. an installed package, if `find_package(... CONFIG)` finds one (through the vcpkg toolchain,
   `CMAKE_PREFIX_PATH`, or the system), or else
2. with `WABCUT_FETCH_DEPENDENCIES=ON` (set by the `core` preset), a download by `FetchContent` of the release
   archive pinned in `cmake/WabcutDependencies.cmake`, with its SHA256 hash. These are the same versions as
   the vcpkg baseline, built as static libraries and not installed.

On Windows, open a new PowerShell and run `. .\tools\dev-env.ps1 -Core`. Nothing about vcpkg or Qt needs to
exist. If another compiler such as MinGW is on `PATH`, set `$env:CC = 'cl'; $env:CXX = 'cl'` before the first
configure so CMake picks MSVC.

On Linux and macOS, install CMake 3.28+, Ninja and a C++23 toolchain, and select the compiler with `CC` / `CXX`
if it is not the default.

The configure step checks that the standard library provides `std::expected`, `std::format`, `std::jthread` /
`std::stop_token` and floating-point `std::from_chars`. If it does not, it stops with the list of supported compilers:

| Toolchain | Minimum | Checked |
|---|---|---|
| MSVC | 19.36 (Visual Studio 2022 17.6) | every build (`win-msvc`, `core`) |
| clang-cl | LLVM 17, with the MSVC standard library | `core` with clang-cl 21.1.8 (verified locally); CI `core.yml` |
| GCC | 13 (libstdc++ 13) | CI only (GCC 14) |
| Clang | 17 with libstdc++ 13, or LLVM 20 with its own libc++ | CI only (macOS: Homebrew LLVM) |
| Apple Clang | not supported yet: Xcode 16's libc++ lacks `std::jthread` and floating-point `std::from_chars` | |

On macOS install LLVM with Homebrew (`brew install llvm`) and build with it: for the core build set `CC` / `CXX` to
`$(brew --prefix llvm)/bin/clang` / `clang++` and link its libc++ (`LDFLAGS="-L$(brew --prefix llvm)/lib/c++
-Wl,-rpath,$(brew --prefix llvm)/lib/c++"`, as `core.yml` does); for the full build use the `arm64-osx-llvm` triplet
with `cmake/toolchains/macos-llvm.cmake` (as `unix-full.yml` does), so every vcpkg port uses the same compiler and
library.

### clang-cl

clang-cl compiles the code with Clang's front end and MSVC's standard library and ABI: it catches what MSVC accepts and
other compilers reject. Visual Studio installs it with the *C++ Clang tools for Windows* component; any LLVM 17+
`clang-cl.exe` works (vcpkg downloads one to `<vcpkg>/downloads/tools/clang/`). The core build with it, in a tree of
its own:

```powershell
. .\tools\dev-env.ps1 -Core
$clang = 'C:\path\to\LLVM\bin\clang-cl.exe'
cmake --preset core -B build\core-clang-cl -DCMAKE_C_COMPILER=$clang -DCMAKE_CXX_COMPILER=$clang
cmake --build build\core-clang-cl
ctest --test-dir build\core-clang-cl
```

Last verified 2026-10-12 with vcpkg's clang-cl 21.1.8 (the machine's Visual Studio 18 Build Tools have no Clang
component): the core preset builds with `/W4 /WX`, and all 678 of its tests pass (3 are skipped because they need
downloaded models). What clang-cl found, all fixed: `/Zc:preprocessor` is cl-only (clang-cl's preprocessor always
conforms); `/W4` means `-Wall -Wextra` for clang-cl, and `-Wextra`'s missing-field-initializers flags the code base's
deliberate `T{a, b}` initialization, so it is off for every Clang and GCC build; the 128-bit division in
`model/Time.h` calls `__udivti3`, which only compiler-rt provides, so clang-cl uses the portable loop there; and
unused functions and fields that Clang reports (`-Wunused-private-field`, `-Wunused-function`). The full build has
not been tried with clang-cl: its vcpkg dependencies are built with MSVC.

## Build options

| Option | Default | Meaning |
|---|---|---|
| `WABCUT_CORE_ONLY` | `OFF` | Build only the Qt-free modules: model, command, keymap, interchange, ai (implies `WABCUT_BUILD_APP=OFF`) |
| `WABCUT_BUILD_APP` | `ON` | Build the Qt Quick application |
| `WABCUT_BUILD_TESTS` | `ON` | Build the test suites (needs Catch2) |
| `WABCUT_WARNINGS_AS_ERRORS` | `OFF`; **ON in every preset and in CI** | `/WX` or `-Werror` |
| `WABCUT_FETCH_DEPENDENCIES` | `OFF`; ON in `core` | Download missing nlohmann_json, pugixml, zlib and Catch2 |
| `WABCUT_WITH_LOCAL_AI` | `ON` in full Windows builds; `OFF` elsewhere and with `WABCUT_CORE_ONLY` | Local transcription: whisper.cpp in the `wabcut-infer` helper (vcpkg feature `ai-local`: ggml with run-time CPU variants and the Vulkan backend, from the overlay ports in `cmake/ports`). See [Local AI](#local-ai) |
| `WABCUT_SANITIZE` | empty; `address` in `win-msvc-asan` | Build every target, the fetched dependencies included, with AddressSanitizer (MSVC `/fsanitize=address`; GCC/Clang also UBSan). See [Sanitizers and fuzzing](#sanitizers-and-fuzzing) |
| `WABCUT_FUZZ` | `OFF`; ON in `win-msvc-asan` | The parser harnesses in `tests/fuzz` become libFuzzer executables, everything else gets coverage instrumentation. Needs `WABCUT_SANITIZE=address` |
| `WABCUT_FUZZ_REGRESSION_RUNS` | `1000` | Mutations each fuzz harness runs in ctest after its seeds |
| `WABCUT_CORE_COMMAND_TESTS` | `OFF`; ON in `win-msvc-asan` | Build the command suite in core-only builds too, without the stdio tests that spawn `wabcut-cli` |
| `WABCUT_WITH_CLOUD_AI` | `ON` (not with `WABCUT_CORE_ONLY`) | AI provider keys in the OS keychain. Adds the vcpkg feature `ai-cloud`: QtKeychain 0.17 from the overlay port `cmake/ports/qtkeychain`, built against `WABCUT_QT_DIR` (`dev-env.ps1` passes it into vcpkg's clean build environment with `VCPKG_KEEP_ENV_VARS`; the port refuses any other Qt than the one pinned in `wabcut-qt.cmake`). OFF: keys last for the session only. The cloud adapters (`src/ai_net`) are built either way |

### Local AI

`WABCUT_WITH_LOCAL_AI` builds the `wabcut-infer` helper (whisper.cpp, out of process) that local transcription
runs in (docs/ai.md, "Transcription service"). It adds the vcpkg feature `ai-local`: whisper-cpp and ggml from
the overlay ports, with shaderc, glslang, SPIRV-Tools, SPIRV-Headers and the Vulkan headers and loader for the
GPU backend (no Vulkan SDK needed).

**Default.** ON for full Windows builds, OFF on Linux and macOS (not measured there yet) and in core builds. A
build dir keeps the value it was first configured with, so an existing build dir configured as OFF stays OFF.

**Cost** (AI-7, 2026-10-11, measured on the 32 GB development machine). A cold build of the chain with the
binary cache off, `VCPKG_MAX_CONCURRENCY=1`:

| Package | Time |
|---|---|
| spirv-tools | 11 min |
| ggml (all CPU variants, Vulkan shaders) | 5 min |
| glslang | 1.5 min |
| shaderc | 54 s |
| whisper-cpp, vulkan-loader, spirv-headers, vulkan-headers | 1 min together |
| **Total** | **19.6 min**, peak working set **2.1 GB** (all vcpkg processes) |

From the shared binary cache (`G:\dev\vcpkg-cache`, `VCPKG_DEFAULT_BINARY_CACHE` in `dev-env.ps1`) the whole project
manifest with `ai-local` restores in seconds. A fresh install of the project manifest with `ai-local` and
`ai-cloud` (the win-msvc triplet and overlays, vcpkg's default host triplet) restored all 34 packages in 8.7 s
and built none. The ABIs include the compiler version and the host triplet: after an MSVC update the first
configure builds the chain once and stores it for everyone else (about 20 minutes; set `VCPKG_MAX_CONCURRENCY=1`
first on a machine short of memory, where vcpkg otherwise builds with every core).

**Turn it on in an existing build dir:**

```powershell
. .\tools\dev-env.ps1
$env:VCPKG_MAX_CONCURRENCY = 1
cmake --preset win-msvc -DWABCUT_WITH_LOCAL_AI=ON   # restores ai-local from the binary cache
cmake --build build\win-msvc --target wabcut-infer wabcut-cli -j 1
build\win-msvc\bin\wabcut-cli.exe transcribe --providers   # "local  available"
```

`-DWABCUT_WITH_LOCAL_AI=OFF` removes it again; the core preset never has it.

**GPU and fallback.** ggml loads its backends at run time from the helper's directory (`ggml-cpu-*.dll`,
`ggml-vulkan.dll`, `vulkan-1.dll`, copied there at build time). Without the Vulkan backend in the build, the
configure step says so ("no ggml Vulkan backend in this build"). Without a Vulkan driver or device at run time,
the helper loads the model on the CPU. Either way the transcription result carries a note for the user (the
CLI prints "note: no Vulkan GPU was usable ...", and `TranscribeOutcome::note` has it for the app). Nothing
fails; CPU transcription is several times slower (use `--model base`).

### Warnings

Every Wabcut target gets `/W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor` on MSVC, and
`-Wall -Wextra -Wpedantic -Wshadow -Wconversion` on GCC and Clang. Third-party headers are included as SYSTEM.
Warnings are errors in the presets and in CI. The option is off by default, so a packager building a release
with a newer compiler is not stopped by a new warning (pass `-DWABCUT_WARNINGS_AS_ERRORS=OFF` with a preset to
get the same).

MSVC's C4702 ("unreachable code") comes from its optimizer in Qt headers inlined into qmlcachegen output.
`/external:W0` does not reach it, so it is switched off (`/wd4702`) only for the targets that compile
Qt-generated code: `wabcut_ui`, `wabcut_uiplugin`, `wabcut` and `wabcut_app_tests`, through
`wabcut_allow_qt_generated_code()` in `cmake/WabcutCompilerOptions.cmake`. The one other exception is the single
source file `tests/audio/test_clock.cpp`, which has a `return` after a throwing `FAIL()`; it is listed in
`tests/CMakeLists.txt` until that test is fixed. All other code keeps the full `/W4 /WX`.

## UTF-8 code page

Every Windows executable of the build (the app, `wabcut-cli`, the helpers `wabcut-plughost`, `wabcut-infer` and
`wabcut-embed`, the tools, the fake helpers and every test suite) embeds `cmake/utf8.manifest`, which makes UTF-8
the process's active code page (`activeCodePage`, Windows 10 1903 or later). `wabcut_target_defaults()` adds it to
each target whose type is `EXECUTABLE` (`wabcut_windows_executable()` in `cmake/WabcutCompilerOptions.cmake`); CMake
merges it with the linker's own manifest through `mt.exe`.

Why: without it the narrow ("A") Win32 calls, the CRT (`fopen`, `std::ofstream` on a `std::string`, `strerror`) and
`std::filesystem::path::string()` use the ANSI code page, which cannot represent most names: Thai on an English
Windows, emoji anywhere. `path::string()` then throws `std::system_error`, and a narrow file name names the wrong
file. With the manifest they all use UTF-8, the same text as `pathToUtf8()`. Code still converts paths with
`pathToUtf8()` / `pathFromUtf8()` (`wabcut/model/MediaLocator.h`) and builds derived names on the path itself
(`path part = file; part += ".part";`): the manifest is the safety net for third-party code that takes narrow
paths, not a license for `path::string()`. A new executable gets the manifest by calling `wabcut_target_defaults()`
like every other target.

## DLL search order

No Wabcut process loads a DLL from the current directory (a project folder opened from Explorer) or `PATH`:

- Every `main()` calls `wabcut::platform::hardenDllSearch()` first (`src/platform/include/wabcut/platform/DllSearch.h`):
  `SetDefaultDllDirectories` with the application folder, System32 and `AddDllDirectory` folders, and
  `SetDllDirectoryW(L"")`. Libraries that live elsewhere must be loaded by full path or have their folder added with
  `AddDllDirectory`.
- The shipped executables (`wabcut`, `wabcut-*`) link with `/DEPENDENTLOADFLAG:0xA00`: their static imports come from
  their own folder and System32 only (`0x800` alone would drop their own folder, where Qt, FFmpeg and ONNX Runtime
  are). Tests and tools keep the default, since in the build tree they find Qt through the `PATH` that ctest prepends.
  In a build tree, `wabcut-cli` therefore needs the Qt DLLs that the `wabcut` target's windeployqt step copies to `bin`.
- Helper processes (`ai::ChildProcess`, the plug-in host) start with a minimal environment (system, locale, `QT_*` and
  `WABCUT_*` variables; never a variable named like a key, token, secret or password; `PATH` = the helper's folder,
  the application folder and Windows' own) in their own folder, unless the caller names a working directory
  (`src/platform/include/wabcut/platform/HelperProcess.h`).

## Qt version policy

- Qt **older than 6.11** stops the configure step with a message naming the Qt that was found and how to point
  `WABCUT_QT_DIR` at the right one. `dev-env.ps1` makes the same check before CMake runs.
- Qt **6.12 or newer** configures, with a warning. The render code uses QRhi through Qt's private Gui API, which
  has no compatibility promise across minor releases, so a new minor version needs a deliberate update (see
  below).
- Every configure prints `Wabcut: Qt <version> (tested with 6.11.3) from <path>`.

## Debug builds

The `win-msvc` vcpkg triplet builds the dependencies in **release mode only**, which halves the FFmpeg, x264 and
x265 build time. A Debug build of Wabcut against them would mix the debug CRT and STL with release-CRT DLLs
(mismatched `_ITERATOR_DEBUG_LEVEL` and heaps). So configuring `CMAKE_BUILD_TYPE=Debug` with an MSVC compiler and
a `*-release` triplet stops with an explanation. The same happens for a multi-config generator that offers Debug.

- **For debugging, use `win-msvc` (RelWithDebInfo).** It has full PDBs and is what CI and the team build.
- **For a true Debug build, opt in to `win-msvc-debug`.** It uses vcpkg's stock `x64-windows` triplet, which
  builds every dependency in Debug and Release with matching CRTs, in its own tree `build/win-msvc-debug`.
  The first configure rebuilds FFmpeg, x264 and x265 twice, which takes hours and several GB. This preset is
  documented but not built by CI or by default.

```powershell
. .\tools\dev-env.ps1
cmake --preset win-msvc-debug
cmake --build --preset win-msvc-debug
ctest --preset win-msvc-debug
```

## Sanitizers and fuzzing

The `win-msvc-asan` preset (C.10) is the robustness build. It is core only, in its own tree `build/win-msvc-asan`:
- `WABCUT_CORE_ONLY`, with the dependencies fetched and built from source;
- `WABCUT_SANITIZE=address`: every target and every dependency is compiled with `/fsanitize=address`, so the STL's
  container annotations match everywhere;
- MSVC has no UBSan. The nearest it has is on: the STL's hardened preconditions (`_MSVC_STL_HARDENING`: an index out
  of range, an empty `optional` or `expected` dereferenced, ...) and destructor tombstones. A failed check aborts.
  GCC and Clang builds of `WABCUT_SANITIZE=address` add `-fsanitize=undefined`;
- `WABCUT_FUZZ=ON`: the harnesses in `tests/fuzz` are libFuzzer executables, and the rest of the code carries the
  coverage instrumentation libFuzzer needs (`/fsanitize-coverage`, MSVC 17.0 or later);
- `WABCUT_CORE_COMMAND_TESTS=ON`: the command suite builds without `wabcut-cli`.

The ASan runtime DLL is copied next to the executables, so they run outside a developer prompt too.

```powershell
. .\tools\dev-env.ps1 -Core
cmake --preset win-msvc-asan
cmake --build --preset win-msvc-asan          # -j 1: ASan objects are large; the preset sets it
ctest --preset win-msvc-asan                  # suites + fuzz regression runs, one at a time, without [perf]
```

- **Memory.** An ASan process takes several times the memory of a normal one. The build preset uses one job and the
  test preset runs one test at a time. Run fuzzers one after another, not alongside other ASan runs.
- **Timings.** Sanitizer builds are slower, so the test preset leaves out `perf`. The scale and soak numbers come from
  `win-msvc`: `ctest --preset win-msvc -L perf`.
- **A finding.** ASan prints the stack, aborts the test, and ctest shows it with `--output-on-failure`.

### Fuzz harnesses

`tests/fuzz/README.md` lists the harnesses and how to run one. There is one per untrusted input:
- project files, version history files, review packages;
- command requests and MCP messages;
- FCP7 XML, FCPXML, OTIO, EDL and Premiere `.prproj`;
- Premiere `.kys`, `.cube` LUTs and ASC CDL;
- AI provider stream and error bodies.

Every build runs them in ctest (label `fuzz`). Each harness takes its seeds and every kept reproducer, then
`WABCUT_FUZZ_REGRESSION_RUNS` deterministic mutations. Without `WABCUT_FUZZ` the driver is a small portable main
(`tests/fuzz/FuzzMain.cpp`, libFuzzer's flags, no coverage feedback). In `win-msvc-asan` it is libFuzzer itself.
Explore with libFuzzer for a fixed budget, one fuzzer at a time:

```powershell
build\win-msvc-asan\bin\wabcut_fuzz_seeds.exe build\win-msvc-asan\fuzz\seeds
build\win-msvc-asan\bin\fuzz_otio.exe -max_total_time=600 -timeout=10 -rss_limit_mb=2048 -max_len=65536 `
    -artifact_prefix=build\win-msvc-asan\fuzz\artifacts\otio\ build\win-msvc-asan\fuzz\corpus\otio `
    build\win-msvc-asan\fuzz\seeds\otio tests\interchange\fixtures
```

Generated corpora and artifacts stay in the build tree. A crash that was fixed keeps its minimized reproducer in
`tests/fuzz/seeds/<harness>/regression-*`, which every later run replays.

## Tests and labels

Each module owns `tests/<module>/CMakeLists.txt` and calls `wabcut_add_test_suite()`. Tests are found with Catch2
discovery, and every CTest test carries labels:

| Label | Tests |
|---|---|
| `<suite>`: `model`, `keymap`, `media`, `audio`, `render`, `playback`, `export`, `interchange`, `command`, `ai`, `app` | every test not claimed by a hardware label below; additional labels a suite passes through `PROPERTIES LABELS` are added |
| `gpu` | Catch2 tags `[gpu]`, `[hw]` (hardware video decode), `[screenshot]` (offscreen grabs on the real RHI backend), or `[qml]` together with `[rhi]` (the GPU status pill reads the live adapter) |
| `audio-device` | Catch2 tags `[audio-device]`, `[device]` (real output devices) |
| `perf` | Catch2 tag `[perf]` (unless a hardware label above applies): strict timing thresholds that need a quiet machine. Run them with `-L perf` when nothing else is building; exclude them with `-LE perf` on a loaded machine. Functional tests assert what must happen and only record timings |
| `install` | `install.smoke` and `install.package`, see [Install tree, installer and portable ZIP](#install-tree-installer-and-portable-zip) |
| `fuzz` | the parser fuzz harnesses' regression runs and the seed generator `fuzz.seeds` ([Sanitizers and fuzzing](#sanitizers-and-fuzzing)) |

The mapping by Catch2 tag lives in `WABCUT_TEST_LABEL_TAGS` in `tests/CMakeLists.txt`. Suites need no change, and
a new test that needs a GPU or an audio device just needs one of those tags. A test that matches a hardware
label does not get its suite name as a label. For example, the hardware decode tests are labelled
`gpu media`, because the media suite adds `media` itself.

Every discovered test has a 300 s `TIMEOUT`, so a hung test (for example a real audio device that never reopens)
fails instead of blocking the run. A suite can override it through `PROPERTIES TIMEOUT <s>`.

```powershell
ctest --preset win-msvc -L keymap          # one suite
ctest --preset win-msvc -LE "gpu|audio-device"   # what CI runs (= ctest --preset win-msvc-ci)
ctest --preset win-msvc -L install         # the install tree, the packages and the installer round trip
```

At the time of writing, `ctest --preset win-msvc` has 603 tests: 28 `gpu`, 4 `audio-device`, 1 `install`, and
the fixture setups `media.generate_test_media` and `playback.generate_perf_smoke_media`. `win-msvc-ci` runs 571 of
them. `ctest --preset core` runs 276 tests (149 model, 62 keymap, 65 interchange). The ai suite has no tests yet.

## Install tree, installer and portable ZIP

```powershell
cmake --install build\win-msvc --prefix C:\somewhere\Wabcut          # the install tree
./cmake/packaging/Install-WixToolset.ps1                             # once: WiX 5.0.2 for this user (for the MSI)
cpack --config build\win-msvc\CPackConfig.cmake                     # build\win-msvc\package\
```

`cpack` writes `wabcut-<version>-windows-x64.msi` (when WiX 5 is installed; configure reports it) and
`wabcut-<version>-windows-x64-portable.zip`. Both hold the install tree; [release.md](release.md#packages) describes
the installer (per user, no administrator rights, Start menu entry, opt-in `.wabcut` association, uninstall that keeps
user data) and why WiX. WiX is a .NET tool installed per user (`%LOCALAPPDATA%\wabcut-tools\wix\5.0.2`; it needs the
.NET SDK 6+ that Visual Studio brings); `WABCUT_WIX_DIR` points configure at another copy.

The install tree (`cmake/WabcutInstall.cmake`, packaging in `cmake/packaging/`) is self-contained. It runs on a
Windows 10/11 machine with nothing else installed:

```
bin/                      wabcut.exe, wabcut-cli.exe, and the AI helpers wabcut-infer.exe (whisper.cpp) and
                          wabcut-embed.exe (ONNX Runtime), each with version information (FileVersion, ProductName)
                          Qt libraries (windeployqt), qt.conf
                          FFmpeg, x264, x265, dav1d, libvpl, SVT-AV1, libvpx, Opus and pugixml DLLs
                          ggml-cpu-*.dll and ggml-vulkan.dll (whisper.cpp's compute modules, loaded at run time),
                          vulkan-1.dll, onnxruntime.dll and onnxruntime_providers_shared.dll
                          MSVC runtime (app-local; the UCRT ships with Windows 10+)
bin/docs/                 the guides the Home screen opens (interchange, keymap, roadmap, release)
plugins/                  Qt plugins (platforms/qwindows.dll, imageformats, ...)
qml/                      QML modules the app imports
LICENSE.txt               Wabcut's GPL-3.0-or-later
THIRD-PARTY-NOTICES.txt   every component with version and license; FFmpeg's exact configure flags; what is downloaded
                          instead of shipped (models, the llama.cpp runtime); the list of runtime libraries in the tree
licenses/                 each vcpkg port's copyright file, Qt's LGPL-3.0 text, the Boxicons MIT license (`boxicons.txt`)
```

The local assistant's runtime (llama-server) and every model are downloaded on request and never part of the tree.

- The vcpkg DLLs come from CMake's `RUNTIME_DEPENDENCY_SET`, which follows the executables' imports. Only DLLs
  that are actually loaded get shipped; modules loaded by name at run time (ggml's compute backends, ONNX Runtime's
  provider bridge, the Vulkan loader) are installed explicitly.
- Qt comes from `qt_generate_deploy_app_script`, which runs windeployqt with `--qmldir src/app/qml` so it can see
  the imports of the statically linked QML module.
- Version information: `cmake/packaging/version.rc.in` gives every executable `FileVersion <x.y.z>.<build>`,
  `ProductVersion <x.y.z> (<commit>)`, `ProductName Wabcut` and the app icon (`src/app/resources/brand`).
  `-DWABCUT_BUILD_NUMBER=<n>` sets the build number (CI: the run number).
- The notices template is `cmake/THIRD-PARTY-NOTICES.txt.in`. Versions are read from each port's
  `vcpkg.spdx.json`. FFmpeg's configure line comes from running `ffmpeg -buildconf` with the vcpkg-built tool.
- `dumpbin`, which reads the DLL imports, is recorded with its full path. The same goes for the Windows SDK
  (`WABCUT_WINDOWS_SDK_DIR`, from the configure shell's `WindowsSdkDir`), which is where windeployqt takes
  `d3dcompiler_47.dll`, `dxcompiler.dll` and `dxil.dll` from. So `cmake --install` and `cpack` give the same tree
  inside or outside the developer shell.

At the time of writing (2026-10-12, version 0.1.0): the MSI is 81.4 MiB and the portable ZIP 100.6 MiB. Both hold
the same 1,527 files, 264.8 MiB once installed: `bin/` 250.0 MiB (83 files; the AI helpers' run-time modules are
71.1 MiB of it), `qml/` 9.9 MiB, `plugins/` 4.3 MiB, `licenses/` 0.5 MiB. `cpack` takes about 35 s for the ZIP and
90 s for the MSI.

**Tests** (label `install`):

- `install.smoke` (`cmake/InstallSmokeTest.cmake`) installs into `build/<preset>/install-smoke`, then, with `PATH`
  reduced to `%SystemRoot%\System32;%SystemRoot%` and the Qt variables cleared: runs `wabcut-cli --version`, starts
  `wabcut-infer` (which must find a CPU compute module in the tree) and `wabcut-embed`, resolves every DLL the app, the
  helpers and the Qt platform plugin import (each must be in the tree or in System32; the GUI is never started), checks
  the version information, renders a tiny project headlessly with the installed CLI
  (`cmake/packaging/smoke/tiny.wabcut` plus a title added with `wabcut-cli cmd`, through the real renderer and x264),
  and checks the licenses, the FFmpeg flags and the runtime list in the notices.
- `install.package` (`cmake/packaging/PackageSmokeTest.ps1`) builds the packages with cpack and reports each one's
  size and contents; unpacks the portable ZIP and runs the CLI and the headless render from it; checks the MSI
  statically (per-user summary information, the UpgradeCode, no wildcard removals, none of the WiX generator's own
  files); then installs the MSI for the current user into a scratch folder under `%TEMP%` without elevation
  (`msiexec /i ... /qn INSTALL_ROOT=...`, with the association ticked only when this user has no `.wabcut`
  association of their own), checks the Start menu shortcut, the association, the remembered install folder and the
  version, renders from the installed tree, puts "user files" next to it, uninstalls by product code, and checks that
  every installed file, the shortcut and the association are gone and the user files are not. It is skipped (exit
  code 77) when a Wabcut MSI is already installed for this user, and reports the MSI as skipped when WiX is missing.
  The round trip runs under `%TEMP%`, not in the build folder, because Windows Installer refuses to write its log or
  install through a junction (error 1622), and build folders are often behind one.

Both run as part of `ctest --preset win-msvc` and in CI.

## Continuous integration

| Workflow | Runs on | What it does |
|---|---|---|
| `.github/workflows/core.yml` | push to `main`, every PR | The `core` preset on `windows-2025` with MSVC and with clang-cl, `ubuntu-24.04` (GCC 14) and `macos-15` (Homebrew LLVM). The Windows jobs use `dev-env.ps1 -Core`. |
| `.github/workflows/windows-full.yml` | push to `main`, every PR | See below. |
| `.github/workflows/unix-full.yml` | weekly and on demand; **experimental** | `linux-gcc` (GCC 14) and `macos-clang` (Homebrew LLVM, `arm64-osx-llvm` triplet) with the same vcpkg manifest and Qt, local AI off, smart search on the prebuilt ONNX Runtime of each platform, no secrets; tests without `gpu` / `audio-device`, then the install tree and a TGZ as an "unverified" artifact. Marked `continue-on-error`, so it never blocks. |

`windows-full.yml` does the following:

1. It clones vcpkg at the `vcpkg.json` baseline and bootstraps it.
2. It restores the vcpkg binary cache with `actions/cache`. The key is the runner image plus the hashes of
   `vcpkg.json` and the triplets. The cache is saved even when a later step fails.
3. It installs Qt 6.11.3 with `jurplel/install-qt-action` (aqtinstall from the pinned master commit, as above),
   plus `qtshadertools`.
4. It builds with `dev-env.ps1` and the `win-msvc` preset.
5. It installs WiX 5.0.2 for the runner's user (`Install-WixToolset.ps1`).
6. It runs `ctest --preset win-msvc-ci`, which skips the `gpu` and `audio-device` tests but includes the
   install tests (the MSI is installed and uninstalled for the runner's user).
7. It builds the packages with cpack and uploads the MSI and the portable ZIP as artifacts (`-DWABCUT_BUILD_NUMBER` =
   the run number).

Action versions are pinned to release tags. The first full run builds FFmpeg from source, which can take
1–2 hours; later runs restore it from the cache.

## Updating the pins

- **Qt:**
  1. Install the new kit.
  2. Update `WABCUT_TESTED_QT_VERSION` (`CMakeLists.txt`), `$wabcutTestedQt` (`tools/dev-env.ps1`), `QT_VERSION`
     (`.github/workflows/windows-full.yml`, `unix-full.yml`) and this page.
  3. For a new minor version (6.12+), also review the QRhi code, which uses private API, and the minimum check
     in `CMakeLists.txt`.
- **vcpkg baseline:**
  1. Update the vcpkg checkout (`git pull`), then run `vcpkg x-update-baseline` in the repository root. This
     sets `builtin-baseline` to the checkout's commit.
  2. Rebuild, run the full test suite, and review the regenerated `THIRD-PARTY-NOTICES.txt` (new versions, and
     new DLLs in the runtime list).
  3. Update the tables above and, if they changed, the `FetchContent` versions and hashes in
     `cmake/WabcutDependencies.cmake`.
- **aqtinstall:** once a release newer than 3.3.0 ships, replace `AQT_SOURCE` in the workflows with
  `aqtversion: ==<version>`.

## Troubleshooting

| Symptom | Fix |
|---|---|
| `dev-env: VCPKG_ROOT (...) is not a vcpkg checkout` / `vcpkg is not bootstrapped` | Clone and bootstrap vcpkg (see [Prerequisites](#prerequisites-windows)) or set `VCPKG_ROOT`. |
| `dev-env: WABCUT_QT_DIR (...) is not a Qt 6 MSVC x64 kit` / `lacks Qt6ShaderTools` | Install Qt 6.11.3 MSVC 2022 64-bit with Qt Shader Tools, or set `WABCUT_QT_DIR`. |
| `Wabcut needs Qt 6.11 or later` | A different Qt was found first: check `WABCUT_QT_DIR` / `CMAKE_PREFIX_PATH`, then delete `build/<preset>/CMakeCache.txt`. |
| `CMAKE_BUILD_TYPE=Debug cannot be used with the release-only vcpkg triplet` | Use `win-msvc` (RelWithDebInfo) or the `win-msvc-debug` preset. |
| `Wabcut needs a C++23 compiler and standard library with std::expected and std::format` | Upgrade the compiler (see the minimums above); on Linux select it with `CC` / `CXX`. |
| `install.smoke` fails with `would load X.dll from outside the install tree` | A new runtime dependency is missing from the tree. Check the `PRE_EXCLUDE_REGEXES` and `DIRECTORIES` in `cmake/WabcutInstall.cmake`. |
| Configure reports no MSI installer (WiX 5 not found) | Run `cmake/packaging/Install-WixToolset.ps1` (per user, no admin), then configure again. |
| `install.package` is skipped | A Wabcut MSI is installed for this user (the test never touches a real installation): uninstall it from Settings > Apps first. |
