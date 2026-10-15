#include <wabcut/ai/segment/Propagation.h>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace wabcut::ai::segment {

std::string_view toString(Direction direction) {
    switch (direction) {
    case Direction::None:
        return "none";
    case Direction::Forward:
        return "forward";
    case Direction::Backward:
        return "backward";
    case Direction::Both:
        return "both";
    }
    return "both";
}

std::optional<Direction> directionFromString(std::string_view text) {
    for (const Direction d : {Direction::None, Direction::Forward, Direction::Backward, Direction::Both}) {
        if (toString(d) == text) {
            return d;
        }
    }
    return std::nullopt;
}

std::expected<PropagationResult, Error> propagate(Segmenter& segmenter, FrameSource& source, const PropagationOptions& o, std::stop_token stop) {
    if (o.prompt.points.empty() && !o.prompt.box) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "point at the object or draw a box around it"));
    }
    const auto started = std::chrono::steady_clock::now();
    const Ticks fd = source.frameDuration();
    const TimeRange media = source.frames();
    if (fd.value <= 0 || media.duration.value <= 0) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "the media has no frames"));
    }
    TimeRange span = media;
    if (o.span) {
        const Ticks a = std::max(o.span->start, media.start), b = std::min(o.span->end(), media.end());
        span = b > a ? TimeRange::fromStartEnd(a, b) : TimeRange{};
    }
    // The prompt's frame on the media's grid.
    const Ticks t0 = media.start + Ticks{(o.prompt.time - media.start).value / fd.value * fd.value};
    if (!(t0 >= span.start && t0 < span.end())) {
        return std::unexpected(Error(ErrorCode::NotFound, "the prompt's frame is outside the clip"));
    }
    PropagationResult result;
    Ticks lo = span.start, hi = span.end();
    if (!o.acrossShots) {
        for (const Ticks cut : o.cuts) {
            if (cut <= t0 && cut > lo) {
                lo = cut;
                result.stoppedBefore = cut;
            } else if (cut > t0 && cut < hi) {
                hi = cut;
                result.stoppedAfter = cut;
            }
        }
    }
    const bool forward = o.direction == Direction::Forward || o.direction == Direction::Both;
    const bool backward = o.direction == Direction::Backward || o.direction == Direction::Both;
    const int nf = forward ? static_cast<int>(std::max<std::int64_t>(0, (hi - t0 - Ticks{1}).value / fd.value)) : 0;
    const int nb = backward ? static_cast<int>(std::max<std::int64_t>(0, (t0 - lo).value / fd.value)) : 0;
    if (!forward) {
        result.stoppedAfter.reset();
    }
    if (!backward) {
        result.stoppedBefore.reset();
    }
    const int total = 1 + nf + nb + (forward && backward ? 1 : 0);
    const auto [width, height] = matteSize(source.aspect(), o.longSide);
    const int size = segmenter.inputSize();
    MatteTrack& track = result.track;
    track.width = width;
    track.height = height;
    track.aspect = source.aspect();
    track.frameDuration = fd;
    Prompt prompt = o.prompt;
    prompt.time = t0;
    track.prompts = {prompt};
    if (result.stoppedBefore) {
        track.cuts.push_back(*result.stoppedBefore);
    }
    if (result.stoppedAfter) {
        track.cuts.push_back(*result.stoppedAfter);
    }

    int done = 0;
    const auto tick = [&](const std::string& what) -> bool {
        ++done;
        if (stop.stop_requested()) {
            return false;
        }
        return !o.progress || o.progress(static_cast<double>(done) / total, what);
    };
    const auto record = [&](Ticks time, const Segmentation& s) {
        MatteFrame f;
        f.time = time;
        const double certainty = 1.0 / (1.0 + std::exp(-static_cast<double>(s.objectScore)));
        f.confidence = static_cast<float>(std::clamp(static_cast<double>(s.iou), 0.0, 1.0) * (s.present ? certainty : 0.0));
        f.present = s.present;
        f.box = s.present ? boxOf(s.matte) : std::nullopt;
        f.present = f.present && f.box.has_value();
        f.rle = encodeRle(s.matte);
        result.lost += f.present ? 0 : 1;
        result.device = s.device;
        if (o.frameDone) {
            o.frameDone(f);
        }
        track.frames.push_back(std::move(f));
    };
    const auto cancelled = [] { return std::unexpected(Error(ErrorCode::Cancelled, "cancelled")); };
    const auto run = [&](bool ahead, int count) -> std::expected<void, Error> {
        auto picture = source.pictureAt(t0, size);
        if (!picture) {
            return std::unexpected(picture.error());
        }
        auto first = segmenter.prompt(*picture, prompt, 0, count + 1, width, height, stop);
        if (!first) {
            return std::unexpected(first.error());
        }
        const bool recordFirst = track.frames.empty() || track.frames.front().time != t0;
        if (recordFirst && std::ranges::none_of(track.frames, [&](const MatteFrame& f) { return f.time == t0; })) {
            record(t0, *first);
        }
        if (!tick("Segmenting the frame")) {
            return cancelled();
        }
        for (int i = 1; i <= count; ++i) {
            const Ticks t = ahead ? t0 + Ticks{fd.value * i} : t0 - Ticks{fd.value * i};
            auto p = source.pictureAt(t, size);
            if (!p) {
                return std::unexpected(p.error());
            }
            auto s = segmenter.track(*p, i, count + 1, width, height, stop);
            if (!s) {
                return std::unexpected(s.error());
            }
            record(t, *s);
            if (!tick(std::string(ahead ? "Tracking forward: frame " : "Tracking backward: frame ") + std::to_string(i) + " of " +
                      std::to_string(count))) {
                return cancelled();
            }
        }
        return {};
    };
    if (forward || !backward) {
        if (auto r = run(true, nf); !r) {
            return std::unexpected(r.error());
        }
    }
    if (backward) {
        if (auto r = run(false, nb); !r) {
            return std::unexpected(r.error());
        }
    }
    std::sort(track.frames.begin(), track.frames.end(), [](const MatteFrame& a, const MatteFrame& b) { return a.time < b.time; });
    result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    result.framesPerSecond = result.seconds > 0.0 ? static_cast<double>(track.frames.size()) / result.seconds : 0.0;
    return result;
}

} // namespace wabcut::ai::segment
