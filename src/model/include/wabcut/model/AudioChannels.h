#pragma once

// A.9: audio channel formats and channel mapping (docs/ai.md "Audio channels and 5.1 (A.9)").
//
//   * ChannelFormat: what a clip, a track or the master carries: mono (1 channel), stereo (2), 5.1 (6, in the order
//     L, R, C, LFE, Ls, Rs) or adaptive (Premiere's adaptive track: its channels pass through one to one, up to the
//     master's).
//   * AudioChannelMap: Premiere's Modify > Audio Channels for a media item or a clip: the clip's channel format and,
//     for each clip channel, the source channels that feed it (averaged when several: "sum to mono"), whether it is
//     muted and its own volume (dB, a constant or keyframes in the clip's curve time: Premiere's Channel Volume). A
//     clip without a map uses its media item's, and without either it plays as it always did (the stream decoded to the
//     track). The presets (fill left with right, swap, split to mono, sum to mono) are maps.
//
// Pure data; validation is ProjectState's (channelMapProblem).

#include <wabcut/model/Parameters.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut {

enum class ChannelFormat : std::uint8_t {
    Mono,
    Stereo,
    Surround51, ///< L, R, C, LFE, Ls, Rs (SMPTE / WAV / FFmpeg order)
    Adaptive,   ///< as many channels as given, passed through one to one
};

std::string_view toString(ChannelFormat format);
std::optional<ChannelFormat> channelFormatFromString(std::string_view text);

/// Channels of a fixed format (1, 2, 6); Adaptive: 0 (any count).
constexpr int channelCount(ChannelFormat format) {
    switch (format) {
    case ChannelFormat::Mono:
        return 1;
    case ChannelFormat::Stereo:
        return 2;
    case ChannelFormat::Surround51:
        return 6;
    case ChannelFormat::Adaptive:
        break;
    }
    return 0;
}

/// The fixed format of a channel count: 1 mono, 2 stereo, 6 5.1, anything else adaptive.
constexpr ChannelFormat formatForChannels(int channels) {
    return channels == 1 ? ChannelFormat::Mono
           : channels == 2 ? ChannelFormat::Stereo
           : channels == 6 ? ChannelFormat::Surround51
                           : ChannelFormat::Adaptive;
}

/// Bounds (validated; files past them are refused).
inline constexpr int kMaxClipChannels = 32;   ///< clip channels of an adaptive map
inline constexpr int kMaxSourceChannels = 64; ///< source channel indices are below this
inline constexpr int kMaxRouteSources = 8;    ///< sources summed into one clip channel

/// One clip channel of a map.
struct ChannelRoute {
    /// Source channels (0-based) that feed it, averaged when several; empty: silent. A source channel the stream does
    /// not have is silent.
    std::vector<std::int32_t> sources;
    bool muted = false;
    /// The channel's own volume, dB (0 = unity, -144..+24): a constant or keyframes in the clip's curve time.
    ParamValue gainDb;

    bool operator==(const ChannelRoute&) const = default;
};

struct AudioChannelMap {
    ChannelFormat format = ChannelFormat::Stereo;
    /// One route per clip channel: channelCount(format) of them (adaptive: 1..kMaxClipChannels).
    std::vector<ChannelRoute> channels;

    int channelCount() const { return static_cast<int>(channels.size()); }
    bool operator==(const AudioChannelMap&) const = default;

    // ---- Presets -------------------------------------------------------------------------------------------------
    /// Each source channel to the clip channel of the same index, in the format of that many channels: what an
    /// unmapped clip of such a stream plays (mono 1, stereo 2, 5.1 6, otherwise adaptive).
    static AudioChannelMap identity(int sourceChannels);
    /// One mono clip channel from source channel `source` (split to mono: one clip per source channel).
    static AudioChannelMap mono(int source);
    /// A mono clip of every source channel averaged.
    static AudioChannelMap sumToMono(int sourceChannels);
    /// Stereo with both sides from source channel `source` (Premiere's Fill Left with Right: source 1; Fill Right with
    /// Left: source 0).
    static AudioChannelMap fill(int source);
    /// Stereo with the two source channels exchanged (Premiere's Swap Channels).
    static AudioChannelMap swap();
};

/// Why `map` is not a valid channel map, or nullopt: the format's channel count, the bounds above, source lists of at
/// most kMaxRouteSources distinct channels below kMaxSourceChannels, gains finite in -144..+24.
std::optional<std::string> channelMapProblem(const AudioChannelMap& map);

/// The 5.1 panner of a mono or stereo track on a 5.1 master (A.9): the puck's position and the LFE send. Premiere's
/// track 5.1 panner. Values are constants or keyframes in timeline time, on the 32-frame parameter grid (interpolated
/// between grid points, like every automated audio parameter), applied after the track's effects and fader.
struct SurroundPan {
    ParamValue x{0.0};      ///< -1 = left .. +1 = right
    ParamValue y{1.0};      ///< -1 = back .. +1 = front (the default: front centre, so stereo plays L / R, mono C)
    ParamValue lfeDb{-144.0}; ///< send to the LFE channel, dB (-144 = off)

    bool isDefault() const { return x == ParamValue(0.0) && y == ParamValue(1.0) && lfeDb == ParamValue(-144.0); }
    bool operator==(const SurroundPan&) const = default;
};

} // namespace wabcut
