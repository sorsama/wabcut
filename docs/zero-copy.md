# Zero-copy decode → render on Windows (D3D11VA → QRhi D3D11)

Owner: Graphics/playback. Status: **integrated** (I.1, 2026-10-11; see "Integration" below). Spike proven 2026-10-11. The spike is
`tests/render/spike/zero_copy_spike.cpp` (`wabcut_zero_copy_spike`, CTest `render.zero_copy_spike`, exit code 77 where
not applicable); its output lands in `build/<preset>/test-output/render/zero_copy_spike.txt`.

## Question

Today a hardware-decoded frame makes a round trip: NVDEC writes an NV12 surface in video memory, FFmpeg copies it to
system memory (`av_hwframe_transfer_data`, inside `VideoDecoder::frameAt()`), and the `FrameImporter` uploads it back to
the GPU. Can the compositor sample the decoder's surface directly, on the same D3D11 device, with no CPU copy?

## Result: yes

Measured on this machine (NVIDIA GeForce RTX 3060, driver 617.14, Windows 11, Qt 6.11.3 QRhi D3D11, FFmpeg d3d11va from
the vcpkg tree), decoding `h264_1080p60_aac.mp4` frame 37:

| Route | Decoder surface | Pixels vs CPU path | Import cost | hwdownload (today) |
|---|---|---|---|---|
| A: QRhi's own device (what `QQuickWindow` creates) | 1920x1088 NV12, array of 25, `BIND_DECODER \| BIND_SHADER_RESOURCE` | PSNR 75.3 dB | 0.04–0.06 ms / frame | 3.2 ms / frame |
| B: app-created `D3D11_CREATE_DEVICE_VIDEO_SUPPORT` device imported into QRhi | same | PSNR 75.3 dB | 0.04–0.05 ms / frame | 3.1 ms / frame |

"CPU path" is the production path: software decode + `render::Compositor`, Rec.709 output. 75 dB is rounding-level
agreement (the remaining difference is float vs. 8-bit intermediate rounding in chroma interpolation).

What the round trip costs today (from the playback benchmark, `tests/playback/video/README.md`): ~3–6.5 ms per frame
on each decoder thread for the transfer, ~2–4 ms of upload + composite on the render thread for 3–6 layers, and 0.5–1.1
GB/s of PCIe traffic at 3–6 × 1080p60. 4K30 needs ~20 ms decode+transfer and ~7.6 ms upload per frame. Zero-copy
removes the transfer and the upload entirely.

## The recipe (what the spike does)

