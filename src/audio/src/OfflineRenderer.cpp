#include <wabcut/audio/OfflineRenderer.h>

#include <algorithm>

namespace wabcut::audio {

std::expected<OfflineRenderer, Error> OfflineRenderer::create(std::shared_ptr<AudioSource> source, int channels,
                                                              Rate sampleRate, int maxBlockFrames) {
    if (!source) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "offline render needs a source"});
    }
    if (channels < 1 || channels > 64) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "unsupported channel count"});
    }
    if (!sampleRate.isExact()) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "sample rate is invalid or not tick-exact"});
    }
    if (maxBlockFrames < 1) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "maxBlockFrames must be positive"});
    }
    return OfflineRenderer(std::move(source), channels, sampleRate, maxBlockFrames);
}

OfflineRenderer::OfflineRenderer(std::shared_ptr<AudioSource> source, int channels, Rate sampleRate,
                                 int maxBlockFrames) noexcept
    : m_source(std::move(source)), m_channels(channels), m_sampleRate(sampleRate), m_maxBlockFrames(maxBlockFrames) {}

std::expected<void, Error> OfflineRenderer::render(std::span<float> interleaved) noexcept {
    const auto ch = static_cast<std::size_t>(m_channels);
    if (interleaved.size() % ch != 0) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "buffer is not a whole number of frames"});
    }
    const std::size_t frames = interleaved.size() / ch;
    const std::int64_t ticksPerFrame = m_sampleRate.ticksPerUnit();
    for (std::size_t done = 0; done < frames;) {
        const std::size_t n = std::min(static_cast<std::size_t>(m_maxBlockFrames), frames - done);
        m_source->render(interleaved.subspan(done * ch, n * ch), m_channels, m_sampleRate, m_position);
        m_position += Ticks{static_cast<std::int64_t>(n) * ticksPerFrame};
        done += n;
    }
    return {};
}

std::expected<std::vector<float>, Error> OfflineRenderer::render(std::int64_t frames) {
    if (frames < 0) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "negative frame count"});
    }
    std::vector<float> buffer(static_cast<std::size_t>(frames) * static_cast<std::size_t>(m_channels));
    if (auto ok = render(std::span<float>(buffer)); !ok) {
        return std::unexpected(ok.error());
    }
    return buffer;
}

} // namespace wabcut::audio
