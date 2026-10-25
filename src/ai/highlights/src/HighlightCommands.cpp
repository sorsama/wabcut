#include <wabcut/ai/highlights/HighlightCommands.h>

#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/ai/reframe/Reframe.h>
#include <wabcut/ai/shorts/ShortsCommands.h>
#include <wabcut/command/Engine.h>
#include <wabcut/command/Schema.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/Resolve.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <limits>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <string_view>

namespace wabcut::ai::highlights {

using command::CommandOutput;
using command::ErrorCode;
using command::ExecutionContext;
using command::fail;
using command::Json;
namespace schema = command::schema;

std::expected<Measurement, Error> MapHighlightProvider::measure(const MediaItem& media, bool video, const analysis::AnalysisProgress& progress) {
    (void)progress;
    const auto it = m_items.find(media.id.value);
    if (it == m_items.end()) {
        return std::unexpected(Error(wabcut::ErrorCode::NotFound, "no highlight measurements for '" + media.name + "'"));
    }
    Measurement m = it->second;
    if (!video) {
        m.video.reset();
    }
    return m;
}

namespace {

constexpr std::size_t kMaxRecordings = 64;
constexpr std::uintmax_t kMaxChatFileBytes = 512ull << 20;

double rounded(double v, double scale) { return std::round(v * scale) / scale; }
Ticks fromSeconds(double s) { return Ticks{static_cast<std::int64_t>(std::llround(s * static_cast<double>(kTicksPerSecond)))}; }

Json withSequence(std::initializer_list<std::pair<const char*, Json>> properties, std::vector<std::string> required = {}) {
    Json s = schema::object(properties, std::move(required));
    s["properties"]["sequence"] = schema::integer("Sequence id; only needed when the project has several.", 1);
    return s;
}

Json mediaParam(std::string description) {
    return Json{{"type", Json::array({"integer", "string"})}, {"description", description + " A media id or exact name."}, {"minimum", 1}};
}

HighlightProvider& providerOf(ExecutionContext& c) {
    auto* services = command::findService<HighlightServices>(c.services());
    if (!services || !services->provider) {
        fail(ErrorCode::InvalidRequest, "highlight analysis is not available here: the host provides none");
    }
    return *services->provider;
}

/// Floor / ceil to the frame grid.
Ticks frameFloor(Ticks t, Ticks frame) { return Ticks{floorDiv(t.value, frame.value) * frame.value}; }
Ticks frameCeil(Ticks t, Ticks frame) { return Ticks{(floorDiv(t.value - 1, frame.value) + 1) * frame.value}; }

struct Wanted {
    bool game = true;
    bool mic = true;
    bool visual = true;
    bool chat = true;
    bool markers = true;
};

Wanted wantedSignals(ExecutionContext& c) {
    Wanted w;
    if (!c.has("signals")) {
        return w;
    }
    w = Wanted{false, false, false, false, false};
    for (const Json& s : c.params()["signals"]) {
        const std::string name = s.get<std::string>();
        w.game = w.game || name == "game";
        w.mic = w.mic || name == "mic";
        w.visual = w.visual || name == "visual";
        w.chat = w.chat || name == "chat";
        w.markers = w.markers || name == "markers";
    }
    return w;
}

FindOptions findOptions(ExecutionContext& c) {
    FindOptions o;
    o.maxMoments = static_cast<std::size_t>(c.integer("count", 10));
    if (c.has("minScore")) {
        o.minScore = c.number("minScore");
    }
    if (c.has("minGap")) {
        o.minGapSeconds = c.number("minGap");
    }
    return o;
}

/// The audio roles asked for ("audioRoles": by MediaInfo::audio index; "ignore" leaves a stream out), else the
/// classified ones.
std::vector<std::optional<AudioRole>> rolesFor(ExecutionContext& c, const Measurement& m) {
    std::vector<const AudioFeatures*> streams;
    for (const AudioFeatures& f : m.audio) {
        streams.push_back(&f);
    }
    const std::vector<AudioRole> classified = classifyStreams(streams);
    std::vector<std::optional<AudioRole>> roles(classified.begin(), classified.end());
    if (c.has("audioRoles")) {
        const Json& list = c.params()["audioRoles"];
        for (std::size_t i = 0; i < roles.size() && i < list.size(); ++i) {
            const std::string name = list[i].get<std::string>();
            roles[i] = name == "ignore" ? std::nullopt : audioRoleFromString(name);
        }
    }
    return roles;
}

std::string readChatFile(const std::string& utf8) {
    const std::filesystem::path path = pathFromUtf8(utf8);
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        fail(ErrorCode::InvalidParams, "cannot read the chat log: " + ec.message());
    }
    if (size > kMaxChatFileBytes) {
        fail(ErrorCode::InvalidParams, "the chat log is larger than 512 MB");
    }
    std::ifstream in(path, std::ios::binary);
    std::string text(static_cast<std::size_t>(size), '\0');
    if (!in || !in.read(text.data(), static_cast<std::streamsize>(size))) {
        fail(ErrorCode::InvalidParams, "cannot read the chat log");
    }
    return text;
}

std::string reasonsText(const Moment& m) {
    std::string text;
    for (const std::string& r : m.reasons) {
        text += (text.empty() ? "" : "; ") + r;
    }
    return text;
}

Marker markerFor(const Moment& m, std::size_t rank, Ticks frame) {
    Marker marker;
    marker.start = frameFloor(m.range.start, frame);
    marker.duration = std::max(frame, frameCeil(m.range.end(), frame) - marker.start);
    marker.name = std::string(kMarkerPrefix) + std::to_string(rank) + ": " + m.title;
    char head[64];
    std::snprintf(head, sizeof head, "%s%.2f, confidence %s", std::string(kCommentPrefix).c_str(), m.score,
                  std::string(confidenceLabel(m.confidence)).c_str());
    marker.comment = std::string(head) + (m.reasons.empty() ? "" : ": " + reasonsText(m));
    if (marker.name.size() > kMaxMarkerTextBytes) {
        marker.name.resize(kMaxMarkerTextBytes);
    }
    if (marker.comment.size() > kMaxMarkerTextBytes) {
        marker.comment.resize(kMaxMarkerTextBytes);
    }
    marker.colorIndex = kMarkerColor;
    // C.15: what the Highlights panel and the reel read (the name and comment are for people).
    MarkerHighlight h;
    h.score = std::clamp(m.score, 0.0, 1.0);
    h.confidence = m.confidence >= 0.75 ? HighlightConfidence::High : m.confidence >= 0.5 ? HighlightConfidence::Medium : HighlightConfidence::Low;
    for (const std::string& r : m.reasons) {
        if (h.reasons.size() == MarkerHighlight::kMaxReasons) {
            break;
        }
        std::string text = r.substr(0, MarkerHighlight::kMaxReasonBytes);
        while (text.size() < r.size() && !text.empty() && (static_cast<unsigned char>(r[text.size()]) & 0xC0) == 0x80) {
            text.pop_back(); // never cut inside a UTF-8 character
        }
        if (!text.empty()) {
            h.reasons.push_back(std::move(text));
        }
    }
    h.peak = std::clamp(m.peak, marker.start, marker.end());
    h.source = std::string(kHighlightSource);
    marker.highlight = std::move(h);
    return marker;
}

/// A marker highlights.find made: with C.15's highlight metadata from this finder, or (projects from before C.15)
/// in the "Highlight <n>: ..." / "score ..." form.
bool isHighlightMarker(const Marker& m) {
    if (m.highlight) {
        return m.highlight->source == kHighlightSource;
    }
    if (!m.name.starts_with(kMarkerPrefix) || !m.comment.starts_with(kCommentPrefix)) {
        return false;
    }
    const std::string_view rest = std::string_view(m.name).substr(kMarkerPrefix.size());
    std::size_t digits = 0;
    while (digits < rest.size() && std::isdigit(static_cast<unsigned char>(rest[digits]))) {
        ++digits;
    }
    return digits > 0 && rest.substr(digits).starts_with(":");
}

std::vector<Marker> replaceHighlightMarkers(const std::vector<Marker>& existing, const std::vector<Moment>& moments, Ticks frame) {
    std::vector<Marker> markers;
    for (const Marker& m : existing) {
        if (!isHighlightMarker(m)) {
            markers.push_back(m);
        }
    }
    for (std::size_t i = 0; i < moments.size(); ++i) {
        markers.push_back(markerFor(moments[i], i + 1, frame));
    }
    std::stable_sort(markers.begin(), markers.end(), [](const Marker& a, const Marker& b) { return a.start < b.start; });
    if (markers.size() > kMaxMarkers) {
        markers.resize(kMaxMarkers);
    }
    return markers;
}

/// Every recording the sequence's enabled clips play (not generated or still media; 0.2: a nested or multicam clip
/// the recordings it plays, its active angles), in first-seen order.
std::vector<const MediaItem*> recordingsOf(const ProjectState& state, const Sequence& seq) {
    std::vector<const MediaItem*> recordings;
    std::set<std::uint64_t> seen;
    for (const std::vector<TrackPtr>* tracks : {&seq.videoTracks, &seq.audioTracks}) {
        for (const TrackPtr& t : *tracks) {
            for (const Clip& clip : t->clips) {
                for (const MediaId media : mediaPlayedBy(state, clip)) {
                    if (!seen.insert(media.value).second) {
                        continue;
                    }
                    if (const MediaItem* item = state.findMedia(media); item && !item->isGenerator() && !item->info.isStillImage) {
                        recordings.push_back(item);
                    }
                }
            }
        }
    }
    return recordings;
}

// --- highlights.find -------------------------------------------------------------------------------------------------

CommandOutput findCommand(ExecutionContext& c) {
    HighlightProvider& provider = providerOf(c);
    const Wanted wanted = wantedSignals(c);
    const FindOptions options = findOptions(c);
    const ProjectState& state = c.state();
    std::vector<std::string> warnings;

    // What to measure: one media item, or every recording the sequence plays.
    const bool mediaMode = c.has("media");
    std::vector<const MediaItem*> recordings;
    const Sequence* seq = nullptr;
    if (mediaMode) {
        const MediaItem* item = state.findMedia(c.media("media"));
        if (!item) {
            fail(ErrorCode::NotFound, "no such media item");
        }
        recordings.push_back(item);
    } else {
        seq = &c.sequence();
        recordings = recordingsOf(state, *seq);
        if (recordings.empty()) {
            fail(ErrorCode::EditRejected, "sequence '" + seq->name + "' plays no recordings to analyse");
        }
        if (recordings.size() > kMaxRecordings) {
            fail(ErrorCode::InvalidParams, "the sequence plays " + std::to_string(recordings.size()) + " recordings: analyse at most " +
                                               std::to_string(kMaxRecordings) + " at a time (a shorter sequence, or one media item)");
        }
    }

    // Chat logs imported for the recordings (highlights.importChat).
    ChatOptions chatOptions;
    std::size_t chatMessages = 0;
    std::string chatFormat;
    if (c.has("chatDelay")) {
        chatOptions.reactionSeconds = c.number("chatDelay");
    }
    if (c.has("keywords")) {
        for (const Json& k : c.params()["keywords"]) {
            chatOptions.keywords.push_back(k.get<std::string>());
        }
    }

    std::map<std::uint64_t, MediaSignals> signalsByMedia;
    Json analysed = Json::array();
    std::optional<Error> firstError;
    for (std::size_t r = 0; r < recordings.size(); ++r) {
        const MediaItem& item = *recordings[r];
        const std::string what = "measuring '" + item.name + "'" +
                                 (recordings.size() > 1 ? " (" + std::to_string(r + 1) + " of " + std::to_string(recordings.size()) + ")" : "");
        c.checkpoint(what);
        int lastPercent = -1;
        auto measured = provider.measure(item, wanted.visual, [&](double f) {
            const int percent = static_cast<int>(std::lround(f * 100.0));
            if (percent != lastPercent) {
                lastPercent = percent;
                c.checkpoint(what + " " + std::to_string(percent) + "%");
            }
            return true;
        });
        if (!measured) {
            if (measured.error().code == wabcut::ErrorCode::Cancelled) {
                fail(ErrorCode::Cancelled, measured.error().message);
            }
            warnings.push_back("'" + item.name + "': " + measured.error().message);
            if (!firstError) {
                firstError = measured.error();
            }
            continue;
        }
        MediaSignals& s = signalsByMedia[item.id.value];
        const std::vector<std::optional<AudioRole>> roles = rolesFor(c, *measured);
        Json roleNames = Json::array();
        for (std::size_t i = 0; i < measured->audio.size(); ++i) {
            const std::optional<AudioRole> role = roles[i];
            roleNames.push_back(role ? std::string(toString(*role)) : std::string("ignore"));
            if (!role) {
                continue;
            }
            std::optional<AudioRole> use;
            if (*role == AudioRole::Mixed) {
                use = wanted.game && wanted.mic ? std::optional(AudioRole::Mixed)
                      : wanted.game             ? std::optional(AudioRole::Game)
                      : wanted.mic              ? std::optional(AudioRole::Mic)
                                                : std::nullopt;
            } else if ((*role == AudioRole::Game && wanted.game) || (*role == AudioRole::Mic && wanted.mic)) {
                use = role;
            }
            if (use) {
                addAudio(s, measured->audio[i], *use);
            }
        }
        if (wanted.visual && measured->video) {
            addVideo(s, *measured->video);
        }
        if (wanted.markers && !measured->hints.empty()) {
            addHints(s, measured->hints);
        }
        bool usedChat = false;
        if (wanted.chat) {
            if (auto kept = provider.chatFor(item)) {
                ChatOptions chatHere = chatOptions;
                chatHere.offsetSeconds = c.has("chatOffset") ? c.number("chatOffset") : kept->offsetSeconds;
                addChat(s, kept->chat, chatHere);
                usedChat = true;
                chatMessages += kept->chat.messages.size();
                chatFormat = kept->chat.format;
            }
        }
        for (const std::string& note : measured->notes) {
            warnings.push_back("'" + item.name + "': " + note);
        }
        // The recording's own moments, kept with its analyses (media time).
        const Ticks mediaEnd = std::max(item.info.duration, featureTime(s.length));
        provider.remember(item, findMoments(s, mediaEnd, options));
        analysed.push_back(Json{{"media", item.id.value},
                                {"name", item.name},
                                {"audioRoles", std::move(roleNames)},
                                {"video", measured->video.has_value()},
                                {"markers", measured->hints.size()},
                                {"chat", usedChat},
                                {"fromCache", measured->fromCache}});
    }
    if (signalsByMedia.empty()) {
        fail(ErrorCode::EditRejected, "nothing could be analysed: " + (firstError ? firstError->message : std::string("no recordings")));
    }

    std::vector<Moment> moments;
    ProjectState next = state;
    std::size_t markersAdded = 0;
    const bool addMarkers = c.boolean("markers", true);
    if (mediaMode) {
        const MediaItem& item = *recordings.front();
        const MediaSignals& s = signalsByMedia.begin()->second;
        moments = findMoments(s, std::max(item.info.duration, featureTime(s.length)), options);
        if (addMarkers) {
            next = c.take(wabcut::edit::setMediaMarkers(next, item.id, replaceHighlightMarkers(item.markers, moments, Ticks{kTicksPerSecond / 1000})))
                       .state;
            markersAdded = moments.size();
        }
    } else {
        SignalsByMedia byMedia;
        for (const auto& [id, s] : signalsByMedia) {
            byMedia[id] = &s;
        }
        const MediaSignals mapped = mapToSequence(state, seq->id, byMedia, &warnings);
        moments = findMoments(mapped, std::max(seq->duration, seq->contentEnd()), options);
        if (addMarkers) {
            const Ticks frame{seq->frameRate.ticksPerUnit()};
            next = c.take(wabcut::edit::setSequenceMarkers(next, seq->id, replaceHighlightMarkers(seq->markers, moments, frame))).state;
            markersAdded = moments.size();
        }
    }
    if (moments.empty()) {
        warnings.push_back("no moment stood out: try a lower \"minScore\" or more signals (a chat log, OBS markers)");
    }

    Json list = Json::array();
    for (std::size_t i = 0; i < moments.size(); ++i) {
        list.push_back(momentToJson(moments[i], i + 1));
    }
    CommandOutput out;
    out.state = std::move(next);
    out.data = Json{{"timeBase", mediaMode ? "media" : "timeline"},
                    {"moments", std::move(list)},
                    {"recordings", std::move(analysed)},
                    {"markersAdded", markersAdded}};
    if (mediaMode) {
        out.data["media"] = recordings.front()->id.value;
    } else {
        out.data["sequence"] = seq->id.value;
    }
    if (chatMessages > 0) {
        out.data["chat"] = Json{{"format", chatFormat}, {"messages", chatMessages}};
    }
    out.warnings = std::move(warnings);
    return out;
}

// --- highlights.importChat ------------------------------------------------------------------------------------------

CommandOutput importChatCommand(ExecutionContext& c) {
    HighlightProvider& provider = providerOf(c);
    auto parsed = parseChatLog(readChatFile(c.string("file")));
    if (!parsed) {
        fail(ErrorCode::InvalidParams, parsed.error().message);
    }
    const MediaItem* item = nullptr;
    if (c.has("media")) {
        item = c.state().findMedia(c.media("media"));
    } else {
        for (const MediaItem* r : recordingsOf(c.state(), c.sequence())) {
            if (!item || r->info.duration > item->info.duration) {
                item = r; // the longest recording: the stream itself
            }
        }
    }
    if (!item) {
        fail(ErrorCode::EditRejected, "there is no recording to keep the chat log with: name one with \"media\"");
    }
    const double offset = c.has("offset") ? c.number("offset") : 0.0;
    if (!c.dryRun()) {
        if (auto kept = provider.keepChat(*item, *parsed, offset); !kept) {
            fail(ErrorCode::EditRejected, "the chat log could not be kept: " + kept.error().message);
        }
    }
    const double first = parsed->messages.front().seconds + offset;
    const double last = parsed->messages.back().seconds + offset;
    std::vector<std::string> warnings;
    const double length = item->info.duration.toSeconds();
    if (length > 0.0 && (last < 0.0 || first > length + 60.0)) {
        warnings.push_back("the chat does not overlap '" + item->name + "': check \"offset\" (chat time + offset = recording time)");
    }
    CommandOutput out;
    out.state = c.state();
    out.data = Json{{"media", item->id.value},
                    {"name", item->name},
                    {"format", parsed->format},
                    {"messages", parsed->messages.size()},
                    {"first", rounded(first, 1000.0)},
                    {"last", rounded(last, 1000.0)},
                    {"offset", offset}};
    out.warnings = std::move(warnings);
    return out;
}

// --- highlights.toReel -----------------------------------------------------------------------------------------------

/// The moments to use: given ones (timeline seconds) or the sequence's highlight markers.
std::vector<Moment> reelMoments(ExecutionContext& c, const Sequence& seq) {
    std::vector<Moment> moments;
    if (c.has("moments")) {
        const Json& list = c.params()["moments"];
        for (std::size_t i = 0; i < list.size(); ++i) {
            const Json& item = list[i];
            const double start = item.at("start").get<double>();
            const double end = item.at("end").get<double>();
            if (!(end > start) || start < 0.0 || end - start > 600.0) {
                fail(ErrorCode::InvalidParams, "moment " + std::to_string(i + 1) + ": \"end\" must follow \"start\", at most 10 minutes later");
            }
            Moment m;
            m.range = TimeRange::fromStartEnd(fromSeconds(start), fromSeconds(end));
            m.peak = item.contains("peak") ? std::clamp(fromSeconds(item["peak"].get<double>()), m.range.start, m.range.end() - Ticks{1})
                                           : m.range.start + Ticks{m.range.duration.value * 2 / 3};
            m.score = item.contains("score") ? item["score"].get<double>() : 1.0 - static_cast<double>(i) * 0.01;
            m.title = item.contains("title") && item["title"].is_string() ? item["title"].get<std::string>() : std::string();
            moments.push_back(std::move(m));
        }
    } else {
        moments = markedMoments(seq);
        if (moments.empty()) {
            fail(ErrorCode::EditRejected, "sequence '" + seq.name + "' has no highlight markers: run highlights.find first, or pass \"moments\"");
        }
    }
    return moments;
}

struct Piece {
    Ticks from; ///< source sequence time
    Ticks to;
    Speed speed{1, 1};
};

/// Copies [piece.from, piece.to) of every source track to the same track of the reel at `at` (fresh ids; clips at
/// a speed other than 100 % get `piece.speed` only when they play at 100 % unremapped). Returns the timeline length.
Ticks copyPiece(const Sequence& source, Sequence& reel, const Piece& piece, Ticks at, IdGenerator& ids,
                std::map<std::uint64_t, LinkGroupId>& groups, std::vector<ClipId>& firstClips, std::vector<ClipId>& lastClips,
                bool& speedSkipped) {
    const bool slow = !piece.speed.isUnity();
    const Ticks length = slow ? toTimelineTicks(piece.to - piece.from, piece.speed) : piece.to - piece.from;
    const auto copyTracks = [&](const std::vector<TrackPtr>& from, std::vector<TrackPtr>& to) {
        for (std::size_t k = 0; k < from.size() && k < to.size(); ++k) {
            std::vector<Clip> clips = to[k]->clips.toVector();
            bool changed = false;
            for (Clip clip : from[k]->clips) {
                if (clip.end() <= piece.from || clip.start() >= piece.to) {
                    continue;
                }
                if (clip.start() < piece.from) {
                    clip.moveHead(piece.from - clip.start());
                }
                if (clip.end() > piece.to) {
                    clip.moveTail(piece.to - clip.end());
                }
                Ticks offset = clip.start() - piece.from;
                if (slow) {
                    if (clip.speed.isUnity() && !clip.isRemapped() && !clip.reversed) {
                        const Ticks sourceLength = clip.sourceDuration();
                        offset = toTimelineTicks(offset, piece.speed);
                        clip.speed = piece.speed;
                        clip.timelineRange.duration = toTimelineTicks(sourceLength, piece.speed);
                    } else {
                        speedSkipped = true;
                        offset = toTimelineTicks(offset, piece.speed);
                    }
                }
                clip.timelineRange.start = at + offset;
                clip.id = ids.allocate<ClipId>();
                if (clip.linkGroup.isValid()) {
                    auto [it, inserted] = groups.try_emplace(clip.linkGroup.value, LinkGroupId{});
                    if (inserted) {
                        it->second = ids.allocate<LinkGroupId>();
                    }
                    clip.linkGroup = it->second;
                }
                if (!clip.effects().empty()) {
                    EffectStack stack = clip.effects();
                    for (EffectInstance& e : stack) {
                        e.id = ids.allocate<EffectInstanceId>();
                    }
                    clip.setEffects(std::move(stack));
                }
                firstClips.push_back(clip.id);
                lastClips.push_back(clip.id);
                clips.push_back(std::move(clip));
                changed = true;
            }
            if (changed) {
                auto track = std::make_shared<Track>(*to[k]);
                std::stable_sort(clips.begin(), clips.end(), [](const Clip& a, const Clip& b) { return a.start() < b.start(); });
                // A clip that a slowed neighbour now overlaps is shortened (one track holds no overlaps).
                for (std::size_t i = 1; i < clips.size(); ++i) {
                    if (clips[i - 1].end() > clips[i].start()) {
                        clips[i - 1].moveTail(clips[i].start() - clips[i - 1].end());
                    }
                }
                std::erase_if(clips, [](const Clip& cl) { return cl.timelineRange.duration <= Ticks{}; });
                track->clips = ClipList(std::move(clips));
                to[k] = std::move(track);
            }
        }
    };
    copyTracks(source.videoTracks, reel.videoTracks);
    copyTracks(source.audioTracks, reel.audioTracks);
    return length;
}

CommandOutput toReelCommand(ExecutionContext& c) {
    const Sequence& source = c.sequence();
    std::vector<Moment> moments = reelMoments(c, source);
    const std::string order = c.string("order", "time");
    const auto count = static_cast<std::size_t>(c.integer("count", static_cast<std::int64_t>(kMaxReelMoments)));
    std::stable_sort(moments.begin(), moments.end(), [](const Moment& a, const Moment& b) { return a.score > b.score; });
    if (moments.size() > count) {
        moments.resize(count);
    }
    const double handleBefore = c.has("handleBefore") ? c.number("handleBefore") : 0.5;
    const double handleAfter = c.has("handleAfter") ? c.number("handleAfter") : 0.5;
    const Ticks sourceEnd = std::max(source.duration, source.contentEnd());
    const Ticks frame{source.frameRate.ticksPerUnit()};
    // Ranges with handles, inside the source and not overlapping the next one.
    for (Moment& m : moments) {
        const Ticks a = std::max(Ticks{}, m.range.start - fromSeconds(handleBefore));
        const Ticks b = std::min(sourceEnd, m.range.end() + fromSeconds(handleAfter));
        m.range = TimeRange::fromStartEnd(frameFloor(a, frame), std::max(frameCeil(b, frame), frameFloor(a, frame) + frame));
    }
    if (c.has("maxDuration")) {
        const Ticks budget = fromSeconds(c.number("maxDuration"));
        Ticks used;
        std::vector<Moment> kept;
        for (Moment& m : moments) { // best first
            if (used + m.range.duration <= budget || kept.empty()) {
                used = used + m.range.duration;
                kept.push_back(std::move(m));
            }
        }
        moments = std::move(kept);
    }
    if (order == "time") {
        std::stable_sort(moments.begin(), moments.end(), [](const Moment& a, const Moment& b) { return a.range.start < b.range.start; });
    }

    const bool slowMo = c.boolean("slowMotion", false);
    const double slowSpeed = c.has("slowMotionSpeed") ? c.number("slowMotionSpeed") : 0.5;
    const double slowSeconds = c.has("slowMotionSeconds") ? c.number("slowMotionSeconds") : 1.5;
    const std::string transition = c.string("transition", "crossDissolve");
    const Ticks transitionLength = frameFloor(fromSeconds(c.has("transitionDuration") ? c.number("transitionDuration") : 0.5), frame);

    // Beats of the music, when asked to cut to them.
    std::vector<Ticks> beats;
    std::optional<analysis::BeatAnalysis> beatAnalysis;
    const MediaItem* music = nullptr;
    Ticks musicStart;
    const auto beatsPerCut = static_cast<std::size_t>(c.integer("beatsPerCut", 4));
    std::vector<std::string> warnings;
    if (c.has("music")) {
        music = c.state().findMedia(c.media("music"));
        if (!music || music->info.audio.empty()) {
            fail(ErrorCode::InvalidParams, "\"music\" must be a media item with audio");
        }
        musicStart = c.has("musicStart") ? fromSeconds(c.number("musicStart")) : Ticks{};
        auto* analysisServices = command::findService<analysis::AnalysisServices>(c.services());
        if (!analysisServices || !analysisServices->provider) {
            fail(ErrorCode::InvalidRequest, "beat detection is not available here: the host provides no media analysis");
        }
        c.checkpoint("finding the beats of '" + music->name + "'");
        auto found = analysisServices->provider->beats(*music, 0, analysis::BeatOptions{}, [&c](double) {
            c.checkpoint("finding the beats");
            return true;
        });
        if (!found) {
            fail(ErrorCode::EditRejected, "the beats of '" + music->name + "' could not be found: " + found.error().message);
        }
        beatAnalysis = std::move(*found);
        for (Ticks b : beatAnalysis->beats) {
            if (b >= musicStart) {
                beats.push_back(b - musicStart);
            }
        }
        if (beats.size() < 2) {
            fail(ErrorCode::EditRejected, "'" + music->name + "' has too few beats after \"musicStart\" to cut to");
        }
    }

    // The reel: the source's tracks, empty.
    ProjectState state = c.state();
    const std::string name = c.has("name") ? c.string("name") : source.name + " - Highlights";
    auto copy = reframe::duplicateSequence(state, source.id, name, source.width, source.height);
    if (!copy) {
        fail(ErrorCode::EditRejected, copy.error().message);
    }
    state = std::move(copy->state);
    auto reel = std::make_shared<Sequence>(*state.findSequence(copy->sequence));
    for (std::vector<TrackPtr>* tracks : {&reel->videoTracks, &reel->audioTracks}) {
        for (TrackPtr& t : *tracks) {
            auto track = std::make_shared<Track>(*t);
            track->clips = ClipList(std::vector<Clip>{});
            track->transitions.clear();
            t = std::move(track);
        }
    }
    reel->markers.clear();
    reel->captionTracks.clear();
    reel->markIn.reset();
    reel->markOut.reset();
    reel->workArea.reset();
    reel->beatGrid.reset();

    IdGenerator& ids = state.ids;
    Ticks cursor;
    std::size_t beatIndex = 0; // the beat the cursor is on
    std::vector<std::pair<std::vector<ClipId>, std::vector<ClipId>>> segments; // first / last clips of each segment
    Json made = Json::array();
    bool speedSkipped = false;
    for (std::size_t i = 0; i < moments.size(); ++i) {
        Moment& m = moments[i];
        c.checkpoint("cutting moment " + std::to_string(i + 1) + " of " + std::to_string(moments.size()));
        Ticks pre = m.peak - m.range.start;
        Ticks post = m.range.end() - m.peak;
        Ticks slowSource;
        Speed speed{1, 1};
        if (slowMo) {
            speed = Speed{static_cast<std::int32_t>(std::lround(std::clamp(slowSpeed, 0.1, 1.0) * 1000.0)), 1000};
            slowSource = std::min(frameFloor(fromSeconds(slowSeconds), frame), post + pre);
            // The slow part is centred on the peak (a third before it).
            const Ticks before = std::min(pre, Ticks{slowSource.value / 3});
            pre = pre - before;
            post = post - (slowSource - before);
            if (post < Ticks{}) {
                slowSource = slowSource + post;
                post = Ticks{};
            }
        }
        const Ticks slowLength = slowMo ? toTimelineTicks(slowSource, speed) : Ticks{};
        if (!beats.empty()) {
            // The segment ends on a beat: a multiple of beatsPerCut beats after the one it starts on.
            const Ticks wanted = pre + slowLength + post;
            std::size_t best = beatIndex;
            Ticks bestError{std::numeric_limits<std::int64_t>::max()};
            for (std::size_t e = beatIndex + std::max<std::size_t>(1, beatsPerCut); e < beats.size(); e += std::max<std::size_t>(1, beatsPerCut)) {
                const Ticks length = beats[e] - cursor;
                if (length <= slowLength + frame) {
                    continue;
                }
                const Ticks error = length > wanted ? length - wanted : wanted - length;
                if (error < bestError) {
                    bestError = error;
                    best = e;
                }
                if (length > wanted) {
                    break;
                }
            }
            if (best == beatIndex) {
                warnings.push_back("the music ends before moment " + std::to_string(i + 1) + ": the reel stops there");
                break;
            }
            const Ticks target = beats[best] - cursor;
            // Lengthen or shorten the footage around the slow part, keeping the peak where it was in the cut.
            const Ticks change = target - (pre + slowLength + post);
            const double share = pre + post > Ticks{} ? static_cast<double>(pre.value) / static_cast<double>((pre + post).value) : 0.5;
            Ticks preChange{static_cast<std::int64_t>(std::llround(static_cast<double>(change.value) * share))};
            pre = std::max(Ticks{}, pre + preChange);
            post = std::max(Ticks{}, target - slowLength - pre);
            beatIndex = best;
        }
        const Ticks slowStart = m.peak - (slowMo ? std::min(m.peak - m.range.start, Ticks{slowSource.value / 3}) : Ticks{});
        Ticks from = std::max(Ticks{}, slowStart - pre);
        Ticks to = std::min(sourceEnd, slowStart + slowSource + post);
        std::vector<ClipId> first;
        std::vector<ClipId> last;
        Ticks at = cursor;
        std::vector<Piece> pieces;
        if (slowMo && slowSource > Ticks{}) {
            pieces.push_back({from, slowStart, Speed{1, 1}});
            pieces.push_back({slowStart, slowStart + slowSource, speed});
            pieces.push_back({slowStart + slowSource, to, Speed{1, 1}});
        } else {
            pieces.push_back({from, to, Speed{1, 1}});
        }
        for (std::size_t p = 0; p < pieces.size(); ++p) {
            if (pieces[p].to <= pieces[p].from) {
                continue;
            }
            std::vector<ClipId> pieceFirst;
            std::vector<ClipId> pieceLast;
            // Each piece links its own clips: a group spans one piece (a razor cut in the source's terms).
            std::map<std::uint64_t, LinkGroupId> pieceGroups;
            at = at + copyPiece(source, *reel, pieces[p], at, ids, pieceGroups, pieceFirst, pieceLast, speedSkipped);
            if (first.empty()) {
                first = pieceFirst;
            }
            last = pieceLast;
        }
        segments.emplace_back(std::move(first), std::move(last));
        made.push_back(Json{{"moment", i + 1},
                            {"sourceStart", rounded(from.toSeconds(), 1000.0)},
                            {"sourceEnd", rounded(to.toSeconds(), 1000.0)},
                            {"start", rounded(cursor.toSeconds(), 1000.0)},
                            {"end", rounded(at.toSeconds(), 1000.0)},
                            {"slowMotion", slowMo && slowSource > Ticks{}}});
        if (!m.title.empty()) {
            made.back()["title"] = m.title;
        }
        cursor = beats.empty() ? at : beats[beatIndex];
        if (!beats.empty() && at != cursor) {
            // Rounding left the footage a little off the beat: trim or extend the last clips onto it.
            const Ticks delta = cursor - at;
            for (std::vector<TrackPtr>* tracks : {&reel->videoTracks, &reel->audioTracks}) {
                for (TrackPtr& t : *tracks) {
                    std::vector<Clip> clips = t->clips.toVector();
                    bool changed = false;
                    for (Clip& clip : clips) {
                        if (clip.end() == at && std::find(segments.back().second.begin(), segments.back().second.end(), clip.id) != segments.back().second.end()) {
                            clip.moveTail(delta);
                            changed = true;
                        }
                    }
                    if (changed) {
                        auto track = std::make_shared<Track>(*t);
                        track->clips = ClipList(std::move(clips));
                        t = std::move(track);
                    }
                }
            }
        }
    }
    if (speedSkipped) {
        warnings.push_back("clips that were already sped up, reversed or time-remapped kept their speed in the slow-motion parts");
    }
    if (made.empty()) {
        fail(ErrorCode::EditRejected, "no moment fitted in the reel");
    }
    reel->duration = cursor;

    // The music, on a track of its own, from "musicStart" for the length of the reel.
    if (music) {
        auto track = std::make_shared<Track>();
        track->id = ids.allocate<TrackId>();
        track->kind = TrackKind::Audio;
        track->name = "Music";
        Clip clip;
        clip.id = ids.allocate<ClipId>();
        clip.media = music->id;
        clip.stream = StreamRef{StreamKind::Audio, 0};
        clip.sourceIn = musicStart;
        const Ticks available = music->info.duration > musicStart ? music->info.duration - musicStart : Ticks{};
        clip.timelineRange = TimeRange{Ticks{}, std::min(cursor, available)};
        if (available < cursor) {
            warnings.push_back("the music is shorter than the reel");
        }
        track->clips = ClipList(std::vector<Clip>{clip});
        reel->audioTracks.push_back(std::move(track));
        BeatGrid grid;
        grid.bpmMilli = static_cast<std::int64_t>(std::llround(beatAnalysis->bpm * 1000.0));
        grid.beatsPerBar = beatAnalysis->beatsPerBar;
        grid.beats = beats;
        grid.phase = beats.front();
        for (Ticks d : beatAnalysis->downbeats) {
            if (d >= musicStart) {
                grid.phase = d - musicStart;
                break;
            }
        }
        reel->beatGrid = grid;
    }
    Json params{{"moments", made.size()}, {"order", order}, {"slowMotion", slowMo}, {"transition", transition}};
    if (music) {
        params["music"] = music->id.value;
        params["beatsPerCut"] = beatsPerCut;
    }
    reel->derivedFrom = SequenceDerivation{source.id, std::string(kGenerator), params.dump()};
    const SequenceId reelId = reel->id;
    state = state.withSequence(std::move(reel));

    // Transitions at the cuts between moments, on every track that has clips meeting there.
    std::size_t transitions = 0;
    if (transition != "none" && transitionLength > Ticks{} && segments.size() > 1) {
        const Sequence* built = state.findSequence(reelId);
        std::vector<std::pair<TrackId, bool>> tracks;
        for (const TrackPtr& t : built->videoTracks) {
            tracks.emplace_back(t->id, true);
        }
        for (const TrackPtr& t : built->audioTracks) {
            tracks.emplace_back(t->id, false);
        }
        for (std::size_t s = 1; s < segments.size(); ++s) {
            for (const auto& [trackId, isVideo] : tracks) {
                const Sequence* now = state.findSequence(reelId);
                const auto position = now->trackPosition(trackId);
                if (!position) {
                    continue;
                }
                const Track& track = *(isVideo ? now->videoTracks : now->audioTracks)[position->index];
                const Clip* out = nullptr;
                const Clip* in = nullptr;
                for (const Clip& clip : track.clips) {
                    if (std::find(segments[s - 1].second.begin(), segments[s - 1].second.end(), clip.id) != segments[s - 1].second.end()) {
                        out = &clip;
                    }
                    if (!in && std::find(segments[s].first.begin(), segments[s].first.end(), clip.id) != segments[s].first.end()) {
                        in = &clip;
                    }
                }
                if (!out || !in || out->end() != in->start()) {
                    continue;
                }
                wabcut::edit::TransitionSpec spec;
                spec.outgoing = out->id;
                spec.incoming = in->id;
                spec.before = Ticks{transitionLength.value / 2};
                spec.after = transitionLength - spec.before;
                spec.effect = isVideo ? transition : std::string("constantPower");
                auto added = wabcut::edit::addTransition(state, reelId, trackId, spec, wabcut::edit::HandleFit::UseAvailable);
                if (added) {
                    state = std::move(added->state);
                    ++transitions;
                }
            }
        }
    }

    CommandOutput out;
    out.state = std::move(state);
    out.createdIds.push_back(reelId.value);
    out.data = Json{{"source", source.id.value},
                    {"sequence", reelId.value},
                    {"name", name},
                    {"duration", rounded(cursor.toSeconds(), 1000.0)},
                    {"moments", std::move(made)},
                    {"transitions", transitions}};
    if (music) {
        out.data["music"] = music->id.value;
        out.data["bpm"] = rounded(beatAnalysis->bpm, 10.0);
    }
    out.warnings = std::move(warnings);
    return out;
}

// --- highlights.toShort ----------------------------------------------------------------------------------------------

/// One nested command on the Short's session; a failure becomes a warning (the Short is still made).
bool runStep(command::CommandEngine& engine, const std::string& name, Json params, std::vector<std::string>& warnings,
             command::CommandResult* result = nullptr) {
    command::CommandRequest request;
    request.command = name;
    request.params = std::move(params);
    command::CommandResult r = engine.execute(request);
    if (!r.ok()) {
        warnings.push_back(name + ": " + r.error->message);
        return false;
    }
    if (result) {
        *result = std::move(r);
    }
    return true;
}

CommandOutput toShortCommand(ExecutionContext& c) {
    const Sequence& source = c.sequence();
    Moment m;
    if (c.has("start") || c.has("end")) {
        if (!c.has("start") || !c.has("end") || !(c.number("end") > c.number("start")) || c.number("start") < 0.0) {
            fail(ErrorCode::InvalidParams, "give \"start\" and \"end\" (seconds, end after start), or \"moment\"");
        }
        m.range = TimeRange::fromStartEnd(fromSeconds(c.number("start")), fromSeconds(c.number("end")));
        m.peak = m.range.start + Ticks{m.range.duration.value * 2 / 3};
    } else {
        const std::vector<Moment> marked = markedMoments(source);
        const auto rank = static_cast<std::size_t>(c.integer("moment", 1));
        if (marked.empty()) {
            fail(ErrorCode::EditRejected, "sequence '" + source.name + "' has no highlight markers: run highlights.find first, or pass start/end");
        }
        if (rank < 1 || rank > marked.size()) {
            fail(ErrorCode::InvalidParams, "\"moment\" must be between 1 and " + std::to_string(marked.size()));
        }
        m = marked[rank - 1];
    }
    const Ticks sourceEnd = std::max(source.duration, source.contentEnd());
    const Ticks frame{source.frameRate.ticksPerUnit()};
    const double before = c.has("handleBefore") ? c.number("handleBefore") : 0.0;
    const double after = c.has("handleAfter") ? c.number("handleAfter") : 0.0;
    const Ticks start = frameFloor(std::max(Ticks{}, m.range.start - fromSeconds(before)), frame);
    const Ticks end = std::min(sourceEnd, frameCeil(m.range.end() + fromSeconds(after), frame));
    if (end <= start) {
        fail(ErrorCode::InvalidParams, "the moment lies outside the sequence");
    }
    if (end - start > fromSeconds(180.0)) {
        fail(ErrorCode::InvalidParams, "a Short is at most 3 minutes: pick a shorter moment");
    }
    const std::string title = c.has("title") ? c.string("title") : m.title;
    const bool hookTitle = !title.empty() && c.boolean("hookTitle", true);

    // The Short: the moment cut out of the source (every track: gameplay, facecam, sound) into a 1080x1920 sequence.
    constexpr int kWidth = 1080;
    constexpr int kHeight = 1920;
    std::string name = source.name + " - Highlight Short" + (title.empty() ? "" : ": " + title);
    auto cut = shorts::cutSequence(c.state(), source.id, TimeRange::fromStartEnd(start, end), std::move(name), kWidth, kHeight);
    if (!cut) {
        fail(cut.error().code == wabcut::ErrorCode::InvalidArgument ? ErrorCode::InvalidParams : ErrorCode::EditRejected, cut.error().message);
    }
    const SequenceId id = cut->sequence;
    const Sequence& made = *cut->state.findSequence(id);
    // The gameplay: the lowest video clip at the peak. The facecam: the one asked for, or a clip of other media above
    // it that runs through most of the Short (OBS's webcam source), unless the webcam is a region of the capture.
    const Ticks length = end - start;
    const Ticks peak = std::clamp(m.peak - start, Ticks{}, length - Ticks{1});
    ClipId gameplay;
    MediaId gameplayMedia;
    std::size_t gameplayTrack = 0;
    for (std::size_t k = 0; k < made.videoTracks.size() && !gameplay.isValid(); ++k) {
        for (const Clip& clip : made.videoTracks[k]->clips) {
            if (clip.enabled && clip.start() <= peak && peak < clip.end()) {
                gameplay = clip.id;
                gameplayMedia = clip.media;
                gameplayTrack = k;
                break;
            }
        }
    }
    if (!gameplay.isValid()) {
        fail(ErrorCode::EditRejected, "there is no picture at the moment's peak to lay out");
    }
    ClipId facecam;
    if (c.has("facecam")) {
        const ClipId given = c.clip("facecam");
        const auto it = cut->clips.find(given.value);
        if (it == cut->clips.end() || !made.findClip(it->second)) {
            fail(ErrorCode::InvalidParams, "the facecam clip does not play during the moment");
        }
        facecam = it->second;
    } else if (!c.has("facecamRegion")) {
        for (std::size_t k = gameplayTrack + 1; k < made.videoTracks.size() && !facecam.isValid(); ++k) {
            for (const Clip& clip : made.videoTracks[k]->clips) {
                if (clip.enabled && !clip.isNested() && clip.media != gameplayMedia && Ticks{clip.timelineRange.duration.value * 2} >= length) {
                    facecam = clip.id;
                    break;
                }
            }
        }
    }
    std::string layout = c.string("layout", "auto");
    if (layout == "auto") {
        layout = facecam.isValid() || c.has("facecamRegion") ? "verticalSplit" : "verticalBlurFill";
    }

    std::vector<std::string> warnings;
    EditSession session(std::move(cut->state));
    const std::shared_ptr<command::CommandServices> services(c.services(), [](command::CommandServices*) {}); // borrowed
    command::CommandEngine engine(session, edit::editingCommands(), command::EnginePolicy{command::CapabilitySet::all(), 16, services});
    Json layoutParams{{"sequence", id.value}, {"preset", layout}, {"gameplay", gameplay.value}};
    if (facecam.isValid()) {
        layoutParams["facecam"] = facecam.value;
    }
    for (const char* key : {"facecamRegion", "split", "facecamOnTop", "blur", "zoom"}) {
        if (c.has(key)) {
            layoutParams[key] = c.params()[key];
        }
    }
    command::CommandRequest layoutRequest;
    layoutRequest.command = "layout.apply";
    layoutRequest.params = std::move(layoutParams);
    if (command::CommandResult laid = engine.execute(layoutRequest); !laid.ok()) {
        fail(laid.error->code, "the vertical layout: " + laid.error->message);
    }
    Json report{{"layout", layout}, {"gameplay", gameplay.value}};
    if (facecam.isValid()) {
        report["facecam"] = facecam.value;
    }
    if (c.boolean("captions", true)) {
        // 0.4: the "Shorts" caption style by default (big outlined words two thirds down, above the platforms' buttons)
        // with short cues; another preset gets mid-size cues; "" the default look. An unknown preset is a warning.
        const std::string preset = c.has("captionPreset") ? c.string("captionPreset") : std::string("Shorts");
        const bool shortsPreset = std::ranges::equal(preset, std::string_view("shorts"), [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) == b;
        });
        Json generate{{"sequence", id.value}, {"maxLines", 2}};
        // shorts.create's lines (ShortsCommands.cpp kShortsCaptionChars, kVerticalCaptionChars; fix round 1: 12 and 20
        // wrapped on a rendered 1080x1920 frame).
        generate["maxCharsPerLine"] = shortsPreset ? 10 : 14;
        generate["maxDuration"] = shortsPreset ? 2.5 : 4.0;
        command::CommandResult generated;
        report["captions"] = runStep(engine, "captions.generate", std::move(generate), warnings, &generated);
        if (report["captions"].get<bool>() && !preset.empty() &&
            runStep(engine, "captions.applyPreset",
                    Json{{"sequence", id.value}, {"captionTrack", generated.data.value("captionTrack", Json())}, {"preset", preset}},
                    warnings)) {
            report["captionPreset"] = preset;
        }
    }
    if (hookTitle) {
        command::CommandResult track;
        if (runStep(engine, "sequence.addTrack", Json{{"sequence", id.value}, {"kind", "video"}, {"name", "Hook title"}}, warnings, &track)) {
            command::CommandResult added;
            const Ticks shown = std::min(fromSeconds(c.has("titleSeconds") ? c.number("titleSeconds") : 3.0), length);
            if (runStep(engine, "title.add",
                        Json{{"sequence", id.value},
                             {"text", title},
                             {"at", 0},
                             {"duration", shown.value},
                             {"track", track.data["track"]},
                             {"style", Json{{"size", kHeight / 20}}}},
                        warnings, &added) &&
                !added.createdIds.empty()) {
                runStep(engine, "clip.setParam",
                        Json{{"sequence", id.value}, {"clips", Json::array({added.createdIds.front()})}, {"param", "motion.position.y"},
                             {"value", -0.3 * kHeight}},
                        warnings);
                report["title"] = title;
            }
        }
    }
    if (c.boolean("normalize", true)) {
        report["normalized"] = runStep(engine, "audio.normalize", Json{{"sequence", id.value}, {"preset", "youtube"}}, warnings);
    }
    ProjectState state = *session.snapshot();
    auto seq = std::make_shared<Sequence>(*state.findSequence(id));
    seq->derivedFrom = SequenceDerivation{source.id, std::string(kGenerator),
                                          Json{{"short", true}, {"layout", layout}, {"start", rounded(start.toSeconds(), 1000.0)}, {"end", rounded(end.toSeconds(), 1000.0)}}.dump()};
    state = state.withSequence(std::move(seq));