1. **One device.** Decoder and QRhi must use the same `ID3D11Device` (and its immediate context). FFmpeg's
   `AVD3D11VADeviceContext` wraps an existing device: set `device` and `device_context` (AddRef'd), then
   `av_hwdevice_ctx_init`.
2. **Viewable decoder surfaces.** In `get_format`, build the frames context with `avcodec_get_hw_frames_parameters`,
   then `AVD3D11VAFramesContext::BindFlags |= D3D11_BIND_SHADER_RESOURCE` and grow `initial_pool_size` by the number of
   frames the renderer may hold (mailbox + frames in flight), then `av_hwframe_ctx_init`. The surfaces are one
   `Texture2DArray`; each `AVFrame` names its slice (`data[0]` = texture, `data[1]` = slice index).
3. **Import with plane views.** Two `QRhiTexture`s per slice via `QRhi::newTextureArray` + `setArrayRange(slice, 1)` +
   `createFrom({texture, 0})`: `R8` views luma and `RG8` views chroma of the NV12 surface (D3D11 selects the plane from
   the view format; P010 uses `R16`/`RG16`). This needs `QRhi::TextureArrayRange` (D3D11: yes). The shader samples
   `sampler2DArray` at layer 0 (the view starts at the slice).
4. **Padding.** Surfaces are padded (1088 rows for 1080p); sample with `uv * displaySize / surfaceSize`.
5. **Ordering and threads.** Decode and draw are submitted to the same immediate context, so the runtime orders the
   decoder's write before the draw that samples it; no fences are needed. Because the decoder thread and the render
   thread both call into that context, enable `ID3D10Multithread::SetMultithreadProtected(TRUE)` (FFmpeg's own
   lock does not cover QRhi's calls).
6. **Lifetime.** The `AVFrame` reference pins its slice. It must stay alive until the GPU finished sampling it (the
   `FrameImporter` already retires imports only after QRhi's frames in flight) and must not be held so long that the
   decoder's pool starves. Hence the extra pool frames in step 2.

## Constraints and pitfalls

- **Use route B in production.** Route A worked on NVIDIA, but `D3D11_CREATE_DEVICE_VIDEO_SUPPORT` is the documented
  requirement for the video APIs and QRhi does not set it. Wabcut should create the device itself and hand it to Qt
  Quick (`QQuickWindow::setGraphicsDevice(QQuickGraphicsDevice::fromDeviceAndContext(dev, ctx))`, public API) and to
  the export renderer (`QRhi::create(..., importDevice)`).
- **Same adapter only.** On multi-GPU machines the decoder must run on the renderer's adapter (compare LUIDs). Across
  devices, decoder arrays cannot be shared directly; that case keeps the CPU transfer (or a GPU copy into a shared NT
  handle texture with a keyed mutex, later).
- **Device loss** (driver update, TDR): a new device means a new device generation; decoders reopen on it and the
  importer drops every import (`FrameImporter::releaseResources()` already bumps its generation).
- **Profiles.** Streams the GPU cannot decode (H.264 10-bit / 4:2:2 on pre-RTX 50) stay on the software path with CPU
  payloads, unchanged.
- **Qt private API.** `createFrom` + `setArrayRange` behaviour is QRhi's (semi-public); pinned to Qt 6.11 like the
  rest of the render module.
- **Other platforms.** D3D12 and Vulkan need shared NT handles or external-memory import; macOS maps `CVPixelBuffer`
  through `CVMetalTextureCache`; Linux imports DRM-PRIME dmabufs into Vulkan. Same payload shape, different importers.

## Integration (I.1)

What shipped, and where it differs from the plan below:

- **media**: `setSharedD3D11Device(device, context)` (`VideoDecoder.h`) wraps the renderer's device in an
  `AVD3D11VADeviceContext`, turns on `ID3D11Multithread::SetMultithreadProtected` and registers it in
  `HwDeviceRegistry` as the d3d11va device. `OutputMode::Native` decoders on it prefer d3d11va, allocate
  `BIND_SHADER_RESOURCE` surfaces with `DecoderOptions::nativeSurfaces` extra, decode with one frame thread and deliver
  `D3D11Texture{texture, arraySlice}` payloads whose owner is the `AVFrame` reference. `DecoderPoolConfig::decoderOptions`
  carries the mode into pool sessions. Everything else (other backends, software fallback, another device) stays CPU.
  The `DeviceToken` is a process counter + generation (no LUID); the renderer checks the device itself
  (`ID3D11Texture2D::GetDevice` against the QRhi's device). Tests: `tests/media/test_native_output.cpp` (frame-exact
  surfaces by staging readback, 16 frames held at once, fallback without a shared device, Cpu output unchanged, pool
  passthrough).
- **render**: `shareDeviceWithDecoders(QRhi*)` (`ZeroCopy.h`); `frameViewOf()` maps the payload to
  `FrameStorage::D3D11Texture`; `FrameImporter::importNative()` caches R8/RG8 (R16/RG16) texture-array views per
  (texture, slice) and retires the frame after the GPU like an upload; the `Compositor` has a `layer_array.frag`
  pipeline. Test: `[zerocopy]` in `tests/render/test_decoded_clip.cpp` (no upload, PSNR >= 45 dB vs the CPU path).
- **playback**: the scheduler decodes through `media::DecoderPool`; `SchedulerOptions::zeroCopy` switches it to
  `OutputMode::Native`, caps what each source holds (shorter reverse window) and sizes `nativeSurfaces` to the source
  cache + mailbox + 4. `ProgramMonitor` shares its QRhi's device on creation (opt out: `WABCUT_ZERO_COPY=0`).
- **Device: route A for now.** The monitor shares the device Qt Quick created (route A), guarded by an
  `ID3D11VideoDevice` query: a device without video support is refused and decoding stays on the CPU path. Creating
  the device in the app with `VIDEO_SUPPORT` (route B) is a follow-up in `main.cpp` (UI shell).
- **Device changes (I.3, Codex review P2-11)**: a recreated QRhi (device reset, window moved) shares a new device and the
  device generation moves. The scheduler watches `media::sharedD3D11Device()` every pass (also while paused, at least
  every 50 ms): on a change it closes every session and the pool's idle sessions (their decoders and cached surfaces
  belong to the old device), starts a `SeekDisplay::Black` epoch (old composites are dropped; the importer would refuse
  their surfaces anyway) and decodes the current frame again, so a paused picture comes back on the new device without
  a seek. Native frames of an old device that still arrive (a pool session busy during the change) are decoded again;
  until a new device is shared, decoders produce CPU frames. `SchedulerStats::deviceChanges` / `staleDeviceFrames`.
  Test: `tests/playback/video/test_device_change.cpp` (two renderers, two devices, a paused frame re-rendered on the
  second, playback continuing there).
- **Not yet**: a real TDR / device-removed test, multi-adapter (LUID) matching, D3D12/Vulkan/Metal importers.

Benchmark (`tests/playback/video/README.md`, I.1): at 6 x 1080p60 decode p50 drops from ~4.5 ms to ~0.7 ms per frame,
upload + composite from ~4.5 ms to ~0.6 ms, CPU from ~0.6 to ~0.2 cores and PCIe upload from ~1.1 GB/s to 0.

## Recommended plan (the spike's proposal): `FrameHandle` `D3D11Texture` payload

Contract rev 2 already reserves the payload; this is how to fill it in.

**media** (owner: Media engine)

```cpp
struct DeviceToken { std::uint64_t adapterLuid = 0; std::uint64_t generation = 0; };

struct D3D11Texture {
    void* texture = nullptr;       // ID3D11Texture2D* (NV12 or P010 array), kept alive by `owner`
    int arraySlice = 0;
    int surfaceWidth = 0;          // padded allocation size
    int surfaceHeight = 0;
    DeviceToken device;
    std::shared_ptr<void> owner;   // the AVFrame reference: returns the slice to the decoder pool when released
};
```

- `DecoderPool` gets the shared device: `setD3D11Device(ID3D11Device*, ID3D11DeviceContext*, DeviceToken)`. Hardware
  sessions are created on it with `BIND_SHADER_RESOURCE` surfaces and `initial_pool_size += render retention`
  (mailbox capacity + QRhi frames in flight + 2).
- `DecoderOptions::outputMode`: `Native` (payload above) or `CpuTransfer` (today). The pool picks `Native` only when
  the session's device is the shared one; any mismatch, failure or software fallback yields CPU planes.
- `using VideoFrame = FrameHandle` keeps every current caller compiling; CPU-only consumers (thumbnails, waveforms,
  tests) request `CpuTransfer`.

**render** (owner: Graphics/playback)

- `frameViewOf()` (`src/render/src/MediaFrameAdapter.cpp`, the single adapter) maps the payload to a native
  `FrameView` kind (texture, slice, surface size, format, device token).
- `FrameImporter` checks the token against its QRhi's device (LUID + generation). On a match it returns cached
  `QRhiTexture` views per (texture, slice): a decoder has ~25 slices, so a pool of 2 x 25 wrappers per decoder is created
  once and reused. On a mismatch the layer falls back to a CPU payload request (or is skipped for that frame).
- `Compositor` adds a layer-shader variant `sampler2DArray` + uv scale (a second pipeline; colour math unchanged).
- Retention stays as today: the importer keeps the frame (via the pooled composite) until the GPU is done.

**app / playback**

- Startup creates the D3D11 device (`VIDEO_SUPPORT | BGRA`, on the adapter RhiPolicy chose), hands it to
  `QQuickWindow` before the first frame and to `PlaybackController` → `DecoderPool`; export imports the same device
  into its `OffscreenRenderer`. The device is `SetMultithreadProtected(TRUE)`.

**tests**

- Promote the spike: zero-copy vs CPU PSNR >= 45 dB for NV12 and P010; a retention test (full mailbox + frames in
  flight never starve the decoder pool); a simulated device loss falls back to CPU payloads and recovers.

**Expected win** (from the measurements above): per stream-frame ~3.2 ms of decoder-thread transfer and ~0.6–1.2 ms
of render-thread upload disappear, and 0.5–1.1 GB/s of PCIe traffic at 3–6 × 1080p60 (more for 4K) is freed. That is
what makes 4K multicam and more than six 1080p60 streams realistic on mid-range GPUs.
