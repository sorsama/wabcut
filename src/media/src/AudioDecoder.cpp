#include "FfmpegUtil.h"

#include <wabcut/media/AudioDecoder.h>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
#include <libavutil/samplefmt.h>
}

#include <algorithm>
#include <cstring>
#include <numeric>
#include <vector>

namespace wabcut::media {

using namespace detail;

namespace {

/// Owning AVChannelLayout (custom-order layouts allocate).
struct OwnedAvLayout {
    AVChannelLayout layout{};
    OwnedAvLayout() = default;
    OwnedAvLayout(const OwnedAvLayout&) = delete;
    OwnedAvLayout& operator=(const OwnedAvLayout&) = delete;
    ~OwnedAvLayout() { av_channel_layout_uninit(&layout); }
};

std::int64_t floorMod(std::int64_t a, std::int64_t m) {
    const std::int64_t r = a % m;
    return r < 0 ? r + m : r;
}

} // namespace

struct AudioDecoder::Impl {
    std::string path;
    FormatContextPtr fmt;
    AVStream* stream = nullptr;
    int streamIndex = -1;
    const AVCodec* codec = nullptr;
    CodecContextPtr ctx;
    FramePtr frame{av_frame_alloc()};
    DecodeLoop loop;

    // Output format.
    int outRate = 48000;
    int outChannels = 2;
    OwnedAvLayout outLayout;

    // Resampler, created from the first frame after every reset and recreated if the input format changes.
    SwrContextPtr swr;
    int swrInRate = 0;
    int swrInFormat = AV_SAMPLE_FMT_NONE;
    OwnedAvLayout swrInLayout;
    /// Input samples per whole number of output samples: inRate / gcd(inRate, outRate).
    std::int64_t alignment = 1;

    AudioStreamInfo info;
    Ticks origin{};
    std::int64_t streamStartSample = 0; ///< Output sample at which the stream's audio begins.
    std::int64_t prerollSamples = 0;    ///< Output samples decoded and discarded before a seek target.

    // FIFO of converted, interleaved output samples. fifoStart is the output sample index of the first one.
    std::vector<float> fifo;
    std::size_t fifoHead = 0; ///< In sample frames.
    std::int64_t fifoStart = 0;
    bool positioned = false; ///< fifoStart is meaningful (false right after a seek).
    bool resync = true;      ///< The next decoded frame sets the position (after open or a seek).
    /// Expected input sample index (from the origin) of the next decoded frame.
    std::int64_t nextInputSample = 0;
    bool codecEof = false;
    bool flushed = false; ///< Resampler drained after codecEof.

    AudioDecoderStats stats;

    std::int64_t fifoFrames() const {
        return static_cast<std::int64_t>(fifo.size() / static_cast<std::size_t>(outChannels) - fifoHead);
    }
    std::int64_t fifoEnd() const { return fifoStart + fifoFrames(); }

    void dropFront(std::int64_t frames) {
        frames = std::clamp<std::int64_t>(frames, 0, fifoFrames());
        fifoHead += static_cast<std::size_t>(frames);
        fifoStart += frames;
        // Compact once the consumed prefix dominates, so the vector does not grow without bound.
        if (fifoHead > 0 && fifoHead * 2 >= fifo.size() / static_cast<std::size_t>(outChannels)) {
            fifo.erase(fifo.begin(), fifo.begin() + static_cast<std::ptrdiff_t>(fifoHead * outChannels));
            fifoHead = 0;
        }
    }

    void clearFifo() {
        fifo.clear();
        fifoHead = 0;
    }

    // -- Resampling --

