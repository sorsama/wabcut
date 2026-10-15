#include <wabcut/model/AudioChannels.h>

#include <algorithm>
#include <cmath>

namespace wabcut {

std::string_view toString(ChannelFormat format) {
    switch (format) {
    case ChannelFormat::Mono:
        return "mono";
    case ChannelFormat::Stereo:
        return "stereo";
    case ChannelFormat::Surround51:
        return "5.1";
    case ChannelFormat::Adaptive:
        break;
    }
    return "adaptive";
}

std::optional<ChannelFormat> channelFormatFromString(std::string_view text) {
    for (const ChannelFormat f : {ChannelFormat::Mono, ChannelFormat::Stereo, ChannelFormat::Surround51, ChannelFormat::Adaptive}) {
        if (text == toString(f)) {
            return f;
        }
    }
    return std::nullopt;
}

AudioChannelMap AudioChannelMap::identity(int sourceChannels) {
    const int n = std::clamp(sourceChannels, 1, kMaxClipChannels);
    AudioChannelMap map;
    map.format = formatForChannels(n);
    for (int c = 0; c < n; ++c) {
        map.channels.push_back(ChannelRoute{{c}, false, ParamValue(0.0)});
    }
    return map;
}

AudioChannelMap AudioChannelMap::mono(int source) {
    AudioChannelMap map;
    map.format = ChannelFormat::Mono;
    map.channels.push_back(ChannelRoute{{std::max(source, 0)}, false, ParamValue(0.0)});
    return map;
}

AudioChannelMap AudioChannelMap::sumToMono(int sourceChannels) {
    AudioChannelMap map;
    map.format = ChannelFormat::Mono;
    ChannelRoute route;
    for (int c = 0; c < std::clamp(sourceChannels, 1, kMaxRouteSources); ++c) {
        route.sources.push_back(c);
    }
    map.channels.push_back(std::move(route));
    return map;
}

AudioChannelMap AudioChannelMap::fill(int source) {
    AudioChannelMap map;
    map.format = ChannelFormat::Stereo;
    map.channels = {ChannelRoute{{std::max(source, 0)}, false, ParamValue(0.0)}, ChannelRoute{{std::max(source, 0)}, false, ParamValue(0.0)}};
    return map;
}

AudioChannelMap AudioChannelMap::swap() {
    AudioChannelMap map;
    map.format = ChannelFormat::Stereo;
    map.channels = {ChannelRoute{{1}, false, ParamValue(0.0)}, ChannelRoute{{0}, false, ParamValue(0.0)}};
    return map;
}

std::optional<std::string> channelMapProblem(const AudioChannelMap& map) {
    const int count = map.channelCount();
    const int fixed = wabcut::channelCount(map.format);
    if (fixed > 0 ? count != fixed : (count < 1 || count > kMaxClipChannels)) {
        return "a " + std::string(toString(map.format)) + " channel map needs " +
               (fixed > 0 ? std::to_string(fixed) : "1 to " + std::to_string(kMaxClipChannels)) + " channels, not " +
               std::to_string(count);
    }
    for (int c = 0; c < count; ++c) {
        const ChannelRoute& route = map.channels[static_cast<std::size_t>(c)];
        const std::string which = "clip channel " + std::to_string(c + 1);
        if (route.sources.size() > static_cast<std::size_t>(kMaxRouteSources)) {
            return which + " sums more than " + std::to_string(kMaxRouteSources) + " source channels";
        }
        for (std::size_t k = 0; k < route.sources.size(); ++k) {
            const std::int32_t s = route.sources[k];
            if (s < 0 || s >= kMaxSourceChannels) {
                return which + " names source channel " + std::to_string(s + 1) + " (1 to " + std::to_string(kMaxSourceChannels) +
                       ")";
            }
            if (std::find(route.sources.begin(), route.sources.begin() + static_cast<std::ptrdiff_t>(k), s) !=
                route.sources.begin() + static_cast<std::ptrdiff_t>(k)) {
                return which + " names source channel " + std::to_string(s + 1) + " twice";
            }
        }
        if (!route.gainDb.isValid(-144.0, 24.0)) {
            return which + "'s volume is outside -144..+24 dB or not finite";
        }
    }
    return std::nullopt;
}

} // namespace wabcut
