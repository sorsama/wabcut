#pragma once

// Sample-accurate audio decoding to interleaved float32 at a fixed output rate and channel layout.

#include <wabcut/model/Error.h>
#include <wabcut/model/MediaInfo.h>
#include <wabcut/model/Time.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace wabcut::media {

/// Output channel layout of an AudioDecoder. A layout is a channel count in FFmpeg's default order for that
/// count: mono; stereo (L R); 5.1 (L R C LFE Ls Rs); 7.1 (L R C LFE Lb Rb Ls Rs). Other counts use FFmpeg's
/// default too.
struct ChannelLayout {
    int channels = 2;

    static constexpr ChannelLayout mono() { return {1}; }
    static constexpr ChannelLayout stereo() { return {2}; }
    static constexpr ChannelLayout surround51() { return {6}; }
    static constexpr ChannelLayout surround71() { return {8}; }
    static constexpr ChannelLayout fromCount(int channelCount) { return {channelCount}; }

    constexpr bool isValid() const { return channels >= 1 && channels <= 16; }
    /// "mono", "stereo", "5.1", "7.1" or "N channels".
    std::string name() const;
    constexpr bool operator==(const ChannelLayout&) const = default;
};

/// What one AudioDecoder::read() delivered. The buffer always holds `leadingSilence` frames of silence, then
/// `framesFromStream` frames of decoded audio, then silence up to its end.
struct AudioReadResult {
    std::size_t framesFromStream = 0; ///< Sample frames that came from the stream.
    std::size_t leadingSilence = 0;   ///< Silent frames before them (time before the stream's audio starts).
    /// The stream ended inside (or before) this read: the silence after the decoded frames is the end of the
    /// file, and reads further on return no audio.
    bool endOfStream = false;

    /// Transitional: the rev-1 read() returned the frame count, and callers written against it keep compiling
    /// for one release.
    constexpr operator std::size_t() const { return framesFromStream; }
};

struct AudioDecoderStats {
    std::uint64_t framesDecoded = 0; ///< Codec frames (packets' worth of audio) decoded.
    std::uint64_t seeks = 0;         ///< Demuxer seeks caused by non-sequential reads.
};

/// Decodes one audio stream, converted with libswresample to interleaved float32 samples at `outputRate` in
/// the output ChannelLayout.
///
/// Time model: output sample n of the decoder covers source time samplesToTicks(n, outputRate) (source time
/// is relative to the media origin, shared with VideoDecoder). Positions are derived from the stream's
/// timestamps, so they are sample-exact whenever the container's time base resolves single samples (WAV,
/// MP4/MOV, MXF, ...). Random access seeks with a codec pre-roll into a fresh codec context and, when
/// resampling, starts the resampler on an input sample that maps to a whole output sample. A read after a
/// seek therefore lands on the same sample grid as a sequential decode and depends only on the seek point,
/// never on what was played before. It is bit-identical to a sequential decode for PCM and deterministic
/// codecs; codecs that synthesise noise from a running generator (AAC perceptual noise substitution) differ
/// only in that noise (about -100 dBFS).
///
/// Channel mapping follows libswresample's default rematrixing: other source layouts are up/down-mixed to the
/// output layout (e.g. stereo to mono at -3 dB per channel).
///
/// Threading: single-thread-affine, like VideoDecoder. Not for the audio callback thread: read() allocates
/// and may block on I/O, so feed the callback through a ring buffer filled from a decoder worker.
class AudioDecoder {
public:
    /// Opens `path` (UTF-8). `streamIndex` is the container stream index (AudioStreamInfo::index); -1 picks
    /// the best audio stream. `outputRate` must be a whole number of samples per second ({48000, 1}); the
    /// layout has 1..16 channels. Errors: NotFound, InvalidArgument, Unsupported, DecoderError, IoError.
    static std::expected<std::unique_ptr<AudioDecoder>, Error> open(const std::string& path, int streamIndex,
                                                                    Rate outputRate, ChannelLayout output);
    /// Rev-1 overload (channel count), kept for one release.
    static std::expected<std::unique_ptr<AudioDecoder>, Error> open(const std::string& path, int streamIndex,
                                                                    Rate outputRate, int outputChannels) {
        return open(path, streamIndex, outputRate, ChannelLayout::fromCount(outputChannels));
    }

    ~AudioDecoder();
    AudioDecoder(const AudioDecoder&) = delete;
    AudioDecoder& operator=(const AudioDecoder&) = delete;

    /// Fills `interleaved` (outputChannels() values per sample frame, size must be a multiple of it) with the
    /// audio that starts at the output sample containing `sourceTime`. Positions without audio (before the
    /// stream starts, negative times, past the end) are filled with silence; the result says exactly where
    /// the decoded frames are and whether the end of the stream was reached. A short or silent result is not
    /// an error. Errors: InvalidArgument (bad span size), DecoderError, IoError.
    std::expected<AudioReadResult, Error> read(Ticks sourceTime, std::span<float> interleaved);

    Rate outputRate() const;
    int outputChannels() const;
    ChannelLayout outputLayout() const;
    const AudioStreamInfo& streamInfo() const;
    AudioDecoderStats stats() const;

private:
    struct Impl;
    explicit AudioDecoder(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};

} // namespace wabcut::media