    CommandOutput out;
    out.state = std::move(state);
    out.createdIds.push_back(id.value);
    out.data = Json{{"source", source.id.value},
                    {"sequence", id.value},
                    {"start", rounded(start.toSeconds(), 1000.0)},
                    {"end", rounded(end.toSeconds(), 1000.0)},
                    {"width", kWidth},
                    {"height", kHeight}};
    out.data.update(report);
    out.warnings = std::move(warnings);
    return out;
}

void add(command::CommandRegistry& registry, command::CommandSpec spec) {
    [[maybe_unused]] const auto added = registry.add(std::move(spec));
}

} // namespace

Json momentToJson(const Moment& m, std::size_t rank) {
    return Json{{"rank", rank},
                {"start", rounded(m.range.start.toSeconds(), 1000.0)},
                {"end", rounded(m.range.end().toSeconds(), 1000.0)},
                {"peak", rounded(m.peak.toSeconds(), 1000.0)},
                {"thumbnail", rounded(m.peak.toSeconds(), 1000.0)}, // the frame to show for the moment: its peak
                {"duration", rounded(m.range.duration.toSeconds(), 1000.0)},
                {"startTicks", m.range.start.value},
                {"endTicks", m.range.end().value},
                {"peakTicks", m.peak.value},
                {"score", rounded(m.score, 1000.0)},
                {"confidence", rounded(m.confidence, 1000.0)},
                {"confidenceLabel", std::string(confidenceLabel(m.confidence))},
                {"title", m.title},
                {"reasons", m.reasons},
                {"signals", m.levels}};
}

