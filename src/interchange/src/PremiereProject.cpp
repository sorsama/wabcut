// Experimental reader for Adobe Premiere Pro projects (.prproj): gzip-compressed XML holding an object
// graph whose schema Adobe does not document. The structure followed here was worked out from real
// projects (Premiere 10.4 to 26.3) with the help of the MIT-licensed py-premiere library
// (github.com/Pouf/py-premiere, whose samples with ExtendScript ground truth are our test fixtures):
//
//   <PremiereData Version="3">                    objects are the root's direct children only:
//     <X ObjectID="n"> ... <Y ObjectRef="n"/>       per-save integer ids
//     <X ObjectUID="guid"> ... <Y ObjectURef="guid"/> persistent ids
//   Sequence(UID) -> TrackGroups/TrackGroup{First=media-type GUID, Second=ref} -> Video/AudioTrackGroup
//     -> TrackGroup/{FrameRate (ticks per frame/sample), Tracks/Track(URef)} -> Video/AudioClipTrack
//     -> ClipTrack/{Track/{IsLocked,IsMuted}, ClipItems/TrackItems/TrackItem(Ref), TransitionItems/...}
//   Video/AudioClipTrackItem -> ClipTrackItem/{TrackItem/{Start,End}, IsMuted (= disabled), SubClip(Ref)}
//   SubClip -> {Name, Clip(Ref)} -> Video/AudioClip -> Clip/{InPoint, OutPoint, PlaybackSpeed, Source(Ref)}
//   Video/AudioMediaSource -> MediaSource/Media(URef) -> Media/{ActualMediaFilePath, FilePath, Title,
//   Infinite} Video/AudioSequenceSource -> SequenceSource/Sequence(URef)            (a nested sequence)
//   Sequence/PersistentGroupContainer/LinkContainer/Links/Link(Ref) ->
//   TrackItemGroup/TrackItems/TrackItem(Ref) Sequence/MarkerOwner/Markers(Ref) -> Markers/Marker/Second(Ref)
//   -> Marker/DVAMarker (JSON)
// Times are Premiere ticks, which are Wabcut ticks: positions carry over losslessly. Elements at their
// default value are omitted by recent versions (Start 0, PlaybackSpeed 1, IsMuted false).
//
// Nested sequences (X.1): a clip whose source is a Video/AudioSequenceSource plays that sequence (In in its time).
// A multicam clip (a source whose element name says MultiCam, holding a SequenceSource) plays its source sequence as
// a nested clip; its angle switches are not read (no ground-truth sample yet).
//
// Not read: effects and keyframes (component chains), time remapping curves, multicam angle switches,
// captions, bins (everything lands in the import bin). See docs/interchange.md.

#include "ImporterCommon.h"
#include "Importers.h"
#include "TimeMath.h"
#include "XmlUtil.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <nlohmann/json.hpp>
#include <numeric>
#include <unordered_map>

namespace wabcut::interchange::detail {

namespace {

constexpr std::string_view kVideoGroup = "228cda18-3625-4d2d-951e-348879e4ed93";
constexpr std::string_view kAudioGroup = "80b8e3d5-6dca-4195-aefb-cb5f407ab009";
/// Decompressed size limit (real projects reach a few hundred MB of XML).
constexpr std::size_t kMaxProjectXml = 512u * 1024u * 1024u;

bool nameIs(const pugi::xml_node& n, const char* name) {
    return std::strcmp(n.name(), name) == 0;
}

/// Exact rate from ticks per frame/sample: kTicksPerSecond / ticksPerUnit, reduced.
std::optional<Rate> rateFromTicksPerUnit(std::int64_t ticksPerUnit) {
    if (ticksPerUnit <= 0) {
        return std::nullopt;
    }
    const std::int64_t g = std::gcd(kTicksPerSecond, ticksPerUnit);
    const std::int64_t num = kTicksPerSecond / g;
    const std::int64_t den = ticksPerUnit / g;
    if (num > std::numeric_limits<std::int32_t>::max() || den > std::numeric_limits<std::int32_t>::max()) {
        return std::nullopt;
    }
    return Rate{static_cast<std::int32_t>(num), static_cast<std::int32_t>(den)};
}

class PrprojParser {
public:
    PrprojParser(std::string_view xml, const ParseContext& context, IrDocument& doc)
        : m_ctx(context), m_doc(doc), m_media(doc), m_loc(xml, context.fileLabel) {}

