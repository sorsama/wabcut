#include <wabcut/ai/video/Runner.h>

#include <wabcut/ai/video/FaceParse.h>
#include <wabcut/ai/video/Gaze.h>
#include <wabcut/ai/video/ModelKeys.h>
#include <wabcut/ai/video/VideoModels.h>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace wabcut::ai::video {

Runner::Runner(IFrameSource& source, IVideoBackend& backend, ProcessedCache& cache)
    : m_source(source), m_backend(backend), m_cache(cache) {}

std::expected<Picture, Error> processPicture(const Picture& in, const AiChain& chain, IVideoBackend& backend, ProcessedCache* cache,
                                             std::stop_token stop) {
    if (!in.valid()) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "no picture to process"));
    }
    Picture current = in;
    current.alpha.clear(); // the models only see the colour; the alpha plane is resized at the end
    const bool depth = !chain.stages.empty() && chain.stages.front().effect == kAiRefocusEffect;
    const bool faces = !chain.stages.empty() && (chain.stages.front().effect == kAiFaceRetouchEffect || chain.stages.front().effect == kAiEyeContactEffect);
    if (faces) {
        // AI Face Retouch: the face models look at the picture at most kFaceMaxSide on the long side; the map is that size.
        int workWidth = 0;
        int workHeight = 0;
        faceWorkingSize(in.width, in.height, workWidth, workHeight);
        if (workWidth != current.width || workHeight != current.height) {
            current = resized(current, workWidth, workHeight);
            current.alpha.clear();
        }
    }
    if (depth) {
        // AI Refocus: the depth model looks at the picture at 518 lines on the shorter side whatever its size; the map is that size.
        int workWidth = 0;
        int workHeight = 0;
        depthWorkingSize(in.width, in.height, workWidth, workHeight);
        if (workWidth != current.width || workHeight != current.height) {
            current = resized(current, workWidth, workHeight);
            current.alpha.clear();
        }
    }
    for (const Step& step : stepsOf(chain)) {
        if (stop.stop_requested()) {
            return std::unexpected(Error(ErrorCode::Cancelled, "cancelled"));
        }
        auto ran = backend.run(current, step, stop);
        if (!ran) {
            return std::unexpected(ran.error());
        }
        Picture out = std::move(ran->picture);
        if (step.model == Model::FaceLandmarks) {
            current = std::move(out); // 0.7 AI Eye Contact: the answer is the landmark table (Gaze.h), not a picture of the input's size
            continue;
        }
        if (!out.valid() || out.width != current.width * describe(step.model).scale ||
            out.height != current.height * describe(step.model).scale) {
            return std::unexpected(Error(ErrorCode::DecoderError, "the model returned a picture of the wrong size"));
        }
        for (int i = 1; i < step.resizeDivisor; i *= 2) {
            out = halved(out); // the fast 2x: a 4x result halved
        }
        if (step.outputScale() == 1) {
            if (step.strength < 1.0) {
                out = mixed(current, out, step.strength);
            }
            if (step.model == Model::Denoise && step.grain > 0.0) {
                out = mixed(out, current, step.grain); // out + grain x (in - out): the removed noise, put back
            }
        }
        const double inputMegapixels = static_cast<double>(current.width) * current.height / 1e6;
        if (cache != nullptr && ran->milliseconds > 0.0 && inputMegapixels > 0.0 && !ran->device.empty()) {
            cache->recordSpeed(std::string(describe(step.model).key), ran->device, ran->milliseconds / inputMegapixels);
        }
        current = std::move(out);
    }
    if (in.hasAlpha() && !depth && !faces) {
        current.alpha = current.width == in.width && current.height == in.height
                            ? in.alpha
                            : resizedPlane(in.alpha, in.width, in.height, current.width, current.height);
    }
    return current;
}

std::int64_t interpolationKey(Ticks time, Ticks framePeriod) {
    if (framePeriod.value <= 0) {
        return time.value;
    }
    return static_cast<std::int64_t>(std::llround(static_cast<double>(time.value) * kInterpolationSteps / static_cast<double>(framePeriod.value)));
}