std::vector<Moment> markedMoments(const Sequence& sequence) {
    std::vector<Moment> moments;
    for (const Marker& marker : sequence.markers) {
        if (marker.highlight) {
            // Any marker with highlight metadata: this finder's, another tool's or the user's (C.15).
            Moment m;
            m.range = TimeRange{marker.start, std::max(marker.duration, Ticks{1})};
            m.peak = std::clamp(marker.highlight->peak, m.range.start, m.range.end() - Ticks{1});
            m.score = marker.highlight->score;
            m.confidence = marker.highlight->confidence == HighlightConfidence::High     ? 0.85
                           : marker.highlight->confidence == HighlightConfidence::Medium ? 0.6
                                                                                         : 0.35;
            m.reasons = marker.highlight->reasons;
            const std::size_t colon = marker.name.find(": ");
            m.title = marker.name.starts_with(kMarkerPrefix) && colon != std::string::npos ? marker.name.substr(colon + 2) : marker.name;
            moments.push_back(std::move(m));
            continue;
        }
        if (!isHighlightMarker(marker)) {
            continue;
        }
        Moment m;
        m.range = TimeRange{marker.start, std::max(marker.duration, Ticks{1})};
        m.peak = marker.start + Ticks{m.range.duration.value * 2 / 3};
        const std::string_view rest = std::string_view(marker.comment).substr(kCommentPrefix.size());
        double score = 0.0;
        const auto [end, ec] = std::from_chars(rest.data(), rest.data() + rest.size(), score);
        (void)end;
        m.score = ec == std::errc() ? score : 0.0;
        const std::size_t colon = marker.name.find(": ");
        m.title = colon == std::string::npos ? std::string() : marker.name.substr(colon + 2);
        moments.push_back(std::move(m));
    }
    std::stable_sort(moments.begin(), moments.end(), [](const Moment& a, const Moment& b) { return a.score > b.score; });
    return moments;
}