    std::expected<void, Error> parse(const pugi::xml_document& xml);

private:
    void report(ReportOutcome o, ReportCategory c, std::string reason, const pugi::xml_node& node,
                const std::string& sequence = {}) {
        m_doc.report.add(o, c, std::move(reason), m_loc.locate(node), sequence);
    }
    pugi::xml_node ref(const pugi::xml_node& node) const {
        const auto it = m_byId.find(node.attribute("ObjectRef").value());
        return it == m_byId.end() ? pugi::xml_node() : it->second;
    }
    pugi::xml_node uref(const pugi::xml_node& node) const {
        const auto it = m_byUid.find(node.attribute("ObjectURef").value());
        return it == m_byUid.end() ? pugi::xml_node() : it->second;
    }
    static std::optional<std::int64_t> ticksOf(const pugi::xml_node& node, const char* child) {
        return childInt(node, child);
    }

    /// The IrSequence::key nested clips name a sequence by (its ObjectUID).
    static std::string sequenceKey(const pugi::xml_node& sequence) {
        return "prproj:" + std::string(sequence.attribute("ObjectUID").value());
    }
    std::string sequenceName(const pugi::xml_node& sequence) const {
        std::string name(childText(sequence, "Name"));
        return name.empty() ? std::string("Sequence") : name;
    }
    std::size_t mediaFor(const pugi::xml_node& media, const std::string& clipName);
    void parseSequence(const pugi::xml_node& sequence);
    /// Track item ObjectID -> where its clip went: (video?, track index, clip index).
    using ItemPlaces = std::unordered_map<std::string, std::tuple<bool, std::size_t, std::size_t>>;
    void parseTrack(const pugi::xml_node& track, TrackKind kind, std::size_t index, IrSequence& seq,
                    ItemPlaces& places);
    void parseMarkers(const pugi::xml_node& sequence, IrSequence& seq);