std::expected<Picture, Error> interpolatePicture(const Picture& a, const Picture& b, double timestep, IVideoBackend& backend,
                                                 ProcessedCache* cache, std::stop_token stop) {
    if (!a.valid() || !b.valid() || a.width != b.width || a.height != b.height) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "the two frames to interpolate differ in size"));
    }
    if (stop.stop_requested()) {
        return std::unexpected(Error(ErrorCode::Cancelled, "cancelled"));
    }
    Picture colourA = a;
    Picture colourB = b;
    colourA.alpha.clear();
    colourB.alpha.clear();
    auto ran = backend.interpolate(colourA, colourB, timestep, stop);
    if (!ran) {
        return std::unexpected(ran.error());
    }
    Picture out = std::move(ran->picture);
    if (!out.valid() || out.width != a.width || out.height != a.height) {
        return std::unexpected(Error(ErrorCode::DecoderError, "the model returned a picture of the wrong size"));
    }
    const double megapixels = static_cast<double>(a.width) * a.height / 1e6;
    if (cache != nullptr && ran->milliseconds > 0.0 && megapixels > 0.0 && !ran->device.empty()) {
        cache->recordSpeed(std::string(describe(Model::Interpolate).key), ran->device, ran->milliseconds / megapixels);
    }
    if (a.hasAlpha() && b.hasAlpha()) {
        out.alpha = a.alpha; // the alpha plane is not interpolated: the earlier frame's
    }
    return out;
}

namespace {

/// The stored form of a depth map: one 8-bit plane, 0 = nearest .. 255 = farthest, at the map's size.
std::expected<StoredFrame, Error> depthFrameOf(const Picture& grey, const SourceFrame& frame) {
    StoredFrame stored;
    stored.width = grey.width;
    stored.height = grey.height;
    stored.pts = frame.pts.value;
    stored.duration = frame.duration.value;
    StoredPlane plane;
    plane.width = grey.width;
    plane.height = grey.height;
    plane.bytesPerTexel = 1;
    plane.data = distanceMap8(grey);
    stored.planes.push_back(std::move(plane));
    if (!stored.valid()) {
        return std::unexpected(Error(ErrorCode::DecoderError, "the depth model returned an unusable map"));
    }
    return stored;
}

} // namespace

std::expected<StoredFrame, Error> faceStoredFrame(const Picture& map, Ticks pts, Ticks duration) {
    const FaceMapBytes bytes = faceMapFromPicture(map);
    if (!bytes.valid()) {
        return std::unexpected(Error(ErrorCode::DecoderError, "the face model returned an unusable map"));
    }
    StoredFrame stored;
    stored.width = bytes.width;
    stored.height = bytes.height;
    stored.pts = pts.value;
    stored.duration = duration.value;
    const std::size_t pixels = static_cast<std::size_t>(bytes.width) * static_cast<std::size_t>(bytes.height);
    for (std::size_t channel = 0; channel < 3; ++channel) {
        StoredPlane plane;
        plane.width = bytes.width;
        plane.height = bytes.height;
        plane.bytesPerTexel = 1;
        plane.data.resize(pixels);
        for (std::size_t i = 0; i < pixels; ++i) {
            plane.data[i] = bytes.coverage[3 * i + channel];
        }
        stored.planes.push_back(std::move(plane));
    }
    if (!stored.valid()) {
        return std::unexpected(Error(ErrorCode::DecoderError, "the face model returned an unusable map"));
    }
    return stored;
}

