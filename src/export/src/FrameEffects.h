#pragma once

// Private: the picture effects of an export (ExportEffects.h: Lumetri Look/LUT, Video Limiter, Image / Name / Timecode
// Overlay), applied by FrameConverter to every output frame after the ScaleMode crop / pad, on an RGBA work frame.

#include "FfmpegCommon.h"

#include <wabcut/export/ExportSettings.h>

#include <array>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace wabcut::color {
struct Lut;
}

namespace wabcut::exporter::detail {

class FrameEffects {
public:
    /// What the effects need to know about the export.
    struct Context {
        std::string sequenceName;
        std::filesystem::path output;      ///< the output file (Name Overlay)
        std::filesystem::path projectFile; ///< relative image and LUT paths resolve against its folder
        int width = 0;                     ///< the output frame
        int height = 0;
        Rate frameRate{};
        bool dropFrame = false;
        /// The sequence time of output frame 0 and the sequence's start timecode (Timecode Overlay, source Sequence).
        Ticks rangeStart{};
        Ticks sequenceStartTimecode{};
        DynamicRange dynamicRange = DynamicRange::Sdr; ///< PQ / HLG: overlay white is drawn at reference white
    };

    /// Loads the image overlay and the LUT, lays out the static overlays. Text overlays need a QGuiApplication (fonts).
    /// Errors: NotFound (the image or the LUT), InvalidArgument (unreadable files), Unsupported (no fonts).
    static std::expected<std::shared_ptr<FrameEffects>, Error> create(const ExportEffects& effects, const Context& context);
    ~FrameEffects();

    /// Applies the effects to output frame `frameIndex`, in place: `frame` is RGBA (AV_PIX_FMT_RGBA) or 16-bit RGBA
    /// (AV_PIX_FMT_RGBA64LE) at the output size, straight alpha. Thread-safe for different frames.
    void apply(AVFrame* frame, std::int64_t frameIndex) const;

    /// The text of the timecode overlay for output frame `frameIndex` ("" without one).
    std::string timecodeText(std::int64_t frameIndex) const;

private:
    struct Layer;
    FrameEffects() = default;
    void applyLook(AVFrame* frame) const;
    void applyLimiter(AVFrame* frame) const;
    void composite(AVFrame* frame, const Layer& layer) const;

    ExportEffects m_fx;
    Context m_ctx;
    static constexpr int kLutSize = 33;
    std::vector<float> m_lut; ///< kLutSize^3 RGB, red fastest (empty: no look)
    std::unique_ptr<Layer> m_image;
    std::unique_ptr<Layer> m_name;
    double m_overlayGain = 1.0; ///< overlay code values scale (HDR: reference white)
};

} // namespace wabcut::exporter::detail