    [[maybe_unused]] const ParseContext& m_ctx; // not read yet (Clang: -Wunused-private-field)
    IrDocument& m_doc;
    MediaTable m_media;
    XmlLocator m_loc;
    std::unordered_map<std::string, pugi::xml_node> m_byId;
    std::unordered_map<std::string, pugi::xml_node> m_byUid;
    std::int64_t m_nextLink = 0;
};

std::size_t PrprojParser::mediaFor(const pugi::xml_node& media, const std::string& clipName) {
    const std::string uid = media.attribute("ObjectUID").value();
    const std::string key = "media:" + uid;
    if (const std::size_t existing = m_media.find(key); existing != kNoMedia) {
        return existing;
    }
    IrMedia m;
    m.key = key;
    m.location = m_loc.locate(media);
    std::string path(childText(media, "ActualMediaFilePath"));
    if (path.empty()) {
        path = std::string(childText(media, "FilePath"));
    }
    m.name = std::string(childText(media, "Title"));
    bool decoded = false;
    m.reference = referenceFrom(path, m.name.empty() ? clipName : m.name, decoded);
    if (m.name.empty()) {
        m.name = m.reference.name.empty() ? clipName : m.reference.name;
    }
    m.stillImage = iequals(childText(media, "Infinite"), "true");
    return m_media.add(std::move(m));
}

void PrprojParser::parseMarkers(const pugi::xml_node& sequence, IrSequence& seq) {
    const pugi::xml_node markers = ref(sequence.child("MarkerOwner").child("Markers"));
    for (const pugi::xml_node entry : markers.child("Markers").children("Marker")) {
        const pugi::xml_node marker = ref(entry.child("Second"));
        const std::string_view text = childText(marker, "DVAMarker");
        IrMarker out;
        out.location = m_loc.locate(marker ? marker : entry);
        nlohmann::json json = nlohmann::json::parse(text.begin(), text.end(), nullptr, false);
        const nlohmann::json* m = nullptr;
        if (json.is_object() && json.contains("DVAMarker") && json["DVAMarker"].is_object()) {
            m = &json["DVAMarker"];
        }
        auto ticksAt = [&](const char* key) -> std::optional<std::int64_t> {
            if (!m || !m->contains(key) || !(*m)[key].is_object()) {
                return std::nullopt;
            }
            const auto& t = (*m)[key];
            if (t.contains("ticks") && t["ticks"].is_number_integer()) {
                return t["ticks"].get<std::int64_t>();
            }
            if (t.contains("ticks") && t["ticks"].is_string()) {
                return parseInt64(t["ticks"].get<std::string>());
            }
            return std::nullopt;
        };
        const auto start = ticksAt("mStartTime");
        if (!m || !start) {
            m_doc.report.add(ReportOutcome::Dropped, ReportCategory::Marker, "a marker could not be read",
                             out.location, seq.name);
            continue;
        }
        auto str = [&](const char* key) {
            return m->contains(key) && (*m)[key].is_string() ? (*m)[key].get<std::string>() : std::string();
        };
        out.marker.start = Ticks{*start};
        out.marker.duration = Ticks{std::max<std::int64_t>(ticksAt("mDuration").value_or(0), 0)};
        out.marker.name = str("mName");
        out.marker.comment = str("mComment");
        // Colour: a cue point {"color": 0xAABBGGRR} (absent = Premiere's default green).
        if (m->contains("mCuePointList") && (*m)["mCuePointList"].is_array()) {
            for (const auto& cue : (*m)["mCuePointList"]) {
                if (!cue.is_object() || !cue.contains("mValue") || !cue["mValue"].is_string()) {
                    continue;
                }
                const auto value = nlohmann::json::parse(cue["mValue"].get<std::string>(), nullptr, false);
                if (value.is_object() && value.contains("color") && value["color"].is_number_unsigned()) {
                    const auto v = value["color"].get<std::uint32_t>();
                    out.marker.colorIndex =
                        markerColorFromRgb(static_cast<int>(v & 0xFF), static_cast<int>((v >> 8) & 0xFF),
                                           static_cast<int>((v >> 16) & 0xFF));
                }
            }
        }
        seq.markers.push_back(std::move(out));
    }
}

void PrprojParser::parseTrack(const pugi::xml_node& track, TrackKind kind, std::size_t index, IrSequence& seq,
                              ItemPlaces& places) {
    const std::string label = (kind == TrackKind::Video ? "V" : "A") + std::to_string(index + 1);
    const pugi::xml_node clipTrack = track.child("ClipTrack");
    const pugi::xml_node header = clipTrack.child("Track");
    IrTrack t;
    t.kind = kind;
    t.location = m_loc.locate(track);
    t.locked = iequals(childText(header, "IsLocked"), "true");
    // Video track mute is the output toggle; audio track mute lives on the mixer and is not read.
    if (kind == TrackKind::Video && iequals(childText(header, "IsMuted"), "true")) {
        t.enabled = false;
    }
    const pugi::xml_node properties = header.child("Node").child("Properties");
    if (const auto targeted = childInt(properties, "MZ.TrackTargeted")) {
        t.targeted = *targeted != 0;
    }

    for (const pugi::xml_node entry :
         clipTrack.child("ClipItems").child("TrackItems").children("TrackItem")) {
        const pugi::xml_node item = ref(entry);
        const pugi::xml_node cti = item.child("ClipTrackItem");
        const pugi::xml_node subClip = ref(cti.child("SubClip"));
        const pugi::xml_node clip = ref(subClip.child("Clip"));
        const pugi::xml_node core = clip.child("Clip");
        std::string name(childText(subClip, "Name"));
        if (!item || !subClip || !clip) {
            report(ReportOutcome::Dropped, ReportCategory::Clip,
                   "a clip on " + label + " could not be resolved", item ? item : entry, seq.name);
            continue;
        }
        const std::int64_t start = ticksOf(cti.child("TrackItem"), "Start").value_or(0);
        const auto end = ticksOf(cti.child("TrackItem"), "End");
        if (!end || *end <= start || start < 0) {
            report(ReportOutcome::Dropped, ReportCategory::Clip,
                   "'" + name + "' on " + label + " has no valid position", item, seq.name);
            continue;
        }
        const pugi::xml_node source = ref(core.child("Source"));
        const std::string_view sourceType = source.name();
        const bool multicam = toLowerAscii(sourceType).find("multicam") != std::string::npos;
        if (sourceType == "VideoSequenceSource" || sourceType == "AudioSequenceSource" || multicam) {
            // X.1: a nested sequence (or a multicam clip's source sequence) plays as a nested clip.
            pugi::xml_node nested = uref(source.child("SequenceSource").child("Sequence"));
            if (!nested && multicam) {
                const pugi::xml_node named = source.find_node([](const pugi::xml_node& n) {
                    return nameIs(n, "Sequence") && n.attribute("ObjectURef");
                });
                nested = uref(named);
            }
            if (!nested || !nameIs(nested, "Sequence")) {
                report(ReportOutcome::Dropped, ReportCategory::Clip,
                       std::string(multicam ? "multicam clip '" : "nested sequence '") + name + "' on " + label +
                           " names a sequence the project does not hold; its time is left as a gap",
                       item, seq.name);
                continue;
            }
            IrClip c;
            c.name = name.empty() ? sequenceName(nested) : name;
            c.sequenceKey = sequenceKey(nested);
            c.start = Ticks{start};
            c.duration = Ticks{*end - start};
            c.sourceIn = Ticks{std::max<std::int64_t>(ticksOf(core, "InPoint").value_or(0), 0)};
            c.enabled = !iequals(childText(cti, "IsMuted"), "true");
            c.location = m_loc.locate(item);
            c.stream = kind == TrackKind::Video ? StreamRef{StreamKind::Video, 0} : StreamRef{StreamKind::Audio, 0};
            if (multicam) {
                report(ReportOutcome::Approximated, ReportCategory::Clip,
                       "multicam clip '" + c.name + "' on " + label + " plays its source sequence '" +
                           sequenceName(nested) + "' as a nested clip (its angle switches are not read)",
                       item, seq.name);
            }
            places[item.attribute("ObjectID").value()] = {kind == TrackKind::Video, index, t.clips.size()};
            t.clips.push_back(std::move(c));
            continue;
        }
        const pugi::xml_node media = uref(source.child("MediaSource").child("Media"));
        if (!media) {
            report(ReportOutcome::Dropped, ReportCategory::Clip,
                   "'" + name + "' on " + label +
                       " is a merged, multicam or synthetic clip, which is not supported yet",
                   item, seq.name);
            continue;
        }
        const std::string path(childText(media, "FilePath"));
        if (!path.empty() && path.find_first_of("/\\") == std::string::npos) {
            report(ReportOutcome::Dropped, ReportCategory::Clip,
                   "generated clip '" + name + "' on " + label + " (no media file) is not supported yet",
                   item, seq.name);
            continue;
        }
        const std::size_t mediaIndex = mediaFor(media, name);
        IrMedia& m = m_doc.media[mediaIndex];
        if (const auto duration = ticksOf(source, "OriginalDuration");
            duration && *duration > 0 && !m.stillImage) {
            m.duration = Ticks{*duration};
        }
        IrClip c;
        c.name = name.empty() ? m.name : name;
        c.media = mediaIndex;
        c.start = Ticks{start};
        c.duration = Ticks{*end - start};
        // InPoint is a media position from the first frame. Stills report a timecode-like In (01:00:00;00);
        // they have no time of their own, so they start at 0.
        c.sourceIn = m.stillImage ? Ticks{} : Ticks{ticksOf(core, "InPoint").value_or(0)};
        c.enabled = !iequals(childText(cti, "IsMuted"), "true");
        c.location = m_loc.locate(item);
        if (kind == TrackKind::Video) {
            m.hasVideo = true;
            c.stream = {StreamKind::Video, 0};
        } else {
            if (m.audioChannels.empty()) {
                m.audioChannels.push_back(0);
            }
            c.stream = {StreamKind::Audio, 0};
        }
        const double speed = childDouble(core, "PlaybackSpeed").value_or(1.0);
        const bool reverse = iequals(childText(core, "PlayBackwards"), "true");
        if ((std::abs(speed - 1.0) > 1e-9 || reverse) && !core.child("TimeRemapping")) {
            // A constant speed: the builder retimes the clip (its length and media In stay).
            if (const auto ratio = speedFromDouble(std::abs(speed))) {
                IrSpeed sp;
                sp.ratio = ratio->speed;
                sp.reversed = reverse;
                sp.location = m_loc.locate(item);
                if (!ratio->exact) {
                    sp.approximation = "the speed " + std::to_string(speed) + " was rounded to " + speedLabel(ratio->speed);
                }
                c.speed = sp;
            } else {
                report(ReportOutcome::Approximated, ReportCategory::Speed,
                       "'" + c.name + "' on " + label + " has an unusable speed; imported at 100%", item, seq.name);
            }
        }
        if (core.child("TimeRemapping")) {
            report(ReportOutcome::Approximated, ReportCategory::Speed,
                   "time remapping of '" + c.name + "' on " + label + " was not read", item, seq.name);
        }
        places[item.attribute("ObjectID").value()] = {kind == TrackKind::Video, index, t.clips.size()};
        t.clips.push_back(std::move(c));
    }

    for (const pugi::xml_node entry :
         clipTrack.child("TransitionItems").child("TrackItems").children("TrackItem")) {
        const pugi::xml_node item = ref(entry);
        const pugi::xml_node tti = item.child("TransitionTrackItem");
        const std::int64_t start = ticksOf(tti.child("TrackItem"), "Start").value_or(0);
        const auto end = ticksOf(tti.child("TrackItem"), "End");
        IrTransition tr;
        tr.name = std::string(childText(tti, "DisplayName"));
        tr.location = m_loc.locate(item ? item : entry);
        if (!end || *end <= start) {
            report(ReportOutcome::Dropped, ReportCategory::Transition,
                   "transition '" + tr.name + "' has no valid extent", item ? item : entry, seq.name);
            continue;
        }
        // Alignment: how much of the transition lies before the cut (ticks), clamped to its extent.
        const std::int64_t before =
            std::clamp<std::int64_t>(ticksOf(tti, "Alignment").value_or(0), 0, *end - start);
        tr.cut = Ticks{start + before};
        tr.before = Ticks{before};
        tr.after = Ticks{*end - start - before};
        const std::string lower = toLowerAscii(tr.name);
        tr.kind = kind == TrackKind::Audio                      ? TransitionKind::AudioCrossfade
                  : lower.find("black") != std::string::npos    ? TransitionKind::DipToBlack
                  : lower.find("white") != std::string::npos    ? TransitionKind::DipToWhite
                  : lower.find("wipe") != std::string::npos     ? TransitionKind::Wipe
                  : lower.find("dissolve") != std::string::npos ? TransitionKind::Dissolve
                                                                : TransitionKind::Other;
        t.transitions.push_back(std::move(tr));
    }
    (kind == TrackKind::Video ? seq.videoTracks : seq.audioTracks).push_back(std::move(t));
}

void PrprojParser::parseSequence(const pugi::xml_node& sequence) {
    IrSequence seq;
    seq.name = sequenceName(sequence);
    seq.key = sequenceKey(sequence);
    seq.location = m_loc.locate(sequence);
    const pugi::xml_node properties = sequence.child("Node").child("Properties");
    // Track groups: video and audio (data tracks hold captions and are not read).
    pugi::xml_node videoGroup;
    pugi::xml_node audioGroup;
    for (const pugi::xml_node group : sequence.child("TrackGroups").children("TrackGroup")) {
        const std::string_view type = childText(group, "First");
        if (type == kVideoGroup) {
            videoGroup = ref(group.child("Second"));
        } else if (type == kAudioGroup) {
            audioGroup = ref(group.child("Second"));
        }
    }
    if (const auto rate =
            rateFromTicksPerUnit(childInt(videoGroup.child("TrackGroup"), "FrameRate").value_or(0))) {
        seq.frameRate = *rate;
    } else {
        report(ReportOutcome::Approximated, ReportCategory::SequenceSetting,
               "the frame rate could not be read; 30 fps assumed", sequence, seq.name);
    }
    if (const auto rate =
            rateFromTicksPerUnit(childInt(audioGroup.child("TrackGroup"), "FrameRate").value_or(0))) {
        seq.audioRate = *rate;
    }
    {
        // FrameRect is "left,top,width,height".
        std::string text(childText(videoGroup, "FrameRect"));
        std::replace(text.begin(), text.end(), ',', ' ');
        const auto parts = splitWords(text);
        if (parts.size() == 4) {
            seq.width = static_cast<std::int32_t>(parseInt64(parts[2]).value_or(1920));
            seq.height = static_cast<std::int32_t>(parseInt64(parts[3]).value_or(1080));
        }
    }
    if (const auto zero = childInt(properties, "MZ.ZeroPoint"); zero && *zero > 0) {
        seq.startTimecode = Ticks{*zero}; // X.1: Sequence::startTimecode
    }
    // In/out marks; unset is a large negative sentinel.
    if (const auto in = childInt(properties, "MZ.InPoint"); in && *in >= 0) {
        seq.markIn = Ticks{*in};
    }
    if (const auto out = childInt(properties, "MZ.OutPoint"); out && *out > 0) {
        seq.markOut = Ticks{*out};
    }

    ItemPlaces places;
    auto tracksOf = [&](const pugi::xml_node& group) {
        std::vector<std::pair<std::int64_t, pugi::xml_node>> tracks;
        for (const pugi::xml_node t : group.child("TrackGroup").child("Tracks").children("Track")) {
            tracks.emplace_back(attrInt(t, "Index").value_or(0), uref(t));
        }
        std::stable_sort(tracks.begin(), tracks.end(),
                         [](const auto& a, const auto& b) { return a.first < b.first; });
        return tracks;
    };
    std::size_t index = 0;
    for (const auto& [order, track] : tracksOf(videoGroup)) {
        parseTrack(track, TrackKind::Video, index++, seq, places);
    }
    index = 0;
    for (const auto& [order, track] : tracksOf(audioGroup)) {
        parseTrack(track, TrackKind::Audio, index++, seq, places);
    }

    // Links: each Link object groups track items (video with its audio channels).
    for (const pugi::xml_node entry :
         sequence.child("PersistentGroupContainer").child("LinkContainer").child("Links").children("Link")) {
        const pugi::xml_node link = ref(entry);
        std::vector<IrClip*> members;
        for (const pugi::xml_node item :
             link.child("TrackItemGroup").child("TrackItems").children("TrackItem")) {
            const auto it = places.find(item.attribute("ObjectRef").value());
            if (it == places.end()) {
                continue;
            }
            const auto [isVideo, trackIndex, clipIndex] = it->second;
            auto& tracks = isVideo ? seq.videoTracks : seq.audioTracks;
            if (trackIndex < tracks.size() && clipIndex < tracks[trackIndex].clips.size()) {
                members.push_back(&tracks[trackIndex].clips[clipIndex]);
            }
        }
        if (members.size() >= 2) {
            const std::int64_t group = m_nextLink++;
            for (IrClip* c : members) {
                c->linkGroup = group;
            }
        }
    }
    parseMarkers(sequence, seq);
    std::sort(seq.markers.begin(), seq.markers.end(),
              [](const IrMarker& a, const IrMarker& b) { return a.marker.start < b.marker.start; });
    m_doc.sequences.push_back(std::move(seq));
}

std::expected<void, Error> PrprojParser::parse(const pugi::xml_document& xml) {
    const pugi::xml_node root = xml.document_element();
    if (!nameIs(root, "PremiereData")) {
        return std::unexpected(parseError("the project's root element is <" + std::string(root.name()) +
                                          ">, not <PremiereData>"));
    }
    // Only the root's direct children are objects; nested scopes (project view state) reuse ids.
    std::vector<pugi::xml_node> sequences;
    for (const pugi::xml_node child : root.children()) {
        if (const char* id = child.attribute("ObjectID").value(); *id) {
            m_byId.emplace(id, child);
        }
        if (const char* uid = child.attribute("ObjectUID").value(); *uid) {
            m_byUid.emplace(uid, child);
            if (nameIs(child, "Sequence")) {
                sequences.push_back(child);
            }
        }
    }
    for (const pugi::xml_node& s : sequences) {
        parseSequence(s);
    }
    // The project's other media (in bins but not in a sequence) comes along too, like a project import in
    // Premiere. Generators store a non-path in FilePath and are skipped.
    for (const pugi::xml_node child : root.children("Media")) {
        const std::string path(childText(child, "FilePath"));
        if (child.attribute("ObjectUID") && path.find_first_of("/\\") != std::string::npos) {
            mediaFor(child, std::string(childText(child, "Title")));
        }
    }
    if (sequences.empty() && m_doc.media.empty()) {
        return std::unexpected(parseError("the Premiere project contains no sequences and no media"));
    }
    m_doc.report.add(
        ReportOutcome::Dropped, ReportCategory::Effect,
        "the experimental .prproj reader does not read effects, keyframes or clip properties; export "
        "Final Cut Pro XML from Premiere to carry opacity, scale and volume",
        m_loc.locate(root));
    return {};
}

} // namespace

std::expected<IrDocument, Error> parsePremiereProject(std::string_view data, const ParseContext& context) {
    std::string inflated;
    std::string_view xmlText = data;
    if (hasGzipMagic(data)) {
        auto unpacked = gunzip(data, kMaxProjectXml);
        if (!unpacked) {
            return std::unexpected(unpacked.error());
        }
        inflated = std::move(*unpacked);
        xmlText = inflated;
    }
    pugi::xml_document xml;
    if (auto parsed = parseXml(xmlText, xml, "the Premiere project"); !parsed) {
        return std::unexpected(parsed.error());
    }
    IrDocument doc;
    PrprojParser parser(xmlText, context, doc);
    if (auto r = parser.parse(xml); !r) {
        return std::unexpected(r.error());
    }
    return doc;
}

} // namespace wabcut::interchange::detail
