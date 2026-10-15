#include <wabcut/ai/vision/Subjects.h>

#include <wabcut/ai/JsonAccess.h>
#include <wabcut/ai/vision/Saliency.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>

namespace wabcut::ai::vision {

namespace {

using Json = nlohmann::json;

double round4(double v) { return std::round(v * 1e4) / 1e4; }

Json boxJson(const Box& b) { return Json::array({round4(b.x), round4(b.y), round4(b.w), round4(b.h)}); }

std::optional<Box> boxFrom(const Json& j, std::size_t offset = 0) {
    if (!j.is_array() || j.size() < offset + 4) {
        return std::nullopt;
    }
    float v[4];
    for (std::size_t i = 0; i < 4; ++i) {
        const auto x = asNumber(&j[offset + i]);
        if (!x || !std::isfinite(*x)) {
            return std::nullopt;
        }
        v[i] = static_cast<float>(*x);
    }
    return Box{v[0], v[1], v[2], v[3]};
}

Json subjectsJson(const std::vector<SubjectBox>& boxes, bool withActivity) {
    Json list = Json::array();
    for (const SubjectBox& s : boxes) {
        Json row = boxJson(s.box);
        row.push_back(round4(s.score));
        row.push_back(s.track);
        if (withActivity) {
            row.push_back(round4(s.activity));
        }
        list.push_back(std::move(row));
    }
    return list;
}

bool readSubjects(const Json* list, bool withActivity, std::vector<SubjectBox>& out) {
    if (!list) {
        return true;
    }
    if (!list->is_array()) {
        return false;
    }
    for (const Json& row : *list) {
        const auto box = boxFrom(row);
        if (!box || row.size() < (withActivity ? 7u : 6u)) {
            return false;
        }
        SubjectBox s;
        s.box = *box;
        s.score = static_cast<float>(asNumber(&row[4]).value_or(0.0));
        s.track = static_cast<int>(asInt(&row[5]).value_or(-1));
        if (withActivity) {
            s.activity = static_cast<float>(asNumber(&row[6]).value_or(0.0));
        }
        out.push_back(s);
    }
    return true;
}

/// Mean picture change inside `box` (normalised) between two pictures of one size; the lower half of a face.
float regionActivity(const Image& now, const Image& before, const Box& box) {
    if (!now.valid() || !before.valid() || now.width != before.width || now.height != before.height) {
        return 0.0f;
    }
    const int x0 = std::clamp(static_cast<int>(box.x * now.width), 0, now.width - 1);
    const int x1 = std::clamp(static_cast<int>((box.x + box.w) * now.width), x0 + 1, now.width);
    const int y0 = std::clamp(static_cast<int>((box.y + box.h * 0.5f) * now.height), 0, now.height - 1);
    const int y1 = std::clamp(static_cast<int>((box.y + box.h) * now.height), y0 + 1, now.height);
    double sum = 0.0;
    std::size_t n = 0;
    for (int y = y0; y < y1; y += 2) {
        for (int x = x0; x < x1; x += 2) {
            const std::size_t at = (static_cast<std::size_t>(y) * now.width + x) * 3;
            for (int c = 0; c < 3; ++c) {
                sum += std::abs(static_cast<int>(now.rgb[at + c]) - static_cast<int>(before.rgb[at + c]));
            }
            n += 3;
        }
    }
    return n ? static_cast<float>(sum / static_cast<double>(n) / 255.0) : 0.0f;
}

std::vector<SubjectBox> tracked(ByteTracker& tracker, const std::vector<Detection>& detections, float width, float height) {
    const std::vector<int> ids = tracker.update(detections);
    std::vector<SubjectBox> out;
    for (std::size_t i = 0; i < detections.size(); ++i) {
        SubjectBox s;
        s.box = normalized(detections[i].box, width, height);
        s.score = detections[i].score;
        s.track = ids[i];
        out.push_back(s);
    }
    return out;
}

} // namespace

std::size_t SubjectAnalysis::shotOf(Ticks time) const {
    return static_cast<std::size_t>(std::ranges::upper_bound(cuts, time) - cuts.begin());
}

TimeRange SubjectAnalysis::shot(std::size_t index) const {
    const Ticks from = index == 0 ? start : cuts[std::min(index, cuts.size()) - 1];
    const Ticks to = index < cuts.size() ? cuts[index] : end;
    return TimeRange::fromStartEnd(from, std::max(from, to));
}

std::pair<std::size_t, std::size_t> countTracks(const SubjectAnalysis& analysis, std::size_t minSamples) {
    std::map<int, std::size_t> faces;
    std::map<int, std::size_t> people;
    for (const SubjectSample& s : analysis.samples) {
        for (const SubjectBox& b : s.faces) {
            ++faces[b.track];
        }
        for (const SubjectBox& b : s.people) {
            ++people[b.track];
        }
    }
    const auto lasting = [&](const std::map<int, std::size_t>& seen) {
        return static_cast<std::size_t>(std::ranges::count_if(seen, [&](const auto& kv) { return kv.first >= 0 && kv.second >= minSamples; }));
    };
    return {lasting(faces), lasting(people)};
}

std::string subjectsToJson(const SubjectAnalysis& a) {
    Json cuts = Json::array();
    for (const Ticks c : a.cuts) {
        cuts.push_back(c.value);
    }
    Json samples = Json::array();
    for (const SubjectSample& s : a.samples) {
        Json j{{"t", s.time.value}, {"m", round4(s.motion)}};
        if (!s.faces.empty()) {
            j["f"] = subjectsJson(s.faces, true);
        }
        if (!s.people.empty()) {
            j["p"] = subjectsJson(s.people, false);
        }
        if (s.salient) {
            j["s"] = boxJson(*s.salient);
        }
        samples.push_back(std::move(j));
    }
    return Json{{"version", SubjectAnalysis::kVersion}, {"width", a.frameWidth}, {"height", a.frameHeight},
                {"start", a.start.value}, {"end", a.end.value}, {"cuts", std::move(cuts)}, {"models", a.models},
                {"samples", std::move(samples)}}
        .dump();
}

std::optional<SubjectAnalysis> subjectsFromJson(std::string_view json) {
    const Json j = Json::parse(json, nullptr, false);
    if (j.is_discarded() || jsonIntOr(j, "version", 0) != SubjectAnalysis::kVersion) {
        return std::nullopt;
    }
    SubjectAnalysis a;
    a.frameWidth = static_cast<int>(jsonIntOr(j, "width", 0));
    a.frameHeight = static_cast<int>(jsonIntOr(j, "height", 0));
    a.start = Ticks{jsonIntOr(j, "start", 0)};
    a.end = Ticks{jsonIntOr(j, "end", 0)};
    a.models = jsonStringOr(j, "models");
    if (const Json* cuts = jsonArray(j, "cuts")) {
        for (const Json& c : *cuts) {
            const auto t = asInt(&c);
            if (!t) {
                return std::nullopt;
            }
            a.cuts.push_back(Ticks{*t});
        }
    }
    const Json* samples = jsonArray(j, "samples");
    if (!samples || a.end < a.start) {
        return std::nullopt;
    }
    for (const Json& s : *samples) {
        SubjectSample sample;
        const auto t = jsonInt(s, "t");
        if (!t) {
            return std::nullopt;
        }
        sample.time = Ticks{*t};
        sample.motion = static_cast<float>(jsonNumberOr(s, "m", 0.0));
        if (!readSubjects(jsonArray(s, "f"), true, sample.faces) || !readSubjects(jsonArray(s, "p"), false, sample.people)) {
            return std::nullopt;
        }
        if (const Json* salient = jsonMember(s, "s")) {
            sample.salient = boxFrom(*salient);
            if (!sample.salient) {
                return std::nullopt;
            }
        }
        a.samples.push_back(std::move(sample));
    }
    return a;
}

std::expected<SubjectAnalysis, Error> MapSubjectProvider::subjects(const MediaItem& media, int, const std::function<bool(double)>&) {
    const auto it = m_analyses.find(media.id.value);
    if (it == m_analyses.end()) {
        return std::unexpected(Error(ErrorCode::NotFound, "no subject analysis for '" + media.name + "'"));
    }
    return it->second;
}

std::optional<SubjectAnalysis> MapSubjectProvider::cached(const MediaItem& media, int) {
    const auto it = m_analyses.find(media.id.value);
    return it == m_analyses.end() ? std::nullopt : std::optional<SubjectAnalysis>(it->second);
}

// --- The pipeline -------------------------------------------------------------------------------------------------

std::expected<SubjectAnalysis, Error> analyzeSubjects(TimedFrameSource& source, SubjectDetector* detector,
                                                      const analysis::ShotAnalysis& shots, const SubjectOptions& options,
                                                      const SubjectProgress& progress, std::stop_token stop) {
    SubjectAnalysis out;
    out.start = shots.start;
    out.end = shots.end;
    out.models = detector ? detector->modelVersion() : std::string();
    for (const analysis::ShotCut& cut : shots.cuts) {
        if (cut.time > out.start && cut.time < out.end && (out.cuts.empty() || cut.time > out.cuts.back())) {
            out.cuts.push_back(cut.time);
        }
    }
    if (out.end <= out.start) {
        return out;
    }
    const double total = (out.end - out.start).toSeconds();
    const auto cancelled = [] { return std::unexpected(Error(ErrorCode::Cancelled, "subject analysis cancelled")); };

    struct Pending {
        Ticks time;
        Image picture;
        GrayImage gray;
        float motion = 0.0f;
        bool shotStart = false;
    };
    std::vector<Pending> pending;
    ByteTracker faces(options.faceTracker);
    ByteTracker people(options.personTracker);
    Image previousPicture;
    GrayImage previousGray;

    const auto flush = [&]() -> std::expected<void, Error> {
        if (pending.empty()) {
            return {};
        }
        std::vector<FrameDetections> found(pending.size());
        if (detector) {
            std::vector<Image> pictures;
            pictures.reserve(pending.size());
            for (const Pending& p : pending) {
                pictures.push_back(p.picture);
            }
            auto detected = detector->detect(pictures, stop);
            if (!detected) {
                return std::unexpected(detected.error());
            }
            if (detected->size() != pending.size()) {
                return std::unexpected(Error(ErrorCode::DecoderError, "the detector returned the wrong number of results"));
            }
            found = std::move(*detected);
        }
        for (std::size_t i = 0; i < pending.size(); ++i) {
            Pending& p = pending[i];
            if (p.shotStart) {
                faces.reset();
                people.reset();
                previousPicture = Image{};
                previousGray = GrayImage{};
            }
            const auto w = static_cast<float>(p.picture.width);
            const auto h = static_cast<float>(p.picture.height);
            SubjectSample sample;
            sample.time = p.time;
            sample.motion = p.motion;
            sample.faces = tracked(faces, found[i].faces, w, h);
            for (SubjectBox& face : sample.faces) {
                face.activity = regionActivity(p.picture, previousPicture, face.box);
            }
            sample.people = tracked(people, found[i].people, w, h);
            if (const auto region = salientRegion(p.gray, previousGray.valid() ? &previousGray : nullptr)) {
                sample.salient = region->box;
            }
            out.samples.push_back(std::move(sample));
            previousPicture = std::move(p.picture);
            previousGray = std::move(p.gray);
        }
        pending.clear();
        return {};
    };

    for (std::size_t s = 0; s < out.shotCount(); ++s) {
        const TimeRange shot = out.shot(s);
        if (shot.duration.value <= 0) {
            continue;
        }
        const Ticks baseStep{static_cast<std::int64_t>(static_cast<double>(kTicksPerSecond) / std::max(options.baseFps, 0.1))};
        const Ticks fastStep{static_cast<std::int64_t>(static_cast<double>(kTicksPerSecond) / std::max(options.maxFps, options.baseFps))};
        Ticks t = shot.start + Ticks{std::min(baseStep.value / 2, shot.duration.value / 2)};
        GrayImage lastGray;
        bool first = true;
        while (t < shot.end()) {
            if (stop.stop_requested() || out.samples.size() + pending.size() >= options.maxSamples) {
                if (stop.stop_requested()) {
                    return cancelled();
                }
                break;
            }
            auto picture = source.frameAt(t);
            if (!picture) {
                if (picture.error().code == ErrorCode::NotFound) {
                    break; // past the end of the stream
                }
                return std::unexpected(picture.error());
            }
            if (out.frameWidth == 0) { // the pictures' aspect is the video's (callers may set the real size)
                out.frameWidth = picture->width;
                out.frameHeight = picture->height;
            }
            Pending p;
            p.time = t;
            p.gray = toGray(*picture);
            p.motion = lastGray.valid() ? motionEnergy(p.gray, lastGray) : 0.0f;
            p.shotStart = first;
            p.picture = std::move(*picture);
            lastGray = p.gray;
            first = false;
            const bool moving = p.motion >= options.motionThreshold;
            pending.push_back(std::move(p));
            if (pending.size() >= std::max<std::size_t>(options.batch, 1)) {
                if (auto ok = flush(); !ok) {
                    return std::unexpected(ok.error());
                }
            }
            t += moving ? fastStep : baseStep;
            if (progress && !progress(std::clamp((t - out.start).toSeconds() / total, 0.0, 1.0))) {
                return cancelled();
            }
        }
    }
    if (auto ok = flush(); !ok) {
        return std::unexpected(ok.error());
    }
    return out;
}

} // namespace wabcut::ai::vision
