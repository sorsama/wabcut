// The manifest of an export (AI-15): what Wabcut says about the file it made. Everything comes from the exported
// snapshot; nothing here reads a picture. Prompts are never included (only their SHA-256, as AI-10 recorded it), nor
// redaction names (they can name the person a redaction hides).
#include <wabcut/ai/c2pa/ContentCredentials.h>

#include <wabcut/ai/Provenance.h>
#include <wabcut/ai/Sha256.h>
#include <wabcut/model/MediaLocator.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <format>
#include <fstream>
#include <map>
#include <set>

namespace wabcut::ai::c2pa {

namespace {

constexpr std::string_view kSourceTypeBase = "http://cv.iptc.org/newscodes/digitalsourcetype/";

struct UsedMedia {
    std::set<MediaId> media;
};

bool overlaps(const Clip& c, const TimeRange& range) { return c.start() < range.end() && c.end() > range.start; }

/// The media the exported range plays (through nested and multicam sequences too).
void collectMedia(const ProjectState& state, const Sequence& seq, const TimeRange& range, std::set<SequenceId>& visiting, UsedMedia& out) {
    for (auto kind : {TrackKind::Video, TrackKind::Audio}) {
        for (const TrackPtr& track : seq.tracks(kind)) {
            if (!track->enabled || (kind == TrackKind::Audio && track->muted)) {
                continue;
            }
            for (const Clip& clip : track->clips) {
                if (!clip.enabled || !overlaps(clip, range)) {
                    continue;
                }
                if (clip.isNested()) {
                    const Sequence* nested = state.findSequence(clip.sequence);
                    if (nested && visiting.insert(nested->id).second) {
                        collectMedia(state, *nested, TimeRange::fromStartEnd(Ticks{}, std::max(nested->contentEnd(), Ticks{1})), visiting, out);
                        visiting.erase(nested->id);
                    }
                } else if (clip.media.isValid()) {
                    out.media.insert(clip.media);
                }
            }
        }
    }
}

std::string mimeFor(const std::filesystem::path& file, const MediaItem& media) {
    std::string ext = file.extension().string();
    for (char& c : ext) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    static const std::map<std::string, std::string> kMime{
        {".mp4", "video/mp4"},   {".m4v", "video/mp4"},   {".mov", "video/quicktime"}, {".mkv", "video/x-matroska"},
        {".webm", "video/webm"}, {".avi", "video/x-msvideo"}, {".mxf", "application/mxf"}, {".wav", "audio/wav"},
        {".mp3", "audio/mpeg"},  {".m4a", "audio/mp4"},   {".aac", "audio/aac"},       {".flac", "audio/flac"},
        {".ogg", "audio/ogg"},   {".png", "image/png"},   {".jpg", "image/jpeg"},      {".jpeg", "image/jpeg"},
        {".webp", "image/webp"}, {".tif", "image/tiff"},  {".tiff", "image/tiff"},     {".heic", "image/heic"}};
    if (const auto it = kMime.find(ext); it != kMime.end()) {
        return it->second;
    }
    (void)media;
    return "application/octet-stream";
}

/// SHA-256 of a whole file (cancelled -> nullopt).
std::optional<std::pair<std::string, std::uint64_t>> hashFile(const std::filesystem::path& file, std::stop_token stop) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    Sha256 hash;
    std::vector<char> buffer(1 << 20);
    std::uint64_t total = 0;
    while (in) {
        if (stop.stop_requested()) {
            return std::nullopt;
        }
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto n = static_cast<std::size_t>(in.gcount());
        hash.update(std::string_view(buffer.data(), n));
        total += n;
    }
    return std::pair{hash.finishHex(), total};
}

std::string seconds(Ticks t) { return std::format("{:.3f}", t.toSeconds()); }

Json temporal(Ticks start, Ticks end) {
    return Json{{"type", "temporal"}, {"time", {{"type", "npt"}, {"start", seconds(start)}, {"end", seconds(end)}}}};
}

std::string nowIso() {
    return std::format("{:%Y-%m-%dT%H:%M:%SZ}", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
}

/// The output-time ranges where a redaction shows in the export: each clip of its media it applies to, mapped from
/// media time to the timeline and cut to the exported range.
std::vector<std::pair<Ticks, Ticks>> redactionSpans(const Sequence& seq, const Redaction& r, const TimeRange& range) {
    std::vector<std::pair<Ticks, Ticks>> out;
    for (const TrackPtr& track : seq.tracks(TrackKind::Video)) {
        if (!track->enabled) {
            continue;
        }
        for (const Clip& clip : track->clips) {
            if (!clip.enabled || clip.media != r.media || !r.appliesTo(clip.id) || !overlaps(clip, range)) {
                continue;
            }
            const Ticks srcStart = std::max(r.source.start, std::min(clip.sourceIn, clip.sourceOut()));
            const Ticks srcEnd = std::min(r.source.end(), std::max(clip.sourceIn, clip.sourceOut()));
            if (srcEnd <= srcStart) {
                continue;
            }
            const auto a = clip.timelineTimeOfSourceTime(srcStart);
            const auto b = clip.timelineTimeOfSourceTime(srcEnd);
            Ticks start = std::min(a.value_or(clip.start()), b.value_or(clip.end()));
            Ticks end = std::max(a.value_or(clip.start()), b.value_or(clip.end()));
            start = std::max(start, std::max(clip.start(), range.start));
            end = std::min(end, std::min(clip.end(), range.end()));
            if (end > start) {
                out.emplace_back(start - range.start, end - range.start);
            }
        }
    }
    return out;
}

} // namespace

std::string_view toString(CredentialsMode mode) {
    switch (mode) {
    case CredentialsMode::Off: return "off";
    case CredentialsMode::Ai: return "ai";
    case CredentialsMode::Always: return "always";
    }
    return "ai";
}

std::optional<CredentialsMode> credentialsModeFromString(std::string_view text) {
    if (text == "off") return CredentialsMode::Off;
    if (text == "ai") return CredentialsMode::Ai;
    if (text == "always" || text == "on") return CredentialsMode::Always;
    return std::nullopt;
}

bool containsGenerated(const ProjectState& state, SequenceId sequence, std::optional<TimeRange> range) {
    const Sequence* seq = state.findSequence(sequence);
    if (!seq) {
        return false;
    }
    UsedMedia used;
    std::set<SequenceId> visiting{seq->id};
    collectMedia(state, *seq, range.value_or(TimeRange::fromStartEnd(Ticks{}, std::max(seq->contentEnd(), Ticks{1}))), visiting, used);
    return std::ranges::any_of(used.media, [&](MediaId id) {
        const MediaItem* m = state.findMedia(id);
        return m && m->generated.has_value();
    });
}

Json manifestDefinition(const ProjectState& state, SequenceId sequence, const ManifestOptions& options) {
    const Sequence* seq = state.findSequence(sequence);
    const std::string version = options.softwareVersion.empty() ? std::string("unknown") : options.softwareVersion;
    const Json agent{{"name", "Wabcut"}, {"version", version}};
    Json ingredients = Json::array();
    Json sources = Json::array();
    Json actions = Json::array();
    Json redactions = Json::array();
    Json segments = Json::array();
    bool generated = false;
    if (seq) {
        const TimeRange range = options.range.value_or(TimeRange::fromStartEnd(Ticks{}, std::max(seq->contentEnd(), Ticks{1})));
        UsedMedia used;
        std::set<SequenceId> visiting{seq->id};
        collectMedia(state, *seq, range, visiting, used);
        for (MediaId id : used.media) {
            const MediaItem* media = state.findMedia(id);
            if (!media) {
                continue;
            }
            const auto path = resolveMediaPath(media->locator, options.projectFile);
            if (!path) {
                // Synthetic items (titles) have no file; offline media cannot be hashed.
                sources.push_back({{"title", media->name}, {"media", id.value}, {"file", false}, {"aiGenerated", media->generated.has_value()}});
                continue;
            }
            Json ingredient{{"title", media->name.empty() ? pathToUtf8(path->filename()) : media->name},
                            {"format", mimeFor(*path, *media)},
                            {"relationship", "componentOf"}};
            Json source{{"title", ingredient["title"]}, {"media", id.value}, {"file", true}, {"aiGenerated", media->generated.has_value()}};
            if (options.hashSources) {
                if (auto h = hashFile(*path, options.stop)) {
                    ingredient["hash"] = "sha256:" + h->first;
                    ingredient["instance_id"] = "xmp:iid:sha256-" + h->first.substr(0, 32);
                    source["sha256"] = h->first;
                    source["bytes"] = h->second;
                }
            }
            if (media->generated) {
                generated = true;
                ingredient["digital_source_type"] = std::string(kSourceTypeBase) + "trainedAlgorithmicMedia";
                ingredient["description"] = "AI-generated " + media->generated->kind + " (" + media->generated->operation + ") by " +
                                            media->generated->provider + " " + media->generated->model;
            }
            ingredients.push_back(std::move(ingredient));
            sources.push_back(std::move(source));
        }
        // AI-generated segments, as the unsigned report has them (provider, model, prompt hash, place in the output).
        const Json report = provenanceReport(state, sequence, range);
        if (report.contains("assertions") && report["assertions"].size() > 1) {
            segments = report["assertions"][1]["data"];
        }
        for (const Json& s : segments) {
            const Ticks start{s.value("startTicks", std::int64_t{0})};
            const Ticks end = start + Ticks{s.value("durationTicks", std::int64_t{0})};
            actions.push_back({{"action", "c2pa.edited"},
                               {"softwareAgent", agent},
                               {"digitalSourceType", std::string(kSourceTypeBase) + "trainedAlgorithmicMedia"},
                               {"description", "AI-generated " + s.value("kind", std::string("media")) + " (" +
                                                   s.value("operation", std::string()) + ") by " + s.value("provider", std::string()) + " " +
                                                   s.value("model", std::string())},
                               {"changes", Json::array({{{"region", Json::array({temporal(start, end)})}}})},
                               {"parameters", {{"org.wabcut.promptSha256", s.value("promptHash", std::string())}}}});
        }
        // Redactions applied in the export: where (output time) and how; never their names or any picture.
        for (const Redaction& r : state.redactions) {
            if (!r.enabled) {
                continue;
            }
            const auto spans = redactionSpans(*seq, r, range);
            if (spans.empty()) {
                continue;
            }
            Json regions = Json::array();
            Json times = Json::array();
            for (const auto& [start, end] : spans) {
                regions.push_back({{"region", Json::array({temporal(start, end)})}});
                times.push_back({{"start", seconds(start)}, {"end", seconds(end)}});
            }
            const std::string style(toString(r.style));
            const std::string audio = r.audio == RedactionAudio::Mute ? "mute" : r.audio == RedactionAudio::Bleep ? "bleep" : "none";
            actions.push_back({{"action", "org.wabcut.redacted"},
                               {"softwareAgent", agent},
                               {"description", "Privacy redaction applied to a region of the picture (" + style + ")" +
                                                   (audio == "none" ? std::string() : ", sound " + audio + "d")},
                               {"changes", std::move(regions)},
                               {"parameters", {{"style", style}, {"audio", audio}}}});
            redactions.push_back({{"style", style}, {"audio", audio}, {"times", std::move(times)}});
        }
    }
    // First: created in Wabcut from its sources (a composite; with AI-generated material when there is any).
    Json created{{"action", "c2pa.created"},
                 {"softwareAgent", agent},
                 {"when", nowIso()},
                 {"digitalSourceType", std::string(kSourceTypeBase) + (generated ? "compositeWithTrainedAlgorithmicMedia" : "composite")},
                 {"description", "Exported from Wabcut " + version}};
    Json edited{{"action", "c2pa.edited"}, {"softwareAgent", agent}, {"description", "Edited in Wabcut " + version}};
    actions.insert(actions.begin(), std::move(edited));
    actions.insert(actions.begin(), std::move(created));
    Json assertions = Json::array({Json{{"label", "c2pa.actions.v2"}, {"data", {{"actions", std::move(actions)}}}},
                                   Json{{"label", "org.wabcut.sources"}, {"data", {{"sources", std::move(sources)}}}},
                                   Json{{"label", "org.wabcut.ai-segments"}, {"data", {{"segments", std::move(segments)}}}},
                                   Json{{"label", "org.wabcut.redactions"}, {"data", {{"redactions", std::move(redactions)}}}}});
    // 0.2 (Premiere's Content Credentials preferences): who produced it, and a request not to train AI on it.
    if (!options.producer.empty()) {
        const std::string producer = options.producer.substr(0, 256);
        assertions.push_back(Json{{"label", "stds.schema-org.CreativeWork"},
                                  {"data", {{"@context", "http://schema.org/"},
                                            {"@type", "CreativeWork"},
                                            {"author", Json::array({{{"@type", "Person"}, {"name", producer}}})}}}});
    }
    if (options.doNotTrain) {
        const Json notAllowed{{"use", "notAllowed"}};
        assertions.push_back(Json{{"label", "cawg.training-mining"},
                                  {"data", {{"entries", {{"cawg.ai_inference", notAllowed},
                                                         {"cawg.ai_generative_training", notAllowed},
                                                         {"cawg.ai_training", notAllowed},
                                                         {"cawg.data_mining", notAllowed}}}}}});
    }
    return Json{{"claim_generator_info", Json::array({agent})},
                {"title", options.title.empty() ? std::string("Wabcut export") : options.title},
                {"format", options.format},
                {"ingredients", std::move(ingredients)},
                {"assertions", std::move(assertions)}};
}

std::function<std::string(const std::filesystem::path&, std::stop_token)> exportFinalizer(std::shared_ptr<const ProjectState> snapshot,
    SequenceId sequence, ManifestOptions options, IdentityProvider identity, std::shared_ptr<std::atomic<bool>> signedFlag) {
    return [snapshot = std::move(snapshot), sequence, options = std::move(options), identity = std::move(identity),
            signedFlag = std::move(signedFlag)](const std::filesystem::path& file, std::stop_token stop) -> std::string {
        const std::string prefix = "Content Credentials not signed: ";
        if (!snapshot) {
            return prefix + "no project snapshot";
        }
        if (!available()) {
            return prefix + "the C2PA library is not available in this build";
        }
        ManifestOptions o = options;
        o.stop = stop;
        const Json definition = manifestDefinition(*snapshot, sequence, o);
        if (stop.stop_requested()) {
            return prefix + "cancelled";
        }
        auto id = identity ? identity() : std::expected<SigningIdentity, AiError>(std::unexpected(AiError(AiErrorCode::NoCredentials, "no identity")));
        if (!id) {
            return prefix + id.error().message;
        }
        // 0.2: the thumbnail is a picture of the export itself (made by the host: the exporter's own renderer).
        std::string thumbnail;
        if (o.thumbnail) {
            thumbnail = o.thumbnail(file);
        }
        if (stop.stop_requested()) {
            return prefix + "cancelled";
        }
        auto result = signFile(file, definition, *id, stop, o.format, thumbnail);
        if (!result) {
            return prefix + result.error().message;
        }
        if (signedFlag) {
            *signedFlag = true;
        }
        return {};
    };
}

} // namespace wabcut::ai::c2pa