    std::expected<void, Error> createResampler(const AVFrame* f) {
        OwnedAvLayout inLayout;
        if (f->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC || f->ch_layout.nb_channels <= 0) {
            av_channel_layout_default(&inLayout.layout, std::max(1, f->ch_layout.nb_channels));
        } else if (const int r = av_channel_layout_copy(&inLayout.layout, &f->ch_layout); r < 0) {
            return std::unexpected(ffmpegError(ErrorCode::DecoderError, "Bad channel layout", r));
        }
        SwrContext* raw = nullptr;
        int r = swr_alloc_set_opts2(&raw, &outLayout.layout, AV_SAMPLE_FMT_FLT, outRate, &inLayout.layout,
                                    static_cast<AVSampleFormat>(f->format), f->sample_rate, 0, nullptr);
        SwrContextPtr created(raw);
        if (r >= 0) {
            r = swr_init(created.get());
        }
        if (r < 0) {
            return std::unexpected(
                ffmpegError(ErrorCode::DecoderError, "Cannot set up the audio resampler", r));
        }
        swr = std::move(created);
        swrInRate = f->sample_rate;
        swrInFormat = f->format;
        av_channel_layout_uninit(&swrInLayout.layout);
        av_channel_layout_copy(&swrInLayout.layout, &inLayout.layout);
        alignment = swrInRate / std::gcd(swrInRate, outRate);
        return {};
    }

    bool resamplerMatches(const AVFrame* f) const {
        if (!swr || f->sample_rate != swrInRate || f->format != swrInFormat) {
            return false;
        }
        if (f->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC) {
            return f->ch_layout.nb_channels == swrInLayout.layout.nb_channels;
        }
        return av_channel_layout_compare(&f->ch_layout, &swrInLayout.layout) == 0;
    }

    /// Converts `count` input samples starting at `offset` within `f` (nullptr: drain) and appends the
    /// output.
    std::expected<void, Error> convert(const AVFrame* f, int offset, int count) {
        std::vector<const std::uint8_t*> in;
        if (f) {
            const auto format = static_cast<AVSampleFormat>(f->format);
            const int bytes = av_get_bytes_per_sample(format);
            const int channels = f->ch_layout.nb_channels;
            if (av_sample_fmt_is_planar(format)) {
                for (int c = 0; c < channels; ++c) {
                    in.push_back(f->extended_data[c] + static_cast<std::ptrdiff_t>(offset) * bytes);
                }
            } else {
                in.push_back(f->extended_data[0] + static_cast<std::ptrdiff_t>(offset) * bytes * channels);
            }
        }
        for (;;) {
            const int capacity = swr_get_out_samples(swr.get(), f ? count : 0);
            if (capacity <= 0) {
                return {};
            }
            const std::size_t oldSize = fifo.size();
            fifo.resize(oldSize + static_cast<std::size_t>(capacity) * static_cast<std::size_t>(outChannels));
            auto* out = reinterpret_cast<std::uint8_t*>(fifo.data() + oldSize);
            const int produced =
                swr_convert(swr.get(), &out, capacity, f ? in.data() : nullptr, f ? count : 0);
            if (produced < 0) {
                fifo.resize(oldSize);
                return std::unexpected(
                    ffmpegError(ErrorCode::DecoderError, "Audio resampling failed", produced));
            }
            fifo.resize(oldSize + static_cast<std::size_t>(produced) * static_cast<std::size_t>(outChannels));
            if (f || produced == 0) {
                return {};
            }
            // Draining: repeat until the resampler is empty.
        }
    }

    std::expected<void, Error> flushResampler() {
        if (swr && positioned) {
            if (auto r = convert(nullptr, 0, 0); !r) {
                return r;
            }
        }
        flushed = true;
        return {};
    }

    // -- Decoding --

