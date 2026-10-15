# Third-party notices: local AI (WABCUT_WITH_LOCAL_AI)

These components are built into, or shipped next to, `wabcut-infer` when Wabcut is built with
`WABCUT_WITH_LOCAL_AI=ON`. The build copies this file next to `wabcut-infer`. None of them sends telemetry.
Model weights are not part of the build: they are downloaded on first use (`wabcut::ai::ModelStore`), shown with
their license first, and verified by SHA-256.

| Component | Version | License | Shipped as |
|---|---|---|---|
| whisper.cpp | 1.9.5 | MIT | `whisper.dll` / `libwhisper` |
| ggml | 0.26.0 (from whisper.cpp 1.9.5) | MIT | `ggml*.dll`, `ggml-cpu-*.dll`, `ggml-vulkan.dll` |
| Vulkan-Loader | 1.4.357 | Apache-2.0 | `vulkan-1.dll` |
| Vulkan-Headers | 1.4.363 | Apache-2.0 OR MIT | compiled in (headers) |
| SPIRV-Headers | 1.4.357 | MIT (Khronos) | compiled in (headers) |
| shaderc / glslang / SPIRV-Tools | 2026.2 / 16.4 / 1.4.357 | Apache-2.0 / BSD-3-Clause and others / Apache-2.0 | build tools only (compile ggml's shaders); not shipped |
| OpenAI Whisper weights (ggml conversion) | tiny, base, large-v3-turbo q5_0 | MIT | downloaded on first use |
| Silero VAD (ggml conversion) | v6.2.0 | MIT | downloaded on first use |

## whisper.cpp and ggml

```
MIT License

Copyright (c) 2023-2026 The ggml authors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## OpenAI Whisper model weights

```
MIT License

Copyright (c) 2022 OpenAI

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Silero VAD

```
MIT License

Copyright (c) 2020-present Silero Team

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Vulkan-Loader

Copyright (c) 2014-2026 The Khronos Group Inc., Valve Corporation and LunarG, Inc. Licensed under the Apache
License, Version 2.0 (https://www.apache.org/licenses/LICENSE-2.0). Unless required by applicable law or agreed
to in writing, software distributed under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
CONDITIONS OF ANY KIND, either express or implied. The full license text is installed by vcpkg at
`share/vulkan-loader/copyright`.

## Vulkan-Headers and SPIRV-Headers

Copyright (c) The Khronos Group Inc. Vulkan-Headers: Apache-2.0 OR MIT. SPIRV-Headers: MIT-style Khronos license.
Full texts: `share/vulkan-headers/copyright` and `share/spirv-headers/copyright` in the vcpkg installation.
