#include <wabcut/export/CaptureWriter.h>

#include "Encode.h"

#include <algorithm>
#include <format>
#include <optional>
#include <system_error>

namespace wabcut::exporter {

namespace {

using detail::FramePtr;
using detail::PacketPtr;
using detail::SwsContextPtr;
using detail::ffmpegError;

AVPixelFormat avFormatOf(CapturePixel pixel) {
    switch (pixel) {
    case CapturePixel::Nv12:
        return AV_PIX_FMT_NV12;
    case CapturePixel::Yuy2:
        return AV_PIX_FMT_YUYV422;
    case CapturePixel::Uyvy:
        return AV_PIX_FMT_UYVY422;
    case CapturePixel::Yuv420p:
        return AV_PIX_FMT_YUV420P;
    case CapturePixel::P010:
        return AV_PIX_FMT_P010LE;
    case CapturePixel::Bgr24:
        return AV_PIX_FMT_BGR24;
    case CapturePixel::Bgra:
        return AV_PIX_FMT_BGR0; // the fourth byte is unused, not alpha
    }
    return AV_PIX_FMT_NONE;
}

bool isRgb(CapturePixel pixel) {
    return pixel == CapturePixel::Bgr24 || pixel == CapturePixel::Bgra;
}

/// The colour a capture device's pictures are taken to carry: Rec.709 from 720 lines up, else the standard-definition
/// matrix of the line count (625-line PAL: BT.470 B/G, 525-line NTSC and everything else below 720 lines: SMPTE 170M).
detail::ColorTags captureColor(int height) {
    detail::ColorTags tags;
    if (height >= 720) {
        return tags; // BT.709, limited range
    }
    const bool pal = height >= 560 && height <= 600;
    tags.primaries = pal ? AVCOL_PRI_BT470BG : AVCOL_PRI_SMPTE170M;
    tags.transfer = AVCOL_TRC_SMPTE170M;
    tags.matrix = pal ? AVCOL_SPC_BT470BG : AVCOL_SPC_SMPTE170M;
    return tags;
}

} // namespace

struct CaptureWriter::Impl {
    CaptureWriterSettings settings;
    detail::Muxer muxer;
    detail::VideoEncoder video;
    AVStream* videoStream = nullptr;
    std::optional<detail::AudioEncoder> audio;
    AVStream* audioStream = nullptr;
    PacketPtr packet;
    SwsContextPtr sws;
    detail::ColorTags tags;
    std::vector<std::string> fallbacks;
    std::int64_t videoFrames = 0;
    std::int64_t audioFrames = 0;
    std::int64_t lastIndex = -1;
    bool finished = false;
};

CaptureWriter::CaptureWriter() : m_impl(std::make_unique<Impl>()) {}

CaptureWriter::~CaptureWriter() = default; // Impl's Muxer closes the file without a trailer when finish() was not reached

std::expected<std::unique_ptr<CaptureWriter>, Error> CaptureWriter::open(const CaptureWriterSettings& settings) {
    if (settings.width < 2 || settings.height < 2 || (settings.width & 1) != 0 || (settings.height & 1) != 0) {
        return std::unexpected(Error{ErrorCode::InvalidArgument,
                                     std::format("The picture size {} x {} cannot be captured (an even size is needed)",
                                                 settings.width, settings.height)});
    }
    if (!settings.frameRate.isValid() || settings.frameRate.num / settings.frameRate.den > 240) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "The frame rate cannot be captured"});
    }
    if (settings.hasAudio && (settings.audio.channels < 1 || settings.audio.channels > 8 || settings.audio.sampleRate < 8000)) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "The audio format cannot be captured"});
    }
    std::unique_ptr<CaptureWriter> writer(new CaptureWriter);
    Impl& impl = *writer->m_impl;
    impl.settings = settings;
    impl.tags = captureColor(settings.height);

    std::error_code ec;
    if (settings.file.has_parent_path()) {
        std::filesystem::create_directories(settings.file.parent_path(), ec);
    }
    auto muxer = detail::Muxer::create(settings.container, settings.file);
    if (!muxer) {
        return std::unexpected(muxer.error());
    }
    impl.muxer = std::move(*muxer);

    detail::VideoEncodeParams params;
    params.settings = settings.video;
    params.width = settings.width;
    params.height = settings.height;
    params.frameRate = settings.frameRate;
    params.globalHeader = impl.muxer.wantsGlobalHeader();
    params.primaries = impl.tags.primaries;
    params.transfer = impl.tags.transfer;
    params.matrix = impl.tags.matrix;
    params.range = impl.tags.range;
    params.sampleAspect = settings.pixelAspect.isValid() ? AVRational{settings.pixelAspect.num, settings.pixelAspect.den}
                                                          : AVRational{1, 1};
    params.realtime = true;
    auto choice = detail::chooseVideoEncoder(params);
    if (!choice) {
        return std::unexpected(choice.error());
    }
    impl.video = std::move(choice->encoder);
    impl.fallbacks = std::move(choice->fallbacks);

    auto videoStream = impl.muxer.addStream(impl.video.ctx.get());
    if (!videoStream) {
        return std::unexpected(videoStream.error());
    }
    impl.videoStream = *videoStream;
    impl.videoStream->sample_aspect_ratio = params.sampleAspect;
    impl.videoStream->codecpar->sample_aspect_ratio = params.sampleAspect;

    if (settings.hasAudio) {
        auto audio = detail::AudioEncoder::open(settings.audio, impl.muxer.wantsGlobalHeader());
        if (!audio) {
            return std::unexpected(audio.error());
        }
        impl.audio.emplace(std::move(*audio));
        auto audioStream = impl.muxer.addStream(impl.audio->context());
        if (!audioStream) {
            return std::unexpected(audioStream.error());
        }
        impl.audioStream = *audioStream;
    }

    if (!settings.title.empty()) {
        impl.muxer.setMetadata("title", settings.title);
    }
    // A file that survives the program stopping: small clusters / fragments, every packet flushed out.
    impl.muxer.setFormatOption("flush_packets", "1");
    if (settings.container == Container::Mkv || settings.container == Container::WebM) {
        impl.muxer.setFormatOption("cluster_time_limit", "1000"); // milliseconds
    } else if (settings.container == Container::Mp4 || settings.container == Container::Mov) {
        impl.muxer.setFormatOption("movflags", "+frag_keyframe+empty_moov+default_base_moof");
    }
    if (auto opened = impl.muxer.open(); !opened) {
        return std::unexpected(opened.error());
    }

    impl.packet.reset(av_packet_alloc());
    impl.sws.reset(sws_alloc_context());
    if (!impl.packet || !impl.sws) {
        return std::unexpected(Error{ErrorCode::EncoderError, "Out of memory (capture writer)"});
    }
    impl.sws->flags = SWS_BILINEAR; // same size: only the layout and the colour matrix change
    impl.sws->threads = 0;
    return writer;
}