    /// Decodes one codec frame (or drains at the end) and appends its converted samples to the FIFO.
    std::expected<void, Error> decodeMore() {
        if (codecEof) {
            return flushed ? std::expected<void, Error>{} : flushResampler();
        }
        auto got = loop.receive(fmt.get(), ctx.get(), streamIndex, frame.get(), path);
        if (!got) {
            return std::unexpected(got.error());
        }
        if (!*got) {
            codecEof = true;
            return flushResampler();
        }
        ++stats.framesDecoded;
        const AVFrame* f = frame.get();
        const int samples = f->nb_samples;
        if (samples <= 0 || f->sample_rate <= 0) {
            av_frame_unref(frame.get());
            return {};
        }

        if (!resamplerMatches(f)) {
            if (swr && positioned && !resync) {
                // Mid-stream format change: drain the old resampler so its tail lands before the new samples.
                if (auto r = convert(nullptr, 0, 0); !r) {
                    return r;
                }
            }
            if (auto r = createResampler(f); !r) {
                return r;
            }
        }

        // Position of this frame in input samples from the origin.
        const std::int64_t ts =
            f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
        // A timestamp that does not convert (out of range) is treated like a missing one: sample counting
        // goes on.
        std::optional<std::int64_t> stamped;
        if (ts != AV_NOPTS_VALUE) {
            if (const auto start = toTicks(ts, stream->time_base)) {
                if (const auto relative = checkedSub(*start, origin)) {
                    stamped = rescale(*relative, TimeBase{1, f->sample_rate}, Rounding::Nearest);
                }
            }
        }
        const bool hasTs = stamped.has_value();
        const std::int64_t frameStart = hasTs ? *stamped : nextInputSample;

        int skip = 0;
        if (resync) {
            // Start the (fresh) resampler on an input sample that maps to a whole output sample, so the
            // output grid after a seek is identical to the grid of a sequential decode from the start.
            const std::int64_t k = floorMod(-frameStart, alignment);
            if (k >= samples) {
                nextInputSample = frameStart + samples;
                av_frame_unref(frame.get());
                return {};
            }
            skip = static_cast<int>(k);
            const std::int64_t first = frameStart + k;
            clearFifo();
            fifoStart =
                av_rescale(first, outRate, f->sample_rate); // exact: `first` is a multiple of `alignment`
            positioned = true;
            resync = false;
            nextInputSample = first;
        } else if (hasTs) {
            // Repair timestamp gaps/overlaps beyond rounding jitter (2 ms).
            const std::int64_t drift = frameStart - nextInputSample;
            const std::int64_t tolerance = std::max<std::int64_t>(2, f->sample_rate / 500);
            if (drift > tolerance) {
                log(LogLevel::Debug, "{}: {} samples of silence for a timestamp gap", path, drift);
                const int r =
                    swr_inject_silence(swr.get(), static_cast<int>(std::min<std::int64_t>(drift, INT32_MAX)));
                if (r < 0) {
                    return std::unexpected(
                        ffmpegError(ErrorCode::DecoderError, "Cannot fill an audio gap", r));
                }
                nextInputSample += drift;
            } else if (drift < -tolerance) {
                skip = static_cast<int>(std::min<std::int64_t>(-drift, samples));
            }
        }

        if (samples > skip) {
            if (auto r = convert(f, skip, samples - skip); !r) {
                return r;
            }
            nextInputSample += samples - skip;
        }
        av_frame_unref(frame.get());
        return {};
    }

    /// (Re)creates the codec context. Done on every seek instead of avcodec_flush_buffers(): some decoders
    /// keep state across a flush (the AAC decoder's noise-substitution RNG), and a fresh context makes the
    /// samples after a seek a function of the seek point alone, independent of what was played before.
    std::expected<void, Error> openCodec() {
        CodecContextPtr context(avcodec_alloc_context3(codec));
        if (!context) {
            return std::unexpected(Error{ErrorCode::DecoderError, "Out of memory"});
        }
        int r = avcodec_parameters_to_context(context.get(), stream->codecpar);
        if (r >= 0) {
            context->pkt_timebase = stream->time_base;
            r = avcodec_open2(context.get(), codec, nullptr);
        }
        if (r < 0) {
            return std::unexpected(ffmpegError(ErrorCode::DecoderError, "Cannot open the audio decoder", r));
        }
        ctx = std::move(context);
        return {};
    }

