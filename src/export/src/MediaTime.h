#pragma once

// Private: source-time helpers shared by the proxy transcoder and the trimmed copy. Media decoders measure
// source time from the media origin (the earliest start among a file's audio and video streams); files these
// write must keep that mapping exact.

#include "FfmpegCommon.h"

#include <wabcut/model/Time.h>

extern "C" {
#include <libavutil/mathematics.h>
}

namespace wabcut::exporter::detail {

/// Ticks -> units of `tb`, rounded down (a frame never moves after the source time it starts at).
inline std::int64_t ticksToUnits(Ticks t, AVRational tb) {
    return av_rescale_rnd(t.value, tb.den, static_cast<std::int64_t>(tb.num) * kTicksPerSecond,
                          static_cast<AVRounding>(AV_ROUND_DOWN | AV_ROUND_PASS_MINMAX));
}

/// Units of `tb` -> ticks (nearest).
inline Ticks unitsToTicks(std::int64_t units, AVRational tb) {
    return Ticks{av_rescale_rnd(units, static_cast<std::int64_t>(tb.num) * kTicksPerSecond, tb.den, AV_ROUND_NEAR_INF)};
}

/// The media origin as VideoDecoder computes it (media::detail::mediaOrigin): the earliest start among the
/// audio and video streams, in ticks.
inline Ticks mediaOriginOf(const AVFormatContext* fmt) {
    bool found = false;
    Ticks origin{};
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        const AVStream* st = fmt->streams[i];
        const AVMediaType type = st->codecpar->codec_type;
        if ((type != AVMEDIA_TYPE_VIDEO && type != AVMEDIA_TYPE_AUDIO) ||
            (st->disposition & AV_DISPOSITION_ATTACHED_PIC) || st->start_time == AV_NOPTS_VALUE) {
            continue;
        }
        const Ticks t = unitsToTicks(st->start_time, st->time_base);
        if (!found || t < origin) {
            origin = t;
            found = true;
        }
    }
    if (!found && fmt->start_time != AV_NOPTS_VALUE) {
        origin = Ticks{av_rescale(fmt->start_time, kTicksPerSecond, AV_TIME_BASE)};
    }
    return origin;
}

} // namespace wabcut::exporter::detail
