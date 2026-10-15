#pragma once

// The production video source of an export: the fixed snapshot evaluated at each exact output time with the
// playback evaluator (playback::evaluateVideoLayers, the one the preview Scheduler uses), every visible layer
// decoded by media, composited by render::OffscreenRenderer (the monitor's Compositor and ColorPipeline, the
// export output transform, the export texture pool) and read back to RGBA8 (Principle 5: preview == export).
//
// Threads: a decode thread evaluates and decodes requested frames in order (one decoder per clip, forward
// sequential decoding); render threads, each owning a QRhi, render + read back; at most maxInFlight frames
// are requested and not yet taken (the readback backpressure). Needs a QGuiApplication in the process (any
// platform plugin; QT_QPA_PLATFORM=offscreen works headless), like every QRhi user.

#include <wabcut/export/ExportJob.h>
#include <wabcut/render/ColorPipeline.h>
#include <wabcut/render/OffscreenRenderer.h>

namespace wabcut::exporter {

struct RenderedSourceOptions {
    /// Decode with hardware decoders (d3d11va, cuda, ...) when they work. Off by default: software decoding
    /// is deterministic, so an export is reproducible bit for bit on any machine.
    bool hardwareDecode = false;
    /// Frames requested and not yet taken at most (each holds its decoded layers and one RGBA8 image).
    int maxInFlight = 4;
    /// Render threads, each with its own QRhi (1..4). An offscreen frame (upload, composite, synchronous
    /// readback) costs ~8 ms at 1080p on an RTX 3060, so two renderers working on alternate frames roughly
    /// double the throughput; frames are still delivered in order.
    int renderThreads = 2;
    /// Open decoders at most (one per clip in use); the least recently used is closed beyond that.
    int maxDecoders = 16;
    /// Renderer backend; the default honours QSG_RHI_BACKEND like the app, so preview and export agree.
    render::RhiBackendRequest backend = render::RhiBackendRequest::PlatformDefault;
    /// The sequence's colour pipeline (sequence colour settings arrive with a later model revision); its
    /// exportTransform encodes the output.
    render::ColorPipeline color{};
};

/// A factory for the rendered source. Errors at creation: DeviceError (no QRhi), Unsupported.
/// While rendering: DecoderError / IoError naming the clip's media when a visible layer cannot be decoded
/// (a source time past the end of the media shows nothing, as in the preview).
VideoSourceFactory renderedVideoSourceFactory(RenderedSourceOptions options = {});

} // namespace wabcut::exporter