    std::expected<void, Error> seekRaw(std::int64_t outputSample) {
        const auto target = framesToTicks(outputSample, Rate{outRate, 1}, Rounding::Floor);
        const auto absolute = target ? checkedAdd(*target, origin) : std::nullopt;
        const auto stamp = absolute ? toStreamTime(*absolute, stream->time_base) : std::nullopt;
        if (!stamp) {
            return std::unexpected(Error{ErrorCode::InvalidArgument, "Seek target out of range"});
        }
        const std::int64_t ts = *stamp;
        int r = av_seek_frame(fmt.get(), streamIndex, ts, AVSEEK_FLAG_BACKWARD);
        if (r < 0) {
            r = avformat_seek_file(fmt.get(), streamIndex, INT64_MIN, ts, ts, 0);
        }
        if (r < 0) {
            return std::unexpected(ffmpegError(ErrorCode::IoError, "Seeking in " + path + " failed", r));
        }
        ++stats.seeks;
        if (auto reopened = openCodec(); !reopened) {
            return reopened;
        }
        loop.reset();
        swr.reset();
        clearFifo();
        positioned = false;
        resync = true;
        codecEof = false;
        flushed = false;
        return {};
    }

    /// Positions the FIFO so that it starts at or before `sample`, with codec pre-roll.
    std::expected<void, Error> seekTo(std::int64_t sample) {
        std::int64_t preroll = prerollSamples;
        for (int attempt = 0;; ++attempt) {
            const std::int64_t target = sample - preroll;
            if (auto r = seekRaw(target); !r) {
                return r;
            }
            while (!positioned && !(codecEof && flushed)) {
                if (auto r = decodeMore(); !r) {
                    return r;
                }
            }
            if (!positioned) { // nothing to decode there (past the end)
                positioned = true;
                fifoStart = sample;
                return {};
            }
            // Landed too late (a coarse index)? Start earlier, unless we already asked for the stream start.
            if (fifoStart <= sample || target <= streamStartSample || attempt >= 4) {
                return {};
            }
            preroll *= 4;
        }
    }