std::expected<RunResult, Error> Runner::runInterpolation(const Job& job, const std::function<void(const Progress&)>& progress,
                                                         std::stop_token stop) {
    if (!job.identity.valid() || job.times.empty() || job.framePeriod.value <= 0) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "nothing to interpolate"));
    }
    if (!job.provenance.chain.empty()) {
        (void)m_cache.writeProvenance(job.identity, job.provenance);
    }
    std::vector<Ticks> times = job.times;
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end()), times.end());
    RunResult result;
    Progress state;
    state.total = static_cast<int>(times.size());
    const auto report = [&] {
        if (progress) {
            progress(state);
        }
    };
    report();
    // The last pictures decoded: successive times mostly fall between the same two frames.
    struct Held {
        std::optional<SourceFrame> frame;
        std::optional<Picture> picture;
    };
    Held first;
    Held second;
    const auto picture = [&](Held& h) -> std::expected<Picture, Error> {
        if (!h.picture) {
            auto made = h.frame->picture();
            if (!made) {
                return std::unexpected(made.error());
            }
            h.picture = std::move(*made);
        }
        return *h.picture;
    };
    double smoothed = 0.0;
    for (const Ticks time : times) {
        if (stop.stop_requested()) {
            result.cancelled = true;
            break;
        }
        const std::int64_t key = interpolationKey(time, job.framePeriod);
        if (m_cache.contains(job.identity, Ticks{key}, Ticks{0})) {
            ++result.skipped;
            state.skipped = result.skipped;
            report();
            continue;
        }
        const auto started = std::chrono::steady_clock::now();
        // The frame that contains `time` and the one after it.
        if (!first.frame || !(first.frame->pts <= time && time < first.frame->pts + first.frame->duration)) {
            if (second.frame && second.frame->pts <= time && time < second.frame->pts + second.frame->duration) {
                first = std::move(second);
            } else {
                auto at = m_source.frameAt(time, stop);
                if (!at) {
                    if (at.error().code == ErrorCode::Cancelled) {
                        result.cancelled = true;
                        break;
                    }
                    return std::unexpected(at.error());
                }
                if (!*at) {
                    break; // past the end of the media
                }
                first = Held{std::move(**at), std::nullopt};
            }
            second = Held{};
        }
        if (!second.frame) {
            auto after = m_source.frameAt(first.frame->pts + std::max(first.frame->duration, Ticks{1}), stop);
            if (!after) {
                if (after.error().code == ErrorCode::Cancelled) {
                    result.cancelled = true;
                    break;
                }
                return std::unexpected(after.error());
            }
            if (*after) {
                second = Held{std::move(**after), std::nullopt};
            }
        }
        // Where between the two frames the time lies; on a frame (or with no later frame) the stored picture is that frame.
        double weight = 0.0;
        if (second.frame && second.frame->pts > first.frame->pts) {
            weight = static_cast<double>((time - first.frame->pts).value) / static_cast<double>((second.frame->pts - first.frame->pts).value);
        }
        // Strictly between the two frames the model makes the picture; on a frame (or with no later frame) the stored entry is a
        // 1 x 1 stub that only says "nothing to make here": the evaluator shows the decoded frame itself.
        std::expected<StoredFrame, Error> stored = std::unexpected(Error(ErrorCode::Unknown, ""));
        if (weight > 1.0 / 1024.0 && weight < 1.0 - 1.0 / 1024.0) {
            auto pa = picture(first);
            auto pb = picture(second);
            if (!pa || !pb) {
                return std::unexpected(!pa ? pa.error() : pb.error());
            }
            auto between = interpolatePicture(*pa, *pb, weight, m_backend, &m_cache, stop);
            if (!between) {
                if (between.error().code == ErrorCode::Cancelled) {
                    result.cancelled = true;
                    break;
                }
                return std::unexpected(between.error());
            }
            stored = first.frame->encode(*between);
        } else {
            StoredFrame stub;
            stub.width = 1;
            stub.height = 1;
            StoredPlane plane;
            plane.width = 1;
            plane.height = 1;
            plane.bytesPerTexel = 1;
            plane.data = {0};
            stub.planes.push_back(std::move(plane));
            stored = std::move(stub);
        }
        if (!stored) {
            return std::unexpected(stored.error());
        }
        stored->pts = key;
        if (auto put = m_cache.put(job.identity, *stored); !put) {
            return std::unexpected(put.error());
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        smoothed = smoothed <= 0.0 ? ms : 0.7 * smoothed + 0.3 * ms;
        ++result.processed;
        result.device = m_backend.device();
        state.processed = result.processed;
        state.msPerFrame = smoothed;
        state.device = result.device;
        const int left = std::max(0, state.total - state.processed - state.skipped);
        state.secondsLeft = left * smoothed / 1000.0;
        report();
    }
    if (result.device.empty()) {
        result.device = m_backend.device();
    }
    return result;
}

