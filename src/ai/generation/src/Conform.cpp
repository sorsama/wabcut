#include <wabcut/ai/generation/Conform.h>
#include <wabcut/media/VideoDecoder.h>
#include <wabcut/media/Probe.h>
#include <wabcut/export/ExportJob.h>
#include <wabcut/model/ProjectOps.h>
extern "C" {
#include <libswscale/swscale.h>
#include <libavutil/pixfmt.h>
}
#include <algorithm>
namespace wabcut::ai::generation {
namespace {
AVPixelFormat avFormat(media::PixelFormat f) {
    using F = media::PixelFormat;
    switch(f) {
    case F::NV12: return AV_PIX_FMT_NV12; case F::P010: return AV_PIX_FMT_P010LE;
    case F::YUV420P: return AV_PIX_FMT_YUV420P; case F::YUV420P10: return AV_PIX_FMT_YUV420P10LE;
    case F::YUV422P: return AV_PIX_FMT_YUV422P; case F::YUV422P10: return AV_PIX_FMT_YUV422P10LE;
    case F::YUV444P: return AV_PIX_FMT_YUV444P; case F::YUV444P10: return AV_PIX_FMT_YUV444P10LE;
    case F::RGBA8: return AV_PIX_FMT_RGBA; case F::YUVA420P: return AV_PIX_FMT_YUVA420P;
    case F::YUVA444P10: return AV_PIX_FMT_YUVA444P10LE; case F::Unknown: return AV_PIX_FMT_NONE;
    }
    return AV_PIX_FMT_NONE;
}
std::expected<media::Thumbnail, Error> rgba(const media::FrameHandle& frame, int width, int height) {
    const auto* cpu = frame.cpu();
    if (!cpu || avFormat(frame.format) == AV_PIX_FMT_NONE)
        return std::unexpected(Error{ErrorCode::Unsupported, "generated video has an unsupported pixel layout"});
    auto* sws = sws_getContext(frame.width, frame.height, avFormat(frame.format), width, height, AV_PIX_FMT_RGBA,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws) return std::unexpected(Error{ErrorCode::Unknown, "could not scale generated frame"});
    const int colorspace = frame.color.matrix == ColorMatrix::BT709 ? SWS_CS_ITU709 :
        frame.color.matrix == ColorMatrix::BT2020NonConstant ? SWS_CS_BT2020 : SWS_CS_ITU601;
    const int* coefficients = sws_getCoefficients(colorspace);
    sws_setColorspaceDetails(sws, coefficients, frame.color.range == ColorRange::Full ? 1 : 0,
        sws_getCoefficients(SWS_CS_ITU709), 1, 0, 1 << 16, 1 << 16);
    media::Thumbnail out; out.width = width; out.height = height; out.time = frame.pts;
    out.rgba.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
    const std::uint8_t* inputs[4]{}; int strides[4]{};
    for (int i = 0; i < 4; ++i) { inputs[i] = cpu->planes[i].data; strides[i] = cpu->planes[i].stride; }
    std::uint8_t* output[4]{out.rgba.data(), nullptr, nullptr, nullptr}; int outputStride[4]{width * 4, 0, 0, 0};
    const int rows = sws_scale(sws, inputs, strides, 0, frame.height, output, outputStride);
    sws_freeContext(sws);
    if (rows != height) return std::unexpected(Error{ErrorCode::Unknown, "incomplete generated frame"});
    return out;
}
class DecodedSource final : public exporter::IVideoFrameSource {
public:
    DecodedSource(std::unique_ptr<media::VideoDecoder> decoder, int width, int height, Rate rate, Ticks duration, bool reverse)
        : m_decoder(std::move(decoder)), m_width(width), m_height(height), m_rate(rate), m_duration(duration), m_reverse(reverse) {}
    std::string description() const override { return "Wabcut generated-media conform"; }
    int maxInFlight() const override { return 1; }
    std::expected<void, Error> request(std::int64_t index) override { m_index = index; return {}; }
    std::expected<media::FrameHandle, Error> next() override {
        const Ticks step = framesToTicks(1, m_rate);
        const Ticks time = m_reverse ? std::max(Ticks{}, m_duration - framesToTicks(m_index + 1, m_rate)) : framesToTicks(m_index, m_rate);
        auto frame = m_decoder->frameAt(time); if (!frame) return std::unexpected(frame.error());
        auto scaled = rgba(*frame, m_width, m_height); if (!scaled) return std::unexpected(scaled.error());
        auto pixels = std::make_shared<std::vector<std::uint8_t>>(std::move(scaled->rgba));
        media::FrameHandle out; out.width = m_width; out.height = m_height; out.format = media::PixelFormat::RGBA8;
        out.color.matrix = ColorMatrix::RGB; out.color.range = ColorRange::Full;
        out.color.primaries = ColorPrimaries::BT709; out.color.transfer = TransferFunction::SRGB;
        out.pts = framesToTicks(m_index, m_rate); out.duration = step;
        media::CpuPlanes cpu; cpu.owner = pixels; cpu.planes[0] = {pixels->data(), m_width * 4, m_width, m_height};
        out.payload = std::move(cpu); return out;
    }
    void stop() override {}
private:
    std::unique_ptr<media::VideoDecoder> m_decoder; int m_width, m_height; Rate m_rate; Ticks m_duration;
    bool m_reverse; std::int64_t m_index = 0;
};
}
std::expected<media::Thumbnail, Error> boundaryFrame(const std::string& path, Ticks time, std::stop_token stop) {
    if (stop.stop_requested()) return std::unexpected(Error{ErrorCode::Cancelled, "cancelled"});
    media::DecoderOptions options; options.allowHardware = false;
    auto decoder = media::VideoDecoder::open(path, -1, options); if (!decoder) return std::unexpected(decoder.error());
    auto frame = (*decoder)->frameAt(time); if (!frame) return std::unexpected(frame.error());
    const int height = std::min(frame->height, 720);
    const int width = std::max(2, frame->width * height / frame->height);
    return rgba(*frame, width, height);
}
std::expected<MediaInfo, Error> conformVideo(const std::filesystem::path& input, const std::filesystem::path& output,
    const Sequence& target, Ticks duration, bool reverse, std::stop_token stop, const std::function<void(float)>& progress) {
    auto info = media::probe(pathToUtf8(input)); if (!info) return std::unexpected(info.error());
    if (info->video.empty() || duration.value <= 0 || duration > info->duration)
        return std::unexpected(Error{ErrorCode::InvalidArgument, "generated output is shorter than requested"});
    wabcut::edit::SequenceSettings settings; settings.width = target.width; settings.height = target.height;
    settings.frameRate = target.frameRate; settings.videoTracks = 1; settings.audioTracks = 0;
    auto sequence = wabcut::edit::addSequence({}, settings); if (!sequence) return std::unexpected(sequence.error());
    auto extended = wabcut::edit::setSequenceDuration(sequence->state, sequence->id, duration);
    if (!extended) return std::unexpected(extended.error());
    exporter::ExportRequest request; request.snapshot = std::make_shared<const ProjectState>(extended->state);
    request.sequence = sequence->id; request.output = output; request.settings.audio.enabled = false;
    request.settings.video.encoder = "libx264";
    request.videoSource = [input, duration, reverse](const ProjectState&, const Sequence&, const exporter::VideoSourceFormat& f)
        -> std::expected<std::unique_ptr<exporter::IVideoFrameSource>, Error> {
        media::DecoderOptions options; options.allowHardware = false;
        auto decoder = media::VideoDecoder::open(pathToUtf8(input), -1, options);
        if (!decoder) return std::unexpected(decoder.error());
        return std::make_unique<DecodedSource>(std::move(*decoder), f.width, f.height, f.frameRate, duration, reverse);
    };
    request.onProgress = [progress](const exporter::ExportProgress& p) { if (progress) progress(static_cast<float>(p.fraction())); };
    exporter::ExportJob job(std::move(request)); auto result = job.run(stop);
    if (!result) return std::unexpected(result.error());
    auto conformed = media::probe(pathToUtf8(output)); if (!conformed) return std::unexpected(conformed.error());
    if (conformed->video.empty() || conformed->video[0].width != target.width || conformed->video[0].height != target.height ||
        conformed->video[0].frameRate != target.frameRate ||
        std::abs(conformed->duration.value - duration.value) > framesToTicks(1, target.frameRate).value)
        return std::unexpected(Error{ErrorCode::InvalidArgument, "conform verification failed"});
    return conformed;
}
}