    std::expected<AudioReadResult, Error> read(Ticks sourceTime, std::span<float> out) {
        if (out.size() % static_cast<std::size_t>(outChannels) != 0) {
            return std::unexpected(
                Error{ErrorCode::InvalidArgument, "Buffer size is not a multiple of the channel count"});
        }
        std::fill(out.begin(), out.end(), 0.0f);
        const auto frames = static_cast<std::int64_t>(out.size() / static_cast<std::size_t>(outChannels));
        if (frames == 0) {
            return AudioReadResult{};
        }
        const std::int64_t s0 = ticksToSamples(sourceTime, Rate{outRate, 1});
        const std::int64_t s1 = s0 + frames;
        if (s1 <= 0) {
            return AudioReadResult{0, static_cast<std::size_t>(frames), false}; // entirely before the origin
        }
        const std::int64_t want = std::max<std::int64_t>(s0, 0);

        const std::int64_t maxForward = outRate; // decode up to 1 s forward instead of seeking
        if (!positioned || want < fifoStart || want > fifoEnd() + maxForward) {
            if (auto r = seekTo(want); !r) {
                return std::unexpected(r.error());
            }
        }
        const std::int64_t history = outRate / 4; // keep 250 ms behind the read position for small rewinds
        while (fifoEnd() < s1 && !(codecEof && flushed)) {
            if (auto r = decodeMore(); !r) {
                return std::unexpected(r.error());
            }
            if (fifoStart < want - history) {
                dropFront(want - history - fifoStart);
            }
        }

        AudioReadResult result;
        result.endOfStream = codecEof && flushed && fifoEnd() < s1;
        const std::int64_t from = std::max(want, fifoStart);
        const std::int64_t to = std::min(s1, fifoEnd());
        if (to > from) {
            const auto channels = static_cast<std::size_t>(outChannels);
            const float* src =
                fifo.data() + (fifoHead + static_cast<std::size_t>(from - fifoStart)) * channels;
            std::memcpy(out.data() + static_cast<std::size_t>(from - s0) * channels, src,
                        static_cast<std::size_t>(to - from) * channels * sizeof(float));
            result.framesFromStream = static_cast<std::size_t>(to - from);
            result.leadingSilence = static_cast<std::size_t>(from - s0);
        } else if (!result.endOfStream) {
            result.leadingSilence = static_cast<std::size_t>(frames); // the audio starts after this block
        }
        if (fifoStart < s1 - history) {
            dropFront(s1 - history - fifoStart);
        }
        return result;
    }
};

AudioDecoder::AudioDecoder(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}
AudioDecoder::~AudioDecoder() = default;

std::string ChannelLayout::name() const {
    switch (channels) {
    case 1:
        return "mono";
    case 2:
        return "stereo";
    case 6:
        return "5.1";
    case 8:
        return "7.1";
    default:
        return std::to_string(channels) + " channels";
    }
}

std::expected<std::unique_ptr<AudioDecoder>, Error>
AudioDecoder::open(const std::string& path, int streamIndex, Rate outputRate, ChannelLayout output) {
    const int outputChannels = output.channels;
    if (!outputRate.isValid() || outputRate.num % outputRate.den != 0 ||
        outputRate.num / outputRate.den > 768000) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "Output rate must be a whole number of Hz"});
    }
    if (!output.isValid()) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "Output channel count must be 1..16"});
    }
    auto impl = std::make_unique<Impl>();
    impl->path = path;
    impl->outRate = outputRate.num / outputRate.den;
    impl->outChannels = outputChannels;
    av_channel_layout_default(&impl->outLayout.layout, outputChannels);
    if (!impl->frame || !impl->loop.isValid()) {
        return std::unexpected(Error{ErrorCode::DecoderError, "Out of memory"});
    }

    auto opened = openInput(path);
    if (!opened) {
        return std::unexpected(opened.error());
    }
    impl->fmt = std::move(*opened);
    auto selected = selectStream(impl->fmt.get(), AVMEDIA_TYPE_AUDIO, streamIndex);
    if (!selected) {
        return std::unexpected(selected.error());
    }
    impl->stream = *selected;
    impl->streamIndex = impl->stream->index;
    for (unsigned i = 0; i < impl->fmt->nb_streams; ++i) {
        impl->fmt->streams[i]->discard =
            static_cast<int>(i) == impl->streamIndex ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
    }

    impl->codec = avcodec_find_decoder(impl->stream->codecpar->codec_id);
    if (!impl->codec) {
        return std::unexpected(
            Error{ErrorCode::Unsupported,
                  std::format("No decoder for {}", avcodec_get_name(impl->stream->codecpar->codec_id))});
    }
    if (auto r = impl->openCodec(); !r) {
        return std::unexpected(r.error());
    }

    impl->info = describeAudioStream(impl->fmt.get(), impl->stream);
    impl->origin = mediaOrigin(impl->fmt.get());
    if (impl->stream->start_time != AV_NOPTS_VALUE) {
        if (const auto start = toTicks(impl->stream->start_time, impl->stream->time_base)) {
            if (const auto relative = checkedSub(*start, impl->origin)) {
                impl->streamStartSample = ticksToSamples(*relative, Rate{impl->outRate, 1});
            }
        }
    }
    // 100 ms covers MDCT overlap (AAC/MP3/AC-3: one or two frames) and the resampler's filter warm-up; codecs
    // that declare a longer pre-roll (Opus: 80 ms) are honoured on top.
    const int codecPreroll = impl->stream->codecpar->seek_preroll;
    const int inRate = std::max(1, impl->stream->codecpar->sample_rate);
    impl->prerollSamples = impl->outRate / 10 + av_rescale(std::max(0, codecPreroll), impl->outRate, inRate);
    return std::unique_ptr<AudioDecoder>(new AudioDecoder(std::move(impl)));
}

std::expected<AudioReadResult, Error> AudioDecoder::read(Ticks sourceTime, std::span<float> interleaved) {
    return m_impl->read(sourceTime, interleaved);
}

Rate AudioDecoder::outputRate() const {
    return Rate{m_impl->outRate, 1};
}

int AudioDecoder::outputChannels() const {
    return m_impl->outChannels;
}

ChannelLayout AudioDecoder::outputLayout() const {
    return ChannelLayout::fromCount(m_impl->outChannels);
}

const AudioStreamInfo& AudioDecoder::streamInfo() const {
    return m_impl->info;
}

AudioDecoderStats AudioDecoder::stats() const {
    return m_impl->stats;
}

} // namespace wabcut::media