std::expected<RunResult, Error> Runner::run(const Job& job, const std::function<void(const Progress&)>& progress, std::stop_token stop) {
    if (job.kind == JobKind::Interpolation) {
        return runInterpolation(job, progress, stop);
    }
    if (!job.identity.valid() || job.chain.empty() || job.range.duration.value <= 0) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "nothing to process"));
    }
    if (!job.provenance.chain.empty()) {
        (void)m_cache.writeProvenance(job.identity, job.provenance);
    }
    RunResult result;
    Progress state;
    state.total = job.expectedFrames;
    const auto report = [&] {
        if (progress) {
            progress(state);
        }
    };
    report();
    const Ticks end = job.range.end();
    Ticks time = job.range.start;
    double smoothed = 0.0;
    while (time < end) {
        if (stop.stop_requested()) {
            result.cancelled = true;
            break;
        }
        auto frame = m_source.frameAt(time, stop);
        if (!frame) {
            if (frame.error().code == ErrorCode::Cancelled) {
                result.cancelled = true;
                break;
            }
            return std::unexpected(frame.error());
        }
        if (!*frame) {
            break; // past the end of the media
        }
        const SourceFrame& f = **frame;
        const Ticks next = std::max(f.pts + f.duration, time + Ticks{1});
        const Ticks tolerance{std::max<std::int64_t>(1, f.duration.value / 2)};
        if (m_cache.contains(job.identity, f.pts, tolerance)) {
            ++result.skipped;
            state.skipped = result.skipped;
            state.total = std::max(state.total, state.processed + state.skipped);
            report();
            time = next;
            continue;
        }
        const auto started = std::chrono::steady_clock::now();
        auto picture = f.picture();
        if (!picture) {
            return std::unexpected(picture.error());
        }
        auto made = process(*picture, job.chain, stop);
        if (!made) {
            if (made.error().code == ErrorCode::Cancelled) {
                result.cancelled = true;
                break;
            }
            return std::unexpected(made.error());
        }
        auto stored = job.kind == JobKind::Depth   ? depthFrameOf(*made, f)
                      : job.kind == JobKind::Faces ? faceStoredFrame(*made, f.pts, f.duration)
                      : job.kind == JobKind::Gaze  ? gazeStoredFrame(*made, f.pts, f.duration)
                                                   : f.encode(*made);
        if (!stored) {
            return std::unexpected(stored.error());
        }
        stored->pts = f.pts.value;
        stored->duration = f.duration.value;
        if (auto put = m_cache.put(job.identity, *stored); !put) {
            return std::unexpected(put.error());
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        smoothed = smoothed <= 0.0 ? ms : 0.7 * smoothed + 0.3 * ms;
        ++result.processed;
        result.device = m_backend.device();
        state.processed = result.processed;
        state.msPerFrame = smoothed;
        state.device = result.device;
        state.total = std::max(state.total, state.processed + state.skipped);
        const int left = std::max(0, state.total - state.processed - state.skipped);
        state.secondsLeft = left * smoothed / 1000.0;
        report();
        time = next;
    }
    if (result.device.empty()) {
        result.device = m_backend.device();
    }
    return result;
}

double estimateSeconds(const AiChain& chain, int width, int height, int frames, ProcessedCache* cache, bool gpu) {
    if (chain.empty() || width <= 0 || height <= 0 || frames <= 0) {
        return 0.0;
    }
    const char* device = gpu ? "DirectML" : "CPU";
    double perFrame = 0.0;
    int w = width;
    int h = height;
    for (const Step& s : stepsOf(chain)) {
        const double measured = cache != nullptr ? cache->speed(std::string(describe(s.model).key), device) : 0.0;
        // The depth model looks at a 518-line picture and RIFE at most 1920 on the long side (measured speeds are per megapixel of that).
        double seen = static_cast<double>(w) * h;
        if (s.model == Model::Depth) {
            int dw = 0;
            int dh = 0;
            depthWorkingSize(w, h, dw, dh);
            seen = static_cast<double>(dw) * dh;
        } else if (s.model == Model::FaceParse || s.model == Model::FaceLandmarks) {
            int fw = 0;
            int fh = 0;
            faceWorkingSize(w, h, fw, fh);
            seen = static_cast<double>(fw) * fh;
        } else if (s.model == Model::Interpolate) {
            const double k = std::min(1.0, static_cast<double>(kInterpolateMaxSide) / std::max(1, std::max(w, h)));
            seen *= k * k;
        }
        perFrame += measured > 0.0 ? measured * seen / 1e6 : estimateStepMilliseconds(s, w, h, gpu);
        w *= s.outputScale();
        h *= s.outputScale();
    }
    return perFrame * frames / 1000.0;
}

std::string durationText(double seconds) {
    if (!(seconds >= 0.0)) {
        return {};
    }
    if (seconds < 60.0) {
        return "under a minute";
    }
    const long long minutes = static_cast<long long>(std::llround(seconds / 60.0));
    if (minutes < 60) {
        return "about " + std::to_string(minutes) + " min";
    }
    const long long hours = minutes / 60;
    const long long rest = minutes % 60;
    return "about " + std::to_string(hours) + " h" + (rest > 0 ? " " + std::to_string(rest) + " min" : std::string());
}

} // namespace wabcut::ai::video
