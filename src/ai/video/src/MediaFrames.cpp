#include <wabcut/ai/video/MediaFrames.h>

#include <wabcut/ai/video/FrameConvert.h>
#include <wabcut/ai/video/VideoModels.h>
#include <wabcut/media/VideoDecoder.h>

#include <algorithm>
#include <cmath>

namespace wabcut::ai::video {

DecoderFrameSource::DecoderFrameSource(std::string path, int containerStream, std::uint32_t generation)
    : m_path(std::move(path)), m_stream(containerStream), m_generation(generation) {}

DecoderFrameSource::~DecoderFrameSource() = default;

std::expected<std::optional<SourceFrame>, Error> DecoderFrameSource::frameAt(Ticks time, const std::stop_token& stop) {
    if (stop.stop_requested()) {
        return std::unexpected(Error(ErrorCode::Cancelled, "cancelled"));
    }
    if (!m_decoder) {
        media::DecoderOptions options;
        options.sourceGeneration = m_generation;
        auto opened = media::VideoDecoder::open(m_path, m_stream, options);
        if (!opened) {
            return std::unexpected(opened.error());
        }
        m_decoder = std::move(*opened);
    }
    auto frame = m_decoder->frameAt(time);
    if (!frame) {
        if (frame.error().code == ErrorCode::NotFound) {
            return std::optional<SourceFrame>{}; // past the last frame
        }
        return std::unexpected(frame.error());
    }
    auto handle = std::make_shared<const media::FrameHandle>(std::move(*frame));
    SourceFrame out;
    out.pts = handle->pts;
    out.duration = handle->duration;
    out.picture = [handle] { return pictureOf(*handle); };
    out.encode = [handle](const Picture& processed) { return storedFrameOf(processed, *handle); };
    return std::optional<SourceFrame>{std::move(out)};
}

int expectedFrames(const VideoStreamInfo& stream, TimeRange range) {
    if (range.duration.value <= 0 || !stream.frameRate.isValid()) {
        return 0;
    }
    const double seconds = static_cast<double>(range.duration.value) / static_cast<double>(Ticks::fromSeconds(1).value);
    return std::max(1, static_cast<int>(std::lround(seconds * stream.frameRate.toDouble())));
}

} // namespace wabcut::ai::video
