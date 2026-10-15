# ai_local: local inference runtime

Built only with `-DWABCUT_WITH_LOCAL_AI=ON` (default OFF; with OFF nothing here is configured and the default
dependency set is unchanged). Contract: `docs/ai.md`; protocol: `src/ai/include/wabcut/ai/InferProtocol.h`.

- `wabcut-infer`: the out-of-process whisper.cpp helper (NDJSON on stdin/stdout). The editor starts one per job and
  kills it on cancel, timeout or crash, so a GPU driver crash or out-of-memory never takes the editor down.
- `wabcut_ai_local`: what the CLI/app need around it (a WinHTTP download transport for the model store).

## Building

```powershell
. .\tools\dev-env.ps1
$env:VCPKG_MAX_CONCURRENCY = '2'
cmake --preset win-msvc -B build/win-msvc-ai -DWABCUT_WITH_LOCAL_AI=ON
cmake --build build/win-msvc-ai -j 2 --target wabcut-cli wabcut-infer wabcut_ai_tests
```

The first ON configure is heavy: vcpkg builds the overlay ports in `cmake/ports` (ggml with every x86-64 CPU
variant and the Vulkan shaders, whisper.cpp) plus shaderc, glslang, SPIRV-Tools and the Vulkan loader from source.
Build with `-j 2` (and `VCPKG_MAX_CONCURRENCY=2`) on machines with little memory. Vulkan headers, loader and
glslc all come from vcpkg; no Vulkan SDK or CUDA is needed. Later configures restore everything from the binary
cache. Use a separate build directory, so the default build keeps its own `vcpkg_installed`.

The ggml backends are run-time modules (`GGML_BACKEND_DL`): the build copies `ggml-cpu-*.dll`, `ggml-vulkan.dll`
and `vulkan-1.dll` next to `wabcut-infer`, which loads the best CPU variant and any GPU it finds at start
(`wabcut-infer --devices` lists them).

## Using it

```
wabcut-cli transcribe interview.mov --model large-v3-turbo --language auto --srt > interview.srt
```

Models download into the user cache on first use (`%LOCALAPPDATA%\Wabcut\cache\models`, SHA-256 verified,
resumable), results are cached per source in `%LOCALAPPDATA%\Wabcut\cache\analysis`. `WABCUT_CACHE_DIR`
overrides the cache root. Licenses: `THIRD_PARTY_NOTICES.md`.
