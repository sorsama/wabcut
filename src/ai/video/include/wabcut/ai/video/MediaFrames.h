#pragma once

// The media side of the AI video effects (0.7): which cache a media stream's pictures belong to, and the decoder-backed
// frame source the Runner reads (full-size originals, never a proxy: the models see the best picture there is).

#include <wabcut/ai/video/ProcessedCache.h>
#include <wabcut/ai/video/Runner.h>
#include <wabcut/ai/video/Targets.h>
#include <wabcut/model/AiEffects.h>
#include <wabcut/model/ProjectState.h>

#include <cstdint>
#include <memory>
#include <string>

namespace wabcut::media {
class VideoDecoder;
}

namespace wabcut::ai::video {

/// A frame source over one video stream of a file: the frame that contains a time, as the decoder returns it (hardware
/// decoding with a software fallback, read back to memory).
class DecoderFrameSource final : public IFrameSource {
public:
    /// `containerStream`: VideoStreamInfo::index of the stream (-1 = the best one).
    DecoderFrameSource(std::string path, int containerStream, std::uint32_t generation = 0);
    ~DecoderFrameSource() override;

    std::expected<std::optional<SourceFrame>, Error> frameAt(Ticks time, const std::stop_token& stop) override;

private:
    std::string m_path;
    int m_stream;
    std::uint32_t m_generation;
    std::unique_ptr<media::VideoDecoder> m_decoder;
};

/// The processed frames' length of a media stream, for the progress bar: frames in [range) at the stream's nominal rate.
int expectedFrames(const VideoStreamInfo& stream, TimeRange range);

} // namespace wabcut::ai::video