std::expected<void, Error> CaptureWriter::writeVideo(std::int64_t frameIndex, const CapturePicture& picture) {
    Impl& impl = *m_impl;
    if (impl.finished) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "The capture file is already finished"});
    }
    if (frameIndex <= impl.lastIndex) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "Pictures must be written in order"});
    }
    const CapturePixel pixel = impl.settings.pixel;
    FramePtr src(av_frame_alloc());
    FramePtr dst(av_frame_alloc());
    if (!src || !dst) {
        return std::unexpected(Error{ErrorCode::EncoderError, "Out of memory (frame)"});
    }
    src->format = avFormatOf(pixel);
    src->width = impl.settings.width;
    src->height = impl.settings.height;
    for (int p = 0; p < 3; ++p) {
        src->data[p] = const_cast<std::uint8_t*>(picture.planes[p]);
        src->linesize[p] = picture.strides[p];
    }
    src->colorspace = isRgb(pixel) ? AVCOL_SPC_RGB : impl.tags.matrix;
    src->color_range = isRgb(pixel) ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
    dst->format = impl.video.inputFormat;
    dst->width = impl.settings.width;
    dst->height = impl.settings.height;
    if (const int r = av_frame_get_buffer(dst.get(), 0); r < 0) {
        return std::unexpected(ffmpegError(ErrorCode::EncoderError, "Cannot allocate a picture", r));
    }
    dst->colorspace = impl.tags.matrix;
    dst->color_range = impl.tags.range;
    if (const int r = sws_scale_frame(impl.sws.get(), dst.get(), src.get()); r < 0) {
        return std::unexpected(ffmpegError(ErrorCode::EncoderError, "Cannot convert a picture", r));
    }
    dst->color_primaries = impl.tags.primaries;
    dst->color_trc = impl.tags.transfer;
    dst->chroma_location = AVCHROMA_LOC_LEFT;
    dst->pts = frameIndex;
    auto ready = detail::prepareForEncoder(impl.video, std::move(dst));
    if (!ready) {
        return std::unexpected(ready.error());
    }
    if (const int r = avcodec_send_frame(impl.video.ctx.get(), ready->get()); r < 0) {
        return std::unexpected(ffmpegError(ErrorCode::EncoderError, "Cannot encode a picture", r));
    }
    if (auto drained = detail::drainEncoder(impl.video.ctx.get(), impl.packet.get(), impl.muxer, impl.videoStream, false);
        !drained) {
        return drained;
    }
    impl.lastIndex = frameIndex;
    ++impl.videoFrames;
    return {};
}

