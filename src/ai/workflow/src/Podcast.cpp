#include <wabcut/ai/workflow/Podcast.h>

#include <wabcut/ai/edit/SequenceTranscript.h>
#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/ai/shorts/Shorts.h>
#include <wabcut/audio/edit/AudioAnalysis.h>
#include <wabcut/command/Engine.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/ProjectOps.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <fstream>
#include <sstream>

namespace wabcut::ai::workflow {

using command::Json;

namespace {

double seconds(Ticks t) { return static_cast<double>(t.value) / static_cast<double>(kTicksPerSecond); }

command::CommandError error(command::ErrorCode code, std::string message) {
    command::CommandError e;
    e.code = code;
    e.message = std::move(message);
    return e;
}

std::string mediaName(const ProjectState& state, MediaId id) {
    const MediaItem* m = state.findMedia(id);
    return m ? m->name : "media " + std::to_string(id.value);
}

/// The microphones: the audio-only recordings, else the cameras (their own sound).
std::vector<MediaId> micsOf(const PodcastOptions& options) { return options.mics.empty() ? options.cameras : options.mics; }

/// Runs one catalog command on `session` with the host's services.
command::CommandResult run(EditSession& session, std::shared_ptr<command::CommandServices> services, command::ExecutionControl* control,
                           std::string name, Json params) {
    command::CommandEngine engine(session, ai::edit::editingCommands(),
                                  command::EnginePolicy{command::CapabilitySet::all(), 64, std::move(services)});
    command::CommandRequest request;
    request.command = std::move(name);
    request.params = std::move(params);
    return engine.execute(request, control);
}

/// The multicam picture clips of the podcast sequence (its multicam source's pieces on video tracks), in time order.
std::vector<const Clip*> multicamPictures(const ProjectState& state, SequenceId sequence, SequenceId multicam) {
    std::vector<const Clip*> out;
    if (const Sequence* seq = state.findSequence(sequence)) {
        for (const TrackPtr& t : seq->videoTracks) {
            for (const Clip& c : t->clips) {
                if (c.sequence == multicam) {
                    out.push_back(&c);
                }
            }
        }
    }
    std::sort(out.begin(), out.end(), [](const Clip* a, const Clip* b) { return a->start() < b->start(); });
    return out;
}

/// The microphone clips of the podcast sequence (audio clips of the mic media).
std::vector<ClipId> micClips(const ProjectState& state, SequenceId sequence, const std::vector<MediaId>& mics) {
    std::vector<ClipId> out;
    if (const Sequence* seq = state.findSequence(sequence)) {
        for (const TrackPtr& t : seq->audioTracks) {
            for (const Clip& c : t->clips) {
                if (std::ranges::find(mics, c.media) != mics.end()) {
                    out.push_back(c.id);
                }
            }
        }
    }
    return out;
}

/// The podcast sequence: the multicam picture on V1 (its first camera), each microphone on its own Dialogue track at
/// its place in the multicam source (so they stay in sync).
std::expected<std::pair<ProjectState, SequenceId>, Error> buildSequence(const ProjectState& state, SequenceId multicam,
                                                                         const PodcastOptions& options) {
    const Sequence* source = state.findSequence(multicam);
    if (!source || !source->multicam) {
        return std::unexpected(Error{ErrorCode::NotFound, "the multicam source is gone"});
    }
    wabcut::edit::SequenceSettings settings;
    settings.name = options.name;
    settings.videoTracks = 1;
    settings.audioTracks = 0;
    for (const MediaId cam : options.cameras) {
        const MediaItem* m = state.findMedia(cam);
        if (m && !m->info.video.empty() && m->info.video[0].width > 0) {
            settings.width = m->info.video[0].width;
            settings.height = m->info.video[0].height;
            if (m->info.video[0].frameRate.isValid() && m->info.video[0].frameRate.isExact()) {
                settings.frameRate = m->info.video[0].frameRate;
            }
            break;
        }
    }
    auto made = wabcut::edit::addSequence(state, settings);
    if (!made) {
        return std::unexpected(made.error());
    }
    ProjectState s = std::move(made->state);
    const SequenceId seq = made->id;
    const Ticks length = std::max(source->duration, source->contentEnd());
    // The picture.
    std::optional<std::size_t> firstVideo;
    for (std::size_t i = 0; i < source->multicam->angles.size(); ++i) {
        if (source->multicam->angles[i].video.isValid()) {
            firstVideo = i;
            break;
        }
    }
    if (firstVideo) {
        wabcut::edit::SourceClip picture;
        picture.sequence = multicam;
        picture.sourceRange = TimeRange{Ticks{}, length};
        picture.videoAngle = AngleSet::of(*firstVideo);
        picture.patches.push_back(wabcut::edit::SourcePatch{StreamRef{StreamKind::Video, 0}, s.findSequence(seq)->videoTracks.front()->id});
        auto placed = wabcut::edit::overwriteEdit(s, seq, picture, Ticks{});
        if (!placed) {
            return std::unexpected(placed.error());
        }
        s = std::move(placed->state);
    }
    // The microphones: their clips in the source, mirrored at the same times.
    const std::vector<MediaId> mics = micsOf(options);
    for (std::size_t i = 0; i < source->multicam->angles.size(); ++i) {
        const MulticamAngle& angle = source->multicam->angles[i];
        if (angle.audio.empty()) {
            continue;
        }
        const Track* audio = source->findTrack(angle.audio.front());
        if (!audio || audio->clips.empty() || std::ranges::find(mics, audio->clips.front().media) == mics.end()) {
            continue;
        }
        auto track = wabcut::edit::addTrack(s, seq, TrackKind::Audio, angle.name.empty() ? mediaName(s, audio->clips.front().media) : angle.name);
        if (!track) {
            return std::unexpected(track.error());
        }
        s = std::move(track->state);
        wabcut::edit::TrackStateChange role;
        role.role = TrackRole::Dialogue;
        auto roled = wabcut::edit::setTrackState(s, seq, track->id, role);
        if (!roled) {
            return std::unexpected(roled.error());
        }
        s = std::move(roled->state);
        for (const Clip& c : audio->clips) {
            if (c.isNested()) {
                continue;
            }
            wabcut::edit::SourceClip clip;
            clip.media = c.media;
            clip.sourceRange = TimeRange::fromStartEnd(c.sourceIn, c.sourceOut());
            clip.patches.push_back(wabcut::edit::SourcePatch{StreamRef{StreamKind::Audio, c.stream.index}, track->id});
            auto placed = wabcut::edit::overwriteEdit(s, seq, clip, c.start());
            if (!placed) {
                return std::unexpected(placed.error());
            }
            s = std::move(placed->state);
        }
    }
    return std::pair{std::move(s), seq};
}

bool hasTranscript(const ProjectState& state, command::CommandServices* services, MediaId media) {
    auto* t = command::findService<ai::edit::TranscriptServices>(services);
    const MediaItem* m = state.findMedia(media);
    return t && t->transcripts && m && !m->info.audio.empty() && t->transcripts->transcript(*m, 0) != nullptr;
}

std::string nowIso() {
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    return std::format("{:%Y-%m-%dT%H:%M:%SZ}", now);
}

} // namespace

std::string_view toString(StepKind kind) {
    switch (kind) {
    case StepKind::Multicam:
        return "multicam";
    case StepKind::Sequence:
        return "sequence";
    case StepKind::Transcribe:
        return "transcribe";
    case StepKind::Enhance:
        return "enhance";
    case StepKind::Loudness:
        return "loudness";
    case StepKind::Cleanup:
        return "cleanup";
    case StepKind::AutoSwitch:
        return "autoswitch";
    case StepKind::Captions:
        return "captions";
    case StepKind::Chapters:
        return "chapters";
    case StepKind::Shorts:
        return "shorts";
    }
    return "multicam";
}

std::vector<StepKind> allSteps() {
    return {StepKind::Multicam, StepKind::Sequence, StepKind::Transcribe, StepKind::Enhance, StepKind::Loudness,
            StepKind::Cleanup,  StepKind::AutoSwitch, StepKind::Captions, StepKind::Chapters, StepKind::Shorts};
}

std::optional<StepKind> stepFromString(std::string_view text) {
    for (const StepKind k : allSteps()) {
        if (toString(k) == text) {
            return k;
        }
    }
    return std::nullopt;
}

Json contextToJson(const PodcastContext& context) {
    Json transcribed = Json::array();
    for (const MediaId m : context.transcribed) {
        transcribed.push_back(m.value);
    }
    return Json{{"multicam", context.multicam.value}, {"sequence", context.sequence.value}, {"transcribed", std::move(transcribed)},
                {"youtubeChapters", context.youtubeChapters}};
}

PodcastContext contextFromJson(const Json& json) {
    PodcastContext c;
    if (!json.is_object()) {
        return c;
    }
    c.multicam = SequenceId{json.value("multicam", std::uint64_t{0})};
    c.sequence = SequenceId{json.value("sequence", std::uint64_t{0})};
    if (const auto it = json.find("transcribed"); it != json.end() && it->is_array()) {
        for (const Json& m : *it) {
            if (m.is_number_unsigned() || m.is_number_integer()) {
                c.transcribed.push_back(MediaId{m.get<std::uint64_t>()});
            }
        }
    }
    c.youtubeChapters = json.value("youtubeChapters", std::string());
    return c;
}

PodcastPlan planPodcast(const ProjectState& state, const PodcastOptions& options, const PodcastContext& context,
                        command::CommandServices* services) {
    PodcastPlan plan;
    const auto* transcribe = command::findService<ai::edit::TranscribeServices>(services);
    const auto* audio = command::findService<wabcut::audio::edit::AudioServices>(services);
    const auto* multicamServices = command::findService<command::MulticamServices>(services);
    const std::vector<MediaId> mics = micsOf(options);
    const auto add = [&](StepKind kind, std::string title, std::string detail, std::string command, bool heavy) -> StepPlan& {
        StepPlan s;
        s.kind = kind;
        s.title = std::move(title);
        s.detail = std::move(detail);
        s.command = std::move(command);
        s.heavy = heavy;
        if (options.skip.contains(kind)) {
            s.enabled = false;
            s.reason = "unticked";
        }
        plan.steps.push_back(std::move(s));
        return plan.steps.back();
    };
    const auto off = [](StepPlan& s, std::string reason) {
        if (s.enabled) {
            s.enabled = false;
            s.reason = std::move(reason);
        }
    };
    // 1. The multicam source.
    {
        StepPlan& s = add(StepKind::Multicam, "Sync the cameras and microphones",
                          std::to_string(options.cameras.size()) + " camera(s), " + std::to_string(options.mics.size()) + " microphone(s), synced by " +
                              options.sync,
                          "multicam.create", true);
        if (context.multicam.isValid()) {
            off(s, "already made");
        } else if (options.cameras.empty()) {
            off(s, "no camera");
        } else if (options.sync == "audio" && !multicamServices) {
            off(s, "no audio sync here");
            plan.warnings.push_back("this host cannot sync by audio: choose timecode sync or run the workflow in the app");
        }
    }
    // 2. The sequence.
    {
        StepPlan& s = add(StepKind::Sequence, "Lay out the episode",
                          "the multicam picture on V1 and each microphone on its own dialogue track", "", false);
        if (context.sequence.isValid()) {
            off(s, "already made");
        }
    }
    // 3. Transcripts.
    bool words = false; // the later steps will have transcripts: some already, or this step makes them
    {
        StepPlan& s = add(StepKind::Transcribe, "Transcribe the microphones", "on this computer, for clean-up, switching and captions",
                          "transcript.transcribe", true);
        const bool all = std::ranges::all_of(mics, [&](MediaId m) { return hasTranscript(state, services, m); });
        const bool any = std::ranges::any_of(mics, [&](MediaId m) { return hasTranscript(state, services, m); });
        if (all && !mics.empty()) {
            off(s, "already transcribed");
        } else if (!transcribe) {
            off(s, "no transcription here");
        }
        // Continuing from a context without the recordings: the episode's microphones are not known here, so the steps
        // that read transcripts stay on (and say so themselves when there are none).
        words = any || s.enabled || (mics.empty() && context.sequence.isValid());
    }
    // 4. Enhance Speech.
    {
        StepPlan& s = add(StepKind::Enhance, "Enhance Speech on the microphones",
                          "noise and room removed (strength " + std::to_string(static_cast<int>(options.enhanceStrength)) + ")",
                          "audio.enhanceSpeech", true);
        if (!audio || !audio->enhancer) {
            off(s, "no Enhance Speech here");
        }
    }
    // 5. Loudness.
    {
        StepPlan& s = add(StepKind::Loudness, "Loudness to " + std::format("{:.0f}", options.lufs) + " LUFS", "the mix, for podcast apps",
                          "audio.normalize", true);
        if (!audio || !audio->loudness) {
            off(s, "no loudness measurement here");
        }
    }
    // Clean-up, captions and chapters read the transcripts (switching works on the microphones' levels without them).
    const auto needsWords = [&](StepPlan& s) {
        if (!words) {
            off(s, "no transcripts (transcribe the microphones)");
        }
    };
    needsWords(add(StepKind::Cleanup, "Remove fillers and long pauses",
                   std::string(options.fillers ? "fillers" : "") + (options.fillers && options.silences > 0 ? " and " : "") +
                       (options.silences > 0 ? "pauses over " + std::format("{:.1f}", options.silences) + " s" : std::string()),
                   "transcript.cleanup", false));
    add(StepKind::AutoSwitch, "Switch cameras to whoever speaks", "the " + options.switchPreset + " preset", "multicam.autoSwitch", true);
    needsWords(add(StepKind::Captions, "Captions", "the " + options.captionPreset + " style", "captions.generateFromTranscript", false));
    needsWords(add(StepKind::Chapters, "Chapters from the topics", "chapter markers and YouTube chapters", "marker.add", false));
    {
        StepPlan& s = add(StepKind::Shorts, "Shorts", "the best " + std::to_string(options.shortsCount) + " moments as vertical shorts", "shorts.create",
                          true);
        if (!options.shorts) {
            off(s, "off (tick it for shorts)");
        }
    }
    return plan;
}

std::vector<Chapter> chaptersFromTopics(const edit::SequenceTranscript& transcript, double minSeconds, std::size_t max) {
    std::vector<Chapter> out;
    const std::vector<shorts::Sentence> sentences = shorts::splitSentences(transcript);
    if (sentences.empty()) {
        return out;
    }
    std::vector<std::string> texts;
    for (const shorts::Sentence& s : sentences) {
        texts.push_back(s.text);
    }
    const shorts::TextVectors vectors(texts);
    std::vector<std::vector<std::pair<std::uint32_t, float>>> vs;
    for (const std::string& t : texts) {
        vs.push_back(vectors.vectorOf(t));
    }
    const shorts::Topics topics = shorts::segmentTopics(vs);
    // Candidates: topic starts, strongest change (lowest cohesion) first.
    std::vector<std::size_t> candidates;
    for (std::size_t i = 1; i < sentences.size(); ++i) {
        if (i < topics.starts.size() && topics.starts[i]) {
            candidates.push_back(i);
        }
    }
    std::sort(candidates.begin(), candidates.end(), [&](std::size_t a, std::size_t b) { return topics.cohesion[a] < topics.cohesion[b]; });
    const Ticks gap{static_cast<std::int64_t>(minSeconds * static_cast<double>(kTicksPerSecond))};
    const Ticks end = sentences.back().range.end();
    std::vector<std::size_t> chosen;
    for (const std::size_t i : candidates) {
        if (chosen.size() + 1 >= max) {
            break;
        }
        const Ticks t = sentences[i].range.start;
        if (t < gap || end - t < gap) {
            continue;
        }
        if (std::ranges::all_of(chosen, [&](std::size_t c) { return std::max(t, sentences[c].range.start) - std::min(t, sentences[c].range.start) >= gap; })) {
            chosen.push_back(i);
        }
    }
    std::sort(chosen.begin(), chosen.end());
    out.push_back(Chapter{Ticks{}, shorts::titleFrom(sentences.front().text, 60)});
    for (const std::size_t i : chosen) {
        out.push_back(Chapter{sentences[i].range.start, shorts::titleFrom(sentences[i].text, 60)});
    }
    return out;
}

std::expected<StepResult, command::CommandError> runStep(StepKind kind, const ProjectState& state, PodcastContext& context,
                                                        const PodcastOptions& options, std::shared_ptr<command::CommandServices> services,
                                                        command::ExecutionControl* control) {
    EditSession session(state);
    StepResult result;
    const std::vector<MediaId> mics = micsOf(options);
    const auto fromResult = [&](command::CommandResult r) -> std::expected<void, command::CommandError> {
        if (!r.ok()) {
            return std::unexpected(*r.error);
        }
        result.report = std::move(r.data);
        result.warnings.insert(result.warnings.end(), r.warnings.begin(), r.warnings.end());
        return {};
    };
    const auto ids = [](const std::vector<MediaId>& media) {
        Json list = Json::array();
        for (const MediaId m : media) {
            list.push_back(m.value);
        }
        return list;
    };
    const auto needSequence = [&]() -> std::expected<void, command::CommandError> {
        if (!context.sequence.isValid() || !state.findSequence(context.sequence)) {
            return std::unexpected(error(command::ErrorCode::InvalidRequest, "lay out the episode first (the sequence step)"));
        }
        return {};
    };
    switch (kind) {
    case StepKind::Multicam: {
        Json names = Json::array();
        for (std::size_t i = 0; i < options.cameras.size(); ++i) {
            names.push_back("Cam " + std::string(1, static_cast<char>('A' + std::min<std::size_t>(i, 25))));
        }
        for (std::size_t i = 0; i < options.mics.size(); ++i) {
            names.push_back("Mic " + std::to_string(i + 1));
        }
        std::vector<MediaId> all = options.cameras;
        all.insert(all.end(), options.mics.begin(), options.mics.end());
        auto r = run(session, services, control, "multicam.create",
                     Json{{"media", ids(all)}, {"sync", options.sync}, {"name", options.name + " (multicam)"}, {"angleNames", std::move(names)}});
        if (auto ok = fromResult(r); !ok) {
            return std::unexpected(ok.error());
        }
        context.multicam = SequenceId{result.report.value("multicam", std::uint64_t{0})};
        break;
    }
    case StepKind::Sequence: {
        if (!context.multicam.isValid()) {
            return std::unexpected(error(command::ErrorCode::InvalidRequest, "sync the recordings first (the multicam step)"));
        }
        auto built = buildSequence(state, context.multicam, options);
        if (!built) {
            return std::unexpected(error(command::ErrorCode::EditRejected, built.error().message));
        }
        context.sequence = built->second;
        result.state = std::move(built->first);
        result.report = Json{{"sequence", context.sequence.value}, {"name", options.name},
                             {"micClips", micClips(result.state, context.sequence, mics).size()}};
        return result;
    }
    case StepKind::Transcribe: {
        std::vector<MediaId> todo;
        for (const MediaId m : mics) {
            if (!hasTranscript(state, services.get(), m)) {
                todo.push_back(m);
            }
        }
        if (todo.empty()) {
            result.state = state;
            result.report = Json{{"transcribed", 0}, {"note", "every microphone already has a transcript"}};
            return result;
        }
        auto r = run(session, services, control, "transcript.transcribe", Json{{"media", ids(todo)}, {"provider", "local"}});
        if (auto ok = fromResult(r); !ok) {
            return std::unexpected(ok.error());
        }
        context.transcribed.insert(context.transcribed.end(), todo.begin(), todo.end());
        break;
    }
    case StepKind::Enhance: {
        if (auto ok = needSequence(); !ok) {
            return std::unexpected(ok.error());
        }
        Json clips = Json::array();
        for (const ClipId c : micClips(state, context.sequence, mics)) {
            clips.push_back(c.value);
        }
        if (clips.empty()) {
            return std::unexpected(error(command::ErrorCode::InvalidRequest, "no microphone clips in the episode"));
        }
        auto r = run(session, services, control, "audio.enhanceSpeech",
                     Json{{"sequence", context.sequence.value}, {"clips", std::move(clips)}, {"strength", options.enhanceStrength}});
        if (auto ok = fromResult(r); !ok) {
            return std::unexpected(ok.error());
        }
        break;
    }
    case StepKind::Loudness: {
        if (auto ok = needSequence(); !ok) {
            return std::unexpected(ok.error());
        }
        auto r = run(session, services, control, "audio.normalize", Json{{"sequence", context.sequence.value}, {"lufs", options.lufs}});
        if (auto ok = fromResult(r); !ok) {
            return std::unexpected(ok.error());
        }
        break;
    }
    case StepKind::Cleanup: {
        if (auto ok = needSequence(); !ok) {
            return std::unexpected(ok.error());
        }
        Json params{{"sequence", context.sequence.value}, {"fillers", options.fillers}};
        params["silences"] = options.silences > 0 ? options.silences : 0.0;
        auto r = run(session, services, control, "transcript.cleanup", std::move(params));
        if (auto ok = fromResult(r); !ok) {
            return std::unexpected(ok.error());
        }
        break;
    }
    case StepKind::AutoSwitch: {
        if (auto ok = needSequence(); !ok) {
            return std::unexpected(ok.error());
        }
        const auto pictures = multicamPictures(state, context.sequence, context.multicam);
        if (pictures.empty()) {
            return std::unexpected(error(command::ErrorCode::InvalidRequest, "the episode has no multicam picture"));
        }
        Json params{{"sequence", context.sequence.value}, {"clip", pictures.front()->id.value}, {"preset", options.switchPreset}};
        if (options.angles.is_array() && !options.angles.empty()) {
            params["angles"] = options.angles;
        }
        auto r = run(session, services, control, "multicam.autoSwitch", std::move(params));
        if (auto ok = fromResult(r); !ok) {
            return std::unexpected(ok.error());
        }
        break;
    }
    case StepKind::Captions: {
        if (auto ok = needSequence(); !ok) {
            return std::unexpected(ok.error());
        }
        auto r = run(session, services, control, "captions.generateFromTranscript",
                     Json{{"sequence", context.sequence.value}, {"preset", options.captionPreset}});
        if (auto ok = fromResult(r); !ok) {
            return std::unexpected(ok.error());
        }
        break;
    }
    case StepKind::Chapters: {
        if (auto ok = needSequence(); !ok) {
            return std::unexpected(ok.error());
        }
        auto* transcripts = command::findService<ai::edit::TranscriptServices>(services.get());
        if (!transcripts || !transcripts->transcripts) {
            return std::unexpected(error(command::ErrorCode::InvalidRequest, "chapters need the transcripts"));
        }
        const edit::SequenceTranscript st = edit::SequenceTranscript::build(state, context.sequence, *transcripts->transcripts);
        const std::vector<Chapter> chapters = chaptersFromTopics(st, options.minChapterSeconds, options.maxChapters);
        if (chapters.size() < 2) {
            result.state = state;
            result.report = Json{{"chapters", Json::array()}, {"note", "too short or one topic: no chapters"}};
            return result;
        }
        Json list = Json::array();
        for (const Chapter& ch : chapters) {
            auto r = run(session, services, control, "marker.add",
                         Json{{"sequence", context.sequence.value}, {"time", ch.start.value}, {"name", ch.title}, {"type", "chapter"}});
            if (!r.ok()) {
                return std::unexpected(*r.error);
            }
            list.push_back(Json{{"seconds", std::round(seconds(ch.start) * 1000.0) / 1000.0}, {"title", ch.title}});
        }
        auto exported = run(session, services, control, "sequence.exportMarkers", Json{{"sequence", context.sequence.value}, {"format", "youtube"}});
        if (exported.ok()) {
            context.youtubeChapters = exported.data.value("text", std::string());
            result.warnings.insert(result.warnings.end(), exported.warnings.begin(), exported.warnings.end());
        }
        result.report = Json{{"chapters", std::move(list)}, {"youtube", context.youtubeChapters}};
        break;
    }
    case StepKind::Shorts: {
        if (auto ok = needSequence(); !ok) {
            return std::unexpected(ok.error());
        }
        auto found = run(session, services, control, "shorts.find", Json{{"sequence", context.sequence.value}, {"count", options.shortsCount}});
        if (!found.ok()) {
            return std::unexpected(*found.error);
        }
        Json picks = Json::array();
        for (const Json& s : found.data.value("shorts", Json::array())) {
            if (picks.size() >= options.shortsCount) {
                break;
            }
            picks.push_back(Json{{"start", s.value("start", 0.0)}, {"end", s.value("end", 0.0)}, {"title", s.value("title", std::string())}});
        }
        if (picks.empty()) {
            result.state = state;
            result.report = Json{{"shorts", 0}, {"note", "no short-worthy moment found"}};
            return result;
        }
        auto r = run(session, services, control, "shorts.create", Json{{"sequence", context.sequence.value}, {"shorts", std::move(picks)}});
        if (auto ok = fromResult(r); !ok) {
            return std::unexpected(ok.error());
        }
        break;
    }
    }
    result.state = *session.snapshot();
    return result;
}

// ---- Journal -----------------------------------------------------------------------------------------------------

std::optional<StepKind> Journal::next(const PodcastPlan& plan) const {
    for (const StepPlan& s : plan.steps) {
        if (!s.enabled) {
            continue;
        }
        const bool settled = std::ranges::any_of(steps, [&](const auto& st) { return st.first == s.kind && (st.second == "done" || st.second == "skipped"); });
        if (!settled) {
            return s.kind;
        }
    }
    return std::nullopt;
}

Json podcastOptionsToJson(const PodcastOptions& o) {
    Json cams = Json::array(), mics = Json::array(), skip = Json::array();
    for (const MediaId m : o.cameras) {
        cams.push_back(m.value);
    }
    for (const MediaId m : o.mics) {
        mics.push_back(m.value);
    }
    for (const StepKind k : o.skip) {
        skip.push_back(std::string(toString(k)));
    }
    return Json{{"cameras", std::move(cams)}, {"mics", std::move(mics)}, {"name", o.name}, {"sync", o.sync},
                {"enhanceStrength", o.enhanceStrength}, {"lufs", o.lufs}, {"fillers", o.fillers}, {"silences", o.silences},
                {"switchPreset", o.switchPreset}, {"captionPreset", o.captionPreset}, {"maxChapters", o.maxChapters},
                {"minChapterSeconds", o.minChapterSeconds}, {"shorts", o.shorts}, {"shortsCount", o.shortsCount}, {"skip", std::move(skip)},
                {"angles", o.angles.is_array() ? o.angles : Json::array()}};
}

PodcastOptions podcastOptionsFromJson(const Json& j) {
    PodcastOptions o;
    if (!j.is_object()) {
        return o;
    }
    const auto media = [&](const char* key, std::vector<MediaId>& out) {
        if (const auto it = j.find(key); it != j.end() && it->is_array()) {
            for (const Json& m : *it) {
                if (m.is_number_integer() || m.is_number_unsigned()) {
                    out.push_back(MediaId{m.get<std::uint64_t>()});
                }
            }
        }
    };
    media("cameras", o.cameras);
    media("mics", o.mics);
    o.name = j.value("name", o.name);
    o.sync = j.value("sync", o.sync);
    o.enhanceStrength = j.value("enhanceStrength", o.enhanceStrength);
    o.lufs = j.value("lufs", o.lufs);
    o.fillers = j.value("fillers", o.fillers);
    o.silences = j.value("silences", o.silences);
    o.switchPreset = j.value("switchPreset", o.switchPreset);
    o.captionPreset = j.value("captionPreset", o.captionPreset);
    o.maxChapters = j.value("maxChapters", o.maxChapters);
    o.minChapterSeconds = j.value("minChapterSeconds", o.minChapterSeconds);
    o.shorts = j.value("shorts", o.shorts);
    o.shortsCount = j.value("shortsCount", o.shortsCount);
    if (const auto it = j.find("angles"); it != j.end() && it->is_array()) {
        o.angles = *it;
    }
    if (const auto it = j.find("skip"); it != j.end() && it->is_array()) {
        for (const Json& s : *it) {
            if (s.is_string()) {
                if (const auto k = stepFromString(s.get<std::string>())) {
                    o.skip.insert(*k);
                }
            }
        }
    }
    return o;
}

Json journalToJson(const Journal& journal) {
    Json steps = Json::array();
    for (const auto& [kind, status] : journal.steps) {
        steps.push_back(Json{{"step", std::string(toString(kind))}, {"status", status}});
    }
    return Json{{"format", "wabcut.podcastWorkflow"}, {"version", 1}, {"options", journal.options}, {"context", contextToJson(journal.context)},
                {"steps", std::move(steps)}, {"revisions", journal.revisions}, {"updatedAt", journal.updatedAt.empty() ? nowIso() : journal.updatedAt}};
}

std::optional<Journal> journalFromJson(const Json& json) {
    if (!json.is_object() || json.value("format", std::string()) != "wabcut.podcastWorkflow") {
        return std::nullopt;
    }
    Journal j;
    j.options = json.value("options", Json::object());
    j.context = contextFromJson(json.value("context", Json::object()));
    for (const Json& s : json.value("steps", Json::array())) {
        if (const auto k = stepFromString(s.value("step", std::string()))) {
            j.steps.emplace_back(*k, s.value("status", std::string()));
        }
    }
    for (const Json& r : json.value("revisions", Json::array())) {
        if (r.is_string()) {
            j.revisions.push_back(r.get<std::string>());
        }
    }
    j.updatedAt = json.value("updatedAt", std::string());
    return j;
}

std::expected<void, Error> saveJournal(const std::filesystem::path& file, const Journal& journal) {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    // Appended to the path, not to its narrow string(), which throws on Windows for names outside the code page.
    std::filesystem::path part = file;
    part += ".part";
    {
        std::ofstream out(part, std::ios::binary | std::ios::trunc);
        out << journalToJson(journal).dump(2);
        out.flush();
        if (!out) {
            return std::unexpected(Error{ErrorCode::IoError, "cannot write the workflow journal"});
        }
    }
    std::filesystem::rename(part, file, ec);
    if (ec) {
        return std::unexpected(Error{ErrorCode::IoError, "cannot save the workflow journal: " + ec.message()});
    }
    return {};
}

std::expected<Journal, Error> loadJournal(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return std::unexpected(Error{ErrorCode::NotFound, "no workflow journal"});
    }
    std::stringstream text;
    text << in.rdbuf();
    const Json json = Json::parse(text.str(), nullptr, false);
    auto j = journalFromJson(json);
    if (!j) {
        return std::unexpected(Error{ErrorCode::ParseError, "the workflow journal is not readable"});
    }
    return *j;
}

std::filesystem::path journalFileFor(const std::filesystem::path& projectFile) {
    std::filesystem::path folder = projectFile;
    folder += ".history";
    return folder / "podcast-workflow.json";
}

} // namespace wabcut::ai::workflow
