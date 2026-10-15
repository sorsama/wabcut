#pragma once

// GpuScopes: the video scopes of the composited picture computed on the GPU (shaders/scopes.comp) where
// QRhi has compute, else from a downsampled readback on the CPU (wabcut::color::computeScopes) - the same binning
// either way (docs/color.md, "Scopes"). Every canvas pixel on the step grid counts, over black, as the monitor shows
// it. The program monitor records it after a frame at about 30 Hz; tests and wabcut-cli use it after an offscreen
// render. Destroying it (or releaseResources()) while a readback is pending waits for the GPU (QRhi::finish()), so
// call those outside a frame.
//
// Threading: the QRhi's thread, outside a pass, while a frame is being recorded. Results arrive through the callback
// once the GPU has finished (QRhi readback completion: at a later frame, or right after QRhi::finish()).

#include <wabcut/color/Scopes.h>
#include <wabcut/model/Error.h>
#include <wabcut/render/ColorPipeline.h>

#include <QSize>

#include <expected>
#include <functional>
#include <memory>

class QRhi;
class QRhiCommandBuffer;
class QRhiTexture;

namespace wabcut::render {

class GpuScopes {
public:
    explicit GpuScopes(QRhi* rhi);
    ~GpuScopes();
    GpuScopes(const GpuScopes&) = delete;
    GpuScopes& operator=(const GpuScopes&) = delete;

    /// Whether this QRhi runs the compute path (else record() uses the readback fallback).
    bool computeSupported() const;
    /// Force the CPU fallback (tests compare both).
    void setForceCpu(bool force) { m_forceCpu = force; }

    /// Records the scopes of `canvas` (the compositor's linear premultiplied canvas, `pixels` large) as shown through
    /// `transform`, and their readback. `done` runs on this thread when the counts are on the CPU. One request may be
    /// in flight; a call while one is pending is refused (Unsupported) - callers simply try again next frame.
    std::expected<void, Error> record(QRhiCommandBuffer* cb, QRhiTexture* canvas, QSize pixels, OutputTransform transform,
                                      const color::ScopeSettings& settings, std::function<void(color::ScopeData)> done);
    bool pending() const;

    void releaseResources();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    bool m_forceCpu = false;
};

} // namespace wabcut::render