std::expected<void, Error> CaptureWriter::writeAudio(std::span<const float> interleaved) {
    Impl& impl = *m_impl;
    if (!impl.audio) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "This capture has no audio"});
    }
    const auto channels = static_cast<std::size_t>(impl.settings.audio.channels);
    if (interleaved.empty()) {
        return {};
    }
    if (interleaved.size() % channels != 0) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "Audio must be whole frames"});
    }
    if (auto ok = impl.audio->encode(interleaved, impl.muxer, impl.audioStream); !ok) {
        return ok;
    }
    impl.audioFrames += static_cast<std::int64_t>(interleaved.size() / channels);
    return {};
}

std::expected<CaptureWriteResult, Error> CaptureWriter::finish() {
    Impl& impl = *m_impl;
    if (impl.finished) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "The capture file is already finished"});
    }
    impl.finished = true;
    if (const int r = avcodec_send_frame(impl.video.ctx.get(), nullptr); r < 0) {
        return std::unexpected(ffmpegError(ErrorCode::EncoderError, "Cannot finish the picture", r));
    }
    if (auto drained = detail::drainEncoder(impl.video.ctx.get(), impl.packet.get(), impl.muxer, impl.videoStream, true);
        !drained) {
        return std::unexpected(drained.error());
    }
    if (impl.audio) {
        if (auto flushed = impl.audio->flush(impl.muxer, impl.audioStream); !flushed) {
            return std::unexpected(flushed.error());
        }
    }
    if (auto done = impl.muxer.finish(); !done) {
        return std::unexpected(done.error());
    }
    CaptureWriteResult result;
    result.file = impl.settings.file;
    result.encoder = impl.video.name;
    result.fallbacks = impl.fallbacks;
    result.videoFrames = impl.videoFrames;
    result.audioFrames = impl.audioFrames;
    return result;
}

const std::string& CaptureWriter::encoderName() const {
    return m_impl->video.name;
}

std::int64_t CaptureWriter::videoFrames() const {
    return m_impl->videoFrames;
}

std::int64_t CaptureWriter::audioFrames() const {
    return m_impl->audioFrames;
}

} // namespace wabcut::exporter