void registerHighlightCommands(command::CommandRegistry& registry) {
    using command::Capability;
    const Json seconds = schema::number("Seconds.", 0.0, 86400.0);
    add(registry,
        {"highlights.find", 1, "Find Highlights",
         "Finds the best moments of a gaming recording on this computer: game-audio spikes, the player's voice (louder "
         "or higher than usual, shouting, laughter), the picture (fast action, flashes, bursts of picture changes), the "
         "recording's imported Twitch / YouTube chat log (busy chat, emotes; highlights.importChat) and OBS chapter "
         "markers or a marker file next to the recording. Each moment gets a score, a confidence and the reasons, and a "
         "marker (\"Highlight 1: ...\"; earlier highlight markers are replaced). For a media item the moments are in "
         "media time and become clip markers. Measurements are cached per source file.",
         Capability::EditSequence,
         withSequence({{"media", mediaParam("Analyse this media item instead of the sequence.")},
                       {"signals", schema::array(schema::enumeration({"game", "mic", "visual", "chat", "markers"}, "A signal."),
                                                 "The signals to use. Default all (\"visual\" decodes the picture: the slow part).")},
                       {"audioRoles", schema::array(schema::enumeration({"game", "mic", "mixed", "ignore"}, "Role."),
                                                    "What each audio stream of the recordings carries, by stream (OBS track 1, 2, ...). "
                                                    "Default: the most voice-like of several streams is the mic, the others game; "
                                                    "a single stream is mixed.")},
                       {"chatOffset", schema::number("Chat time + this = recording time, seconds (default: as imported).", -86400.0, 86400.0)},
                       {"chatDelay", schema::number("How late chat reacts, seconds (default 6).", 0.0, 120.0)},
                       {"keywords", schema::array(schema::string("A word or emote."), "Words and emotes that count as reactions, besides the usual ones.")},
                       {"count", schema::integer("How many moments at most. Default 10.", 1, 200)},
                       {"minScore", schema::number("Score a moment needs (0..1). Default 0.25.", 0.0, 1.0)},
                       {"minGap", schema::number("Seconds between two moments' peaks. Default 20.", 0.0, 3600.0)},
                       {"markers", schema::boolean("Mark the moments. Default true.", true)}}),
         Json{{"signals", Json::array({"game", "mic", "visual"})}, {"count", 8}}, {}, findCommand});
    add(registry,
        command::touchesFiles(
            {"highlights.importChat", 1, "Import Chat Log",
             "Reads a stream's chat log (TwitchDownloader JSON, yt-dlp live_chat.json or chat-downloader JSON) and keeps it "
             "with a recording's analyses, so highlights.find counts busy chat and emotes. Nothing is uploaded.",
             Capability::MediaAnalysis,
             withSequence({{"file", schema::string("The chat log file.")},
                           {"media", mediaParam("The recording it belongs to (default: the longest one the sequence plays).")},
                           {"offset", schema::number("Chat time + this = recording time, seconds. Default 0.", -86400.0, 86400.0)}},
                          {"file"}),
             Json{{"file", "C:/Users/me/Videos/stream-chat.json"}}, {}, importChatCommand},
            {"file"}));
    add(registry,
        {"highlights.toReel", 1, "Make Highlight Reel",
         "Makes a new sequence from highlight moments (the sequence's highlight markers, or \"moments\"): each moment with "
         "handles, best first up to \"count\" / \"maxDuration\", in time or score order, with transitions between them. With "
         "\"music\" the cuts land on its beats (every \"beatsPerCut\" beats) and the music plays under the reel; "
         "\"slowMotion\" slows the moment's peak. One undo step.",
         Capability::EditProject,
         withSequence({{"moments", schema::array(schema::object({{"start", seconds}, {"end", seconds}, {"peak", seconds},
                                                                 {"score", schema::number("0..1", 0.0, 1.0)}, {"title", schema::string("Label.")}},
                                                                {"start", "end"}),
                                                 "Moments in sequence seconds (default: the highlight markers).")},
                       {"order", schema::enumeration({"time", "score"}, "Order of the moments in the reel. Default time.")},
                       {"count", schema::integer("How many moments at most (best first). Default all, at most 50.", 1, 50)},
                       {"maxDuration", schema::number("Longest reel, seconds (best moments first).", 1.0, 3600.0)},
                       {"handleBefore", schema::number("Seconds added before each moment. Default 0.5.", 0.0, 30.0)},
                       {"handleAfter", schema::number("Seconds added after each moment. Default 0.5.", 0.0, 30.0)},
                       {"transition", schema::enumeration({"crossDissolve", "dipToBlack", "dipToWhite", "none"},
                                                          "Transition between moments (audio crossfades). Default crossDissolve.")},
                       {"transitionDuration", schema::number("Seconds. Default 0.5.", 0.0, 5.0)},
                       {"music", mediaParam("Music to cut to: the cuts land on its beats and it plays under the reel.")},
                       {"musicStart", schema::number("Where the music starts, seconds into it. Default 0.", 0.0, 86400.0)},
                       {"beatsPerCut", schema::integer("Cuts every this many beats (or a multiple). Default 4.", 1, 64)},
                       {"slowMotion", schema::boolean("Slow down each moment's peak. Default false.", false)},
                       {"slowMotionSpeed", schema::number("Speed of the slow part (0.1..1). Default 0.5.", 0.1, 1.0)},
                       {"slowMotionSeconds", schema::number("Seconds of footage slowed. Default 1.5.", 0.1, 10.0)},
                       {"name", schema::string("Name of the new sequence. Default \"<sequence> - Highlights\".")}}),
         Json{{"order", "time"}, {"count", 8}, {"transition", "crossDissolve"}}, {}, toReelCommand});
    add(registry,
        {"highlights.toShort", 1, "Make Highlight Short",
         "Makes one highlight moment (a highlight marker by rank, or start/end) a vertical 1080x1920 Short: the moment cut "
         "out with every track, laid out with layout.apply (\"verticalSplit\" with the facecam under the gameplay when "
         "there is one: a facecam clip above the gameplay or \"facecamRegion\"; else \"verticalBlurFill\": the whole game "
         "view over a blurred copy), captioned (the \"Shorts\" caption style unless \"captionPreset\" says otherwise), with "
         "the moment's title as the hook and loudness for Shorts. One undo step.",
         Capability::EditProject,
         withSequence({{"moment", schema::integer("Rank of the highlight marker. Default 1 (the best).", 1, 200)},
                       {"start", seconds},
                       {"end", seconds},
                       {"handleBefore", schema::number("Seconds added before. Default 0.", 0.0, 30.0)},
                       {"handleAfter", schema::number("Seconds added after. Default 0.", 0.0, 30.0)},
                       {"layout", schema::enumeration({"auto", "verticalSplit", "verticalBlurFill"},
                                                      "The layout (layout.apply). Default auto: verticalSplit with a facecam, else verticalBlurFill.")},
                       {"facecam", Json{{"type", Json::array({"integer", "object"})},
                                        {"description", "The facecam clip in the sequence (default: a clip of other media above the gameplay)."}}},
                       {"facecamRegion", schema::object({{"x", schema::number("Left, a fraction of the frame width.", 0.0, 1.0)},
                                                         {"y", schema::number("Top, a fraction of the frame height.", 0.0, 1.0)},
                                                         {"width", schema::number("A fraction of the frame width.", 0.01, 1.0)},
                                                         {"height", schema::number("A fraction of the frame height.", 0.01, 1.0)}},
                                                        {"x", "y", "width", "height"})},
                       {"split", schema::number("verticalSplit: the gameplay's share of the height. Default 0.6.", 0.2, 0.8)},
                       {"facecamOnTop", schema::boolean("verticalSplit: the facecam on top.", false)},
                       {"blur", schema::number("verticalBlurFill: the background's blur radius. Default 40.", 0.0, 500.0)},
                       {"zoom", schema::number("verticalBlurFill: the gameplay's scale over fit-to-width. Default 1.", 1.0, 4.0)},
                       {"title", schema::string("Hook title (default: the moment's title).")},
                       {"hookTitle", schema::boolean("Show the title at the start. Default true.", true)},
                       {"captions", schema::boolean("Captions from the transcript. Default true.", true)},
                       {"captionPreset",
                        Json{{"type", "string"},
                             {"description", "The caption style: a built-in preset (Shorts, Clean, Bold Box, Podcast, Thai-friendly) or "
                                             "one of the project's. Default \"Shorts\" for vertical output, none (the default look) "
                                             "otherwise; \"\" = the default look."},
                             {"maxLength", 64}}},
                       {"normalize", schema::boolean("Loudness for Shorts. Default true.", true)}}),
         Json{{"moment", 1}}, {}, toShortCommand});
}

} // namespace wabcut::ai::highlights
