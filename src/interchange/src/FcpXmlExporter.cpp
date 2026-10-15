// FCPXML 1.10 / 1.11 export, for Final Cut Pro and DaVinci Resolve, read back exactly by FcpXmlImporter.cpp:
//
//   <fcpxml version="1.10"><resources> <format/> <asset><media-rep/></asset> <effect/>
//                                      <media><sequence>...</sequence></media> (nests) <media><multicam/></media>
//     <library><event><project><sequence format duration tcStart tcFormat audioLayout audioRate><spine>
//       asset-clip | ref-clip | mc-clip | title | gap | transition ..., each with connected clips (lane != 0)
//
// FCPXML has no tracks: a magnetic primary storyline with clips connected above (positive lanes) and below
// (negative lanes). The mapping, which the importer turns back into the same tracks:
//  * the primary storyline (spine) is V1, with gaps between its clips; an A1 clip that is V1's linked partner (same
//    source, range and speed) rides in the same element (a video+audio asset-clip), other A1 clips fill V1's gaps;
//  * V2..Vn are connected clips on lanes 1..n-1, A2..An on lanes -1..-(n-1), anchored to the spine element under
//    their start (an A1 clip under a different V1 clip goes below the other audio lanes);
//  * transitions live in storylines: V1's (with A1's crossfade at the same cut as its audio part).
// Times are exact rational seconds (ticks reduced: "1001/24000s").

#include "Exporters.h"
#include "TimeMath.h"

#include <wabcut/interchange/FileUrl.h>
#include <wabcut/model/Timecode.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <numeric>
#include <pugixml.hpp>
#include <set>
#include <sstream>

namespace wabcut::interchange::detail {

namespace {

/// Ticks as FCPXML rational seconds: "0s", "5s", "1001/24000s".
std::string fcpxTime(Ticks t) {
    if (t.value == 0) {
        return "0s";
    }
    const std::int64_t g = std::gcd(t.value < 0 ? -t.value : t.value, kTicksPerSecond);
    const std::int64_t num = t.value / g;
    const std::int64_t den = kTicksPerSecond / g;
    return den == 1 ? std::to_string(num) + "s" : std::to_string(num) + "/" + std::to_string(den) + "s";
}

std::string number(double v) {
    if (std::isfinite(v) && v == std::floor(v) && std::abs(v) < 1e15) {
        return std::to_string(static_cast<std::int64_t>(v));
    }
    char buffer[40];
    std::snprintf(buffer, sizeof buffer, "%.10g", v);
    return buffer;
}

/// FCP's names for common formats; others are written with their size and frame duration only.
std::string formatName(Rate r, std::int32_t width, std::int32_t height) {
    const Rate n = r.normalized();
    std::string rate;
    if (n == Rate{24000, 1001}) {
        rate = "2398";
    } else if (n == Rate{24, 1}) {
        rate = "24";
    } else if (n == Rate{25, 1}) {
        rate = "25";
    } else if (n == Rate{30000, 1001}) {
        rate = "2997";
    } else if (n == Rate{30, 1}) {
        rate = "30";
    } else if (n == Rate{50, 1}) {
        rate = "50";
    } else if (n == Rate{60000, 1001}) {
        rate = "5994";
    } else if (n == Rate{60, 1}) {
        rate = "60";
    }
    std::string size;
    if (width == 1920 && height == 1080) {
        size = "1080p";
    } else if (width == 1280 && height == 720) {
        size = "720p";
    } else if (width == 3840 && height == 2160) {
        size = "3840x2160p";
    }
    return rate.empty() || size.empty() ? std::string() : "FFVideoFormat" + size + rate;
}

std::string_view roleName(TrackRole role) {
    switch (role) {
    case TrackRole::Dialogue:
        return "dialogue";
    case TrackRole::Music:
        return "music";
    case TrackRole::Effects:
        return "effects";
    case TrackRole::None:
        break;
    }
    return {};
}

class FcpXmlWriter {
public:
    FcpXmlWriter(const IrDocument& doc, const ExportContext& ctx) : m_doc(doc), m_ctx(ctx) {}

    std::string write() {
        pugi::xml_document xml;
        pugi::xml_node root = xml.append_child("fcpxml");
        root.append_attribute("version") = m_ctx.options ? m_ctx.options->fcpxmlVersion.c_str() : "1.10";
        m_resources = root.append_child("resources");
        // Resource ids are handed out as elements need them; formats, assets and effects come first in the file.
        m_formatsNode = m_resources;
        pugi::xml_node library = root.append_child("library");
        pugi::xml_node event = library.append_child("event");
        event.append_attribute("name") = "Wabcut";
        const IrSequence& main = m_doc.sequences.front();
        pugi::xml_node project = event.append_child("project");
        project.append_attribute("name") = main.name.c_str();
        writeSequenceBody(project.append_child("sequence"), 0);
        // Nested and multicam sources: their <media> resources go after the formats, assets and effects.
        for (std::size_t i = 0; i < m_pendingMedia.size(); ++i) {
            writeMediaResource(m_pendingMedia[i]);
        }
        std::ostringstream out;
        out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE fcpxml>\n";
        xml.save(out, "    ", pugi::format_indent | pugi::format_no_declaration, pugi::encoding_utf8);
        return out.str();
    }

private:
    // ---- resources -----------------------------------------------------------------------------------------------

    std::string nextId() { return "r" + std::to_string(++m_nextResource); }

    /// Inserts a resource before the first <media> resource, so formats, assets and effects lead.
    pugi::xml_node addResource(const char* name) {
        if (m_firstMedia) {
            return m_resources.insert_child_before(name, m_firstMedia);
        }
        return m_resources.append_child(name);
    }

    std::string formatId(Rate r, std::int32_t width, std::int32_t height) {
        const auto key = std::make_tuple(r.normalized().num, r.normalized().den, width, height);
        if (const auto it = m_formats.find(key); it != m_formats.end()) {
            return it->second;
        }
        const std::string id = nextId();
        pugi::xml_node f = addResource("format");
        f.append_attribute("id") = id.c_str();
        if (const std::string name = formatName(r, width, height); !name.empty()) {
            f.append_attribute("name") = name.c_str();
        }
        const Rate n = r.normalized();
        // frameDuration is the reciprocal of the rate.
        f.append_attribute("frameDuration") = (std::to_string(n.den) + "/" + std::to_string(n.num) + "s").c_str();
        f.append_attribute("width") = width;
        f.append_attribute("height") = height;
        m_formats.emplace(key, id);
        return id;
    }

    std::string effectId(const std::string& name, const std::string& uid) {
        if (const auto it = m_effects.find(name); it != m_effects.end()) {
            return it->second;
        }
        const std::string id = nextId();
        pugi::xml_node e = addResource("effect");
        e.append_attribute("id") = id.c_str();
        e.append_attribute("name") = name.c_str();
        e.append_attribute("uid") = uid.c_str();
        m_effects.emplace(name, id);
        return id;
    }

    std::string assetId(std::size_t mediaIndex, const IrSequence& seq) {
        if (const auto it = m_assets.find(mediaIndex); it != m_assets.end()) {
            return it->second;
        }
        const IrMedia& m = m_doc.media[mediaIndex];
        std::string format;
        if (m.hasVideo) {
            format = formatId(m.frameRate.value_or(seq.frameRate), m.width > 0 ? m.width : seq.width,
                              m.height > 0 ? m.height : seq.height);
        }
        const std::string id = nextId();
        pugi::xml_node a = addResource("asset");
        a.append_attribute("id") = id.c_str();
        a.append_attribute("name") = m.name.c_str();
        a.append_attribute("start") = "0s";
        a.append_attribute("duration") = fcpxTime(m.stillImage ? Ticks{} : m.duration.value_or(Ticks{})).c_str();
        a.append_attribute("hasVideo") = m.hasVideo ? "1" : "0";
        if (!format.empty()) {
            a.append_attribute("format") = format.c_str();
        }
        const bool audio = !m.audioChannels.empty();
        a.append_attribute("hasAudio") = audio ? "1" : "0";
        if (audio) {
            a.append_attribute("audioSources") = static_cast<int>(m.audioChannels.size());
            a.append_attribute("audioChannels") = std::accumulate(m.audioChannels.begin(), m.audioChannels.end(), 0);
            a.append_attribute("audioRate") = m.sampleRate.num / std::max(m.sampleRate.den, 1);
            const bool uneven = std::adjacent_find(m.audioChannels.begin(), m.audioChannels.end(), std::not_equal_to<>()) !=
                                m.audioChannels.end();
            if (uneven) {
                m_ctx.add(ReportOutcome::Approximated, ReportCategory::Media,
                          "'" + m.name + "' has audio streams of different channel counts, which an asset cannot say",
                          m.location, {});
            }
        }
        pugi::xml_node rep = a.append_child("media-rep");
        rep.append_attribute("kind") = "original-media";
        rep.append_attribute("src") = pathToFileUrl(m.reference.path, FileUrlStyle::Triple).c_str();
        m_ctx.add(m.reference.path.empty() ? ReportOutcome::Approximated : ReportOutcome::Carried, ReportCategory::Media,
                  "'" + m.name + "'" + (m.reference.path.empty() ? " (no file path)" : " (" + m.reference.path + ")"),
                  m.location, {});
        m_assets.emplace(mediaIndex, id);
        return id;
    }

    /// A nested or multicam source's <media> resource id; the resource is written at the end.
    std::string mediaId(std::size_t sequenceIndex) {
        if (const auto it = m_mediaIds.find(sequenceIndex); it != m_mediaIds.end()) {
            return it->second;
        }
        const std::string id = nextId();
        m_mediaIds.emplace(sequenceIndex, id);
        m_pendingMedia.push_back(sequenceIndex);
        return id;
    }

    void writeMediaResource(std::size_t sequenceIndex) {
        const IrSequence& seq = m_doc.sequences[sequenceIndex];
        pugi::xml_node media = m_resources.append_child("media");
        if (!m_firstMedia) {
            m_firstMedia = media;
        }
        media.append_attribute("id") = m_mediaIds.at(sequenceIndex).c_str();
        media.append_attribute("name") = seq.name.c_str();
        if (seq.multicam) {
            writeMulticam(media.append_child("multicam"), sequenceIndex);
        } else {
            writeSequenceBody(media.append_child("sequence"), sequenceIndex);
        }
    }

    // ---- sequences -----------------------------------------------------------------------------------------------

    void sequenceAttributes(pugi::xml_node node, const IrSequence& seq, Ticks duration) {
        node.append_attribute("format") = formatId(seq.frameRate, seq.width, seq.height).c_str();
        node.append_attribute("duration") = fcpxTime(duration).c_str();
        node.append_attribute("tcStart") = fcpxTime(seq.startTimecode.value_or(Ticks{})).c_str();
        node.append_attribute("tcFormat") = seq.dropFrame ? "DF" : "NDF";
        node.append_attribute("audioLayout") = "stereo";
        const std::int32_t hz = seq.audioRate.num / std::max(seq.audioRate.den, 1);
        node.append_attribute("audioRate") = (hz % 1000 == 0 ? std::to_string(hz / 1000) + "k" : number(hz / 1000.0) + "k").c_str();
    }

    /// One element of the primary storyline.
    struct SpineItem {
        Ticks start;
        Ticks duration;
        const IrClip* video = nullptr; ///< a V1 clip (and its A1 partner in `audio`)
        const IrClip* audio = nullptr; ///< an A1 clip, alone or with `video`
        pugi::xml_node node;
        Ticks localStart; ///< the element's own time at its first frame (connected clips are placed in it)
    };

    static bool samePlay(const IrClip& v, const IrClip& a) {
        const bool sameSource = (v.media != kNoMedia && v.media == a.media) || (v.sequence != kNoSequence && v.sequence == a.sequence);
        const bool sameSpeed = v.speed.has_value() == a.speed.has_value() &&
                               (!v.speed || (v.speed->ratio == a.speed->ratio && v.speed->reversed == a.speed->reversed));
        const bool sameRemap = v.remap.has_value() == a.remap.has_value() &&
                               (!v.remap || (v.remap->curve == a.remap->curve && v.remap->curveIn == a.remap->curveIn));
        return sameSource && v.linkGroup >= 0 && v.linkGroup == a.linkGroup && v.start == a.start && v.duration == a.duration &&
               v.sourceIn == a.sourceIn && sameSpeed && sameRemap;
    }

    void writeSequenceBody(pugi::xml_node node, std::size_t index) {
        const IrSequence& seq = m_doc.sequences[index];
        Ticks end = seq.duration.value_or(Ticks{});
        for (const auto* tracks : {&seq.videoTracks, &seq.audioTracks}) {
            for (const IrTrack& t : *tracks) {
                for (const IrClip& c : t.clips) {
                    end = std::max(end, c.start + c.duration);
                }
            }
        }
        for (const IrMarker& m : seq.markers) {
            end = std::max(end, m.marker.start + Ticks{1});
        }
        sequenceAttributes(node, seq, end);
        m_tcStart = seq.startTimecode.value_or(Ticks{}); // top-level spine offsets count from tcStart
        pugi::xml_node spine = node.append_child("spine");

        // The primary storyline: V1, its A1 partners, A1 clips in V1's gaps, gaps.
        const IrTrack* v1 = seq.videoTracks.empty() ? nullptr : &seq.videoTracks.front();
        const IrTrack* a1 = seq.audioTracks.empty() ? nullptr : &seq.audioTracks.front();
        std::vector<SpineItem> items;
        std::set<const IrClip*> placed;
        if (v1) {
            for (const IrClip& v : v1->clips) {
                SpineItem item{v.start, v.duration, &v, nullptr, {}, {}};
                if (a1) {
                    for (const IrClip& a : a1->clips) {
                        if (!placed.contains(&a) && samePlay(v, a)) {
                            item.audio = &a;
                            placed.insert(&a);
                            break;
                        }
                    }
                }
                items.push_back(item);
            }
        }
        std::vector<const IrClip*> a1Connected;
        if (a1) {
            for (const IrClip& a : a1->clips) {
                if (placed.contains(&a)) {
                    continue;
                }
                const bool overlaps = std::any_of(items.begin(), items.end(), [&](const SpineItem& s) {
                    return s.start < a.start + a.duration && a.start < s.start + s.duration;
                });
                if (overlaps) {
                    a1Connected.push_back(&a);
                } else {
                    items.push_back(SpineItem{a.start, a.duration, nullptr, &a, {}, {}});
                }
            }
        }
        std::sort(items.begin(), items.end(), [](const SpineItem& x, const SpineItem& y) { return x.start < y.start; });
        // Gaps between the items and up to the end.
        std::vector<SpineItem> spineItems;
        Ticks cursor{};
        for (const SpineItem& item : items) {
            if (item.start > cursor) {
                spineItems.push_back(SpineItem{cursor, item.start - cursor, nullptr, nullptr, {}, {}});
            }
            spineItems.push_back(item);
            cursor = item.start + item.duration;
        }
        if (end > cursor || spineItems.empty()) {
            spineItems.push_back(SpineItem{cursor, std::max(end - cursor, Ticks{1}), nullptr, nullptr, {}, {}});
        }
        // Transitions of V1 (with A1's at the same cut as their audio), placed after the item they start in.
        std::map<Ticks, const IrTransition*> videoTransitions;
        std::map<Ticks, const IrTransition*> audioTransitions;
        if (v1) {
            for (const IrTransition& t : v1->transitions) {
                videoTransitions.emplace(t.cut, &t);
            }
        }
        if (a1) {
            for (const IrTransition& t : a1->transitions) {
                audioTransitions.emplace(t.cut, &t);
            }
        }
        for (std::size_t i = 0; i < spineItems.size(); ++i) {
            SpineItem& item = spineItems[i];
            // A transition at this item's head that nothing precedes (a fade in at the start of the storyline).
            if (i == 0) {
                if (const auto it = videoTransitions.find(item.start); it != videoTransitions.end() && item.video) {
                    writeTransition(spine, seq, *it->second, audioTransitions, true);
                    videoTransitions.erase(it);
                }
            }
            writeSpineItem(spine, seq, item);
            const Ticks itemEnd = item.start + item.duration;
            if (const auto it = videoTransitions.find(itemEnd); it != videoTransitions.end()) {
                writeTransition(spine, seq, *it->second, audioTransitions, false);
                videoTransitions.erase(it);
            }
        }
        for (const auto& [cut, t] : videoTransitions) {
            m_ctx.add(ReportOutcome::Dropped, ReportCategory::Transition,
                      "'" + t->name + "' has no clip edge at its cut in the primary storyline", t->location, seq.name);
        }
        for (const auto& [cut, t] : audioTransitions) {
            m_ctx.add(ReportOutcome::Dropped, ReportCategory::Transition,
                      "audio crossfade '" + t->name + "' without a video transition at the same cut: FCPXML transitions "
                                                      "belong to the primary storyline's picture and sound together",
                      t->location, seq.name);
        }
        // Connected clips: V2.. above, A2.. below, then A1's clips that share time with a different V1 clip.
        const auto anchorOf = [&](Ticks at) -> SpineItem* {
            for (SpineItem& s : spineItems) {
                if (at >= s.start && at < s.start + s.duration) {
                    return &s;
                }
            }
            return &spineItems.back();
        };
        for (std::size_t k = 1; k < seq.videoTracks.size(); ++k) {
            connectTrack(seq, seq.videoTracks[k], k, static_cast<int>(k), anchorOf);
        }
        for (std::size_t k = 1; k < seq.audioTracks.size(); ++k) {
            connectTrack(seq, seq.audioTracks[k], k, -static_cast<int>(k), anchorOf);
        }
        for (const IrClip* c : a1Connected) {
            const int lane = -static_cast<int>(std::max<std::size_t>(seq.audioTracks.size(), 1));
            SpineItem* anchor = anchorOf(c->start);
            writeClip(anchor->node, seq, *c, nullptr, TrackKind::Audio, lane, anchor->localStart + (c->start - anchor->start));
            m_ctx.add(ReportOutcome::Approximated, ReportCategory::Track,
                      "A1 clip '" + c->name + "' shares time with a different V1 clip: written as a connected clip, so it "
                                              "comes back on an audio track of its own",
                      c->location, seq.name);
        }
        for (const IrTrack* t : {v1, a1}) {
            if (t) {
                reportTrack(seq, *t, 0);
            }
        }
        // Sequence markers on the storyline element under them.
        for (const IrMarker& m : seq.markers) {
            SpineItem* anchor = anchorOf(m.marker.start);
            writeMarker(anchor->node, m, anchor->localStart + (m.marker.start - anchor->start), seq);
        }
    }

    void reportTrack(const IrSequence& seq, const IrTrack& t, std::size_t index) {
        const std::string label = (t.kind == TrackKind::Video ? "V" : "A") + std::to_string(index + 1);
        std::vector<std::string> notes;
        if (!t.enabled || t.muted) {
            notes.push_back(t.muted ? "muted" : "output off");
        }
        if (t.locked) {
            notes.push_back("locked");
        }
        if (!t.name.empty()) {
            notes.push_back("named '" + t.name + "'");
        }
        std::string reason = label;
        if (!notes.empty()) {
            reason += ": FCPXML has no tracks; its state (";
            for (std::size_t i = 0; i < notes.size(); ++i) {
                reason += (i ? ", " : "") + notes[i];
            }
            reason += ") is not carried";
        }
        m_ctx.add(notes.empty() ? ReportOutcome::Carried : ReportOutcome::Approximated, ReportCategory::Track, reason, t.location,
                  seq.name);
    }

    template <class AnchorOf>
    void connectTrack(const IrSequence& seq, const IrTrack& track, std::size_t index, int lane, AnchorOf anchorOf) {
        reportTrack(seq, track, index);
        for (const IrClip& c : track.clips) {
            SpineItem* anchor = anchorOf(c.start);
            writeClip(anchor->node, seq, c, nullptr, track.kind, lane, anchor->localStart + (c.start - anchor->start));
            if (c.linkGroup >= 0) {
                m_ctx.add(ReportOutcome::Approximated, ReportCategory::Clip,
                          "the link of '" + c.name + "' on " + (track.kind == TrackKind::Video ? "V" : "A") +
                              std::to_string(index + 1) +
                              " is not carried: FCPXML keeps a clip's sound with its picture only in the primary storyline",
                          c.location, seq.name);
            }
        }
        for (const IrTransition& t : track.transitions) {
            m_ctx.add(ReportOutcome::Dropped, ReportCategory::Transition,
                      "'" + t.name + "' on a connected clip: FCPXML transitions belong to storylines", t.location, seq.name);
        }
    }

    void writeSpineItem(pugi::xml_node spine, const IrSequence& seq, SpineItem& item) {
        if (!item.video && !item.audio) {
            pugi::xml_node gap = spine.append_child("gap");
            gap.append_attribute("name") = "Gap";
            gap.append_attribute("offset") = fcpxTime(m_tcStart + item.start).c_str();
            gap.append_attribute("duration") = fcpxTime(item.duration).c_str();
            item.node = gap;
            item.localStart = Ticks{};
            return;
        }
        const IrClip& main = item.video ? *item.video : *item.audio;
        const TrackKind kind = item.video ? TrackKind::Video : TrackKind::Audio;
        item.node = writeClip(spine, seq, main, item.video ? item.audio : nullptr, kind, 0, m_tcStart + item.start);
        item.localStart = m_localStart;
    }

    // ---- clips ---------------------------------------------------------------------------------------------------

    /// Writes a clip (and its A1 partner) as one element at `offset` in the parent's time; `lane` 0 in a storyline.
    /// m_localStart is the element's own time at its first frame afterwards.
    pugi::xml_node writeClip(pugi::xml_node parent, const IrSequence& seq, const IrClip& c, const IrClip* partner,
                             TrackKind kind, int lane, Ticks offset) {
        std::vector<std::string> notes;
        pugi::xml_node node;
        const bool both = partner != nullptr;
        const char* enable = both ? nullptr : (kind == TrackKind::Video ? "video" : "audio");
        Ticks localStart = c.sourceIn;
        if (c.title) {
            node = parent.append_child("title");
            node.append_attribute("ref") =
                effectId("Basic Title",
                         ".../Titles.localized/Bumper:Opener.localized/Basic Title.localized/Basic Title.moti").c_str();
            localStart = Ticks{};
        } else if (c.sequence != kNoSequence) {
            const IrSequence& nested = m_doc.sequences[c.sequence];
            node = parent.append_child(nested.multicam ? "mc-clip" : "ref-clip");
            node.append_attribute("ref") = mediaId(c.sequence).c_str();
            localStart = c.sourceIn + nested.startTimecode.value_or(Ticks{});
            if (c.speed || c.remap) {
                notes.push_back("it plays its nested sequence at another speed, which is not carried");
            }
        } else {
            node = parent.append_child("asset-clip");
            node.append_attribute("ref") = assetId(c.media, seq).c_str();
        }
        if (lane != 0) {
            node.append_attribute("lane") = lane;
        }
        node.append_attribute("offset") = fcpxTime(offset).c_str();
        node.append_attribute("name") = c.name.c_str();
        if (c.media != kNoMedia && !c.title) {
            localStart = speedStart(c);
        }
        node.append_attribute("start") = fcpxTime(localStart).c_str();
        node.append_attribute("duration") = fcpxTime(c.duration).c_str();
        if (!c.enabled) {
            node.append_attribute("enabled") = "0";
        }
        if (enable && !c.title) {
            node.append_attribute("srcEnable") = enable;
        }
        if (c.media != kNoMedia && kind == TrackKind::Audio && !both) {
            // The track's role names the sound (dialogue, music, effects).
            if (const IrTrack* t = trackOf(seq, c); t && !roleName(t->role).empty()) {
                node.append_attribute("audioRole") = std::string(roleName(t->role)).c_str();
            }
        }
        if (c.title) {
            writeTitle(node, *c.title, notes);
        } else if (c.sequence != kNoSequence && m_doc.sequences[c.sequence].multicam) {
            writeMulticamSources(node, c, partner, kind);
        }
        if (c.media != kNoMedia) {
            writeTimeMap(node, c, seq, notes);
        }
        writeAdjustments(node, c, kind, seq, notes);
        if (partner) {
            writeAdjustments(node, *partner, TrackKind::Audio, seq, notes);
        }
        const IrClip& audioClip = partner ? *partner : c;
        if (partner && (partner->name != c.name || partner->enabled != c.enabled)) {
            notes.push_back("its sound's own name or on/off state is not carried (one element: the picture's)");
        }
        if ((partner || kind == TrackKind::Audio) && c.media != kNoMedia) {
            writeChannels(node, audioClip, notes);
        }
        for (const IrClip* x : {&c, partner}) {
            if (x && !x->effects.empty()) {
                std::string list;
                for (const EffectInstance& e : x->effects) {
                    list += (list.empty() ? "" : ", ") + e.effect;
                }
                notes.push_back("its effects are not written yet: " + list);
            }
        }
        std::string reason = std::string(c.title ? "title" : c.sequence != kNoSequence ? "nested clip" : "clip") + " '" + c.name + "'" +
                             (partner ? " with its sound" : "");
        for (const std::string& n : notes) {
            reason += "; " + n;
        }
        m_ctx.add(notes.empty() ? ReportOutcome::Carried : ReportOutcome::Approximated, ReportCategory::Clip, reason, c.location,
                  seq.name);
        m_localStart = localStart;
        return node;
    }

    const IrTrack* trackOf(const IrSequence& seq, const IrClip& c) const {
        for (const auto* tracks : {&seq.videoTracks, &seq.audioTracks}) {
            for (const IrTrack& t : *tracks) {
                if (!t.clips.empty() && &c >= &t.clips.front() && &c <= &t.clips.back()) {
                    return &t;
                }
            }
        }
        return nullptr;
    }

    /// The clip's own time at its first frame: its source In, or (retimed) the time-map time that maps to it.
    static Ticks speedStart(const IrClip& c) {
        if (c.remap) {
            return c.remap->curveIn;
        }
        if (c.speed) {
            return Ticks{}; // the time map starts at 0
        }
        return c.sourceIn;
    }

    void writeTimeMap(pugi::xml_node node, const IrClip& c, const IrSequence& seq, std::vector<std::string>& notes) {
        if (!c.speed && !c.remap) {
            return;
        }
        pugi::xml_node map = node.append_child("timeMap");
        const auto point = [&](Ticks time, Ticks value, const char* interp) {
            pugi::xml_node p = map.append_child("timept");
            p.append_attribute("time") = fcpxTime(time).c_str();
            p.append_attribute("value") = fcpxTime(value).c_str();
            p.append_attribute("interp") = interp;
        };
        if (c.remap) {
            bool eased = false;
            for (const Keyframe& k : c.remap->curve.keys) {
                const bool linear = k.interpolation == Interpolation::Linear;
                eased = eased || !linear;
                point(k.time, Ticks{static_cast<std::int64_t>(std::llround(k.value))}, linear ? "linear" : "smooth2");
            }
            if (eased) {
                notes.push_back("its eased time remap keys were written as smooth time points");
            }
            m_ctx.add(ReportOutcome::Carried, ReportCategory::Speed,
                      "'" + c.name + "' variable speed (" + std::to_string(c.remap->curve.keys.size()) + " keys)", c.location,
                      seq.name);
            return;
        }
        const IrSpeed& s = *c.speed;
        if (s.ratio.num == 0) {
            point(Ticks{}, c.sourceIn, "linear");
            point(c.duration, c.sourceIn, "linear");
            m_ctx.add(ReportOutcome::Carried, ReportCategory::Speed, "'" + c.name + "' freeze frame", c.location, seq.name);
            return;
        }
        // Whole multiples of the speed's denominator keep the ratio exact: n/d media ticks per timeline tick.
        const std::int64_t n = s.ratio.num;
        const std::int64_t d = s.ratio.den;
        std::int64_t m = (c.duration.value + d - 1) / d;
        const Ticks span = toSourceTicks(c.duration, s.ratio);
        if (s.reversed) {
            const Ticks top = c.sourceIn + span; // the media at the clip's first frame
            m = std::min<std::int64_t>(m, n > 0 ? top.value / n : 0);
            if (m <= 0) {
                notes.push_back("its reverse speed could not be written; it plays forwards");
                map.parent().remove_child(map);
                return;
            }
            point(Ticks{}, top, "linear");
            point(Ticks{d * m}, top - Ticks{n * m}, "linear");
        } else {
            point(Ticks{}, c.sourceIn, "linear");
            point(Ticks{d * m}, c.sourceIn + Ticks{n * m}, "linear");
        }
        m_ctx.add(ReportOutcome::Carried, ReportCategory::Speed,
                  "'" + c.name + "' " + speedLabel(s.ratio) + (s.reversed ? " reversed" : ""), c.location, seq.name);
    }

    void writeAdjustments(pugi::xml_node node, const IrClip& c, TrackKind kind, const IrSequence& seq,
                          std::vector<std::string>& notes) {
        if (!c.properties) {
            return;
        }
        const ClipProperties& p = *c.properties;
        const ClipProperties defaults;
        const auto differs = [&](ParamId id) { return !(p[id] == defaults[id]); };
        const auto keyed = [&](ParamId id) { return p[id].isCurve(); };
        // Keyframes of a parameter as <param><keyframeAnimation> in the clip's own time (its media time).
        const auto animate = [&](pugi::xml_node adjust, const char* name, ParamId id, const auto& format) {
            const Curve* curve = p[id].curve();
            if (!curve) {
                return;
            }
            pugi::xml_node param = adjust.append_child("param");
            param.append_attribute("name") = name;
            pugi::xml_node animation = param.append_child("keyframeAnimation");
            for (const Keyframe& k : curve->keys) {
                pugi::xml_node key = animation.append_child("keyframe");
                key.append_attribute("time") = fcpxTime(k.time).c_str();
                key.append_attribute("value") = format(k.value).c_str();
                if (k.interpolation != Interpolation::Linear) {
                    key.append_attribute("interp") = k.interpolation == Interpolation::Hold ? "hold" : "easeInOut";
                }
            }
        };
        if (kind == TrackKind::Video) {
            const bool transform = differs(ParamId::Scale) || differs(ParamId::Rotation) || differs(ParamId::PositionX) ||
                                   differs(ParamId::PositionY) || differs(ParamId::AnchorX) || differs(ParamId::AnchorY);
            if (transform) {
                pugi::xml_node t = node.append_child("adjust-transform");
                // FCP measures position and anchor in percent of the frame height, y up; rotation counter-clockwise.
                const double h = std::max(seq.height, 1);
                const auto pct = [h](double px) { return px / h * 100.0; };
                t.append_attribute("position") =
                    (number(pct(p[ParamId::PositionX].constant())) + " " + number(-pct(p[ParamId::PositionY].constant()))).c_str();
                t.append_attribute("scale") = (number(p[ParamId::Scale].constant()) + " " + number(p[ParamId::Scale].constant())).c_str();
                t.append_attribute("rotation") = number(-p[ParamId::Rotation].constant()).c_str();
                t.append_attribute("anchor") =
                    (number(pct(p[ParamId::AnchorX].constant())) + " " + number(-pct(p[ParamId::AnchorY].constant()))).c_str();
                animate(t, "scale", ParamId::Scale, [](double v) { return number(v) + " " + number(v); });
                animate(t, "rotation", ParamId::Rotation, [](double v) { return number(-v); });
                if (keyed(ParamId::PositionX) || keyed(ParamId::PositionY) || keyed(ParamId::AnchorX) || keyed(ParamId::AnchorY)) {
                    notes.push_back("position and anchor keyframes became their first key's value");
                }
            }
            if (differs(ParamId::Opacity)) {
                pugi::xml_node b = node.append_child("adjust-blend");
                b.append_attribute("amount") = number(p[ParamId::Opacity].constant()).c_str();
                animate(b, "amount", ParamId::Opacity, [](double v) { return number(v); });
            }
        } else {
            if (differs(ParamId::VolumeDb)) {
                pugi::xml_node v = node.append_child("adjust-volume");
                v.append_attribute("amount") = (number(p[ParamId::VolumeDb].constant()) + "dB").c_str();
                animate(v, "amount", ParamId::VolumeDb, [](double x) { return number(x) + "dB"; });
            }
            if (differs(ParamId::Pan)) {
                notes.push_back("its pan is not written (FCPXML pans by role)");
            }
        }
    }

    void writeChannels(pugi::xml_node node, const IrClip& c, std::vector<std::string>& notes) {
        const IrMedia& m = m_doc.media[c.media];
        std::int32_t base = 0;
        for (std::size_t s = 0; s < m.audioChannels.size() && s < static_cast<std::size_t>(c.stream.index); ++s) {
            base += m.audioChannels[s];
        }
        const auto list = [](const std::vector<std::int32_t>& channels) {
            std::string out;
            for (const std::int32_t ch : channels) {
                out += (out.empty() ? "" : ", ") + std::to_string(ch + 1);
            }
            return out;
        };
        if (!c.channels) {
            if (c.stream.index == 0) {
                return;
            }
            // Another stream of the file: its channels, counted across the file's streams.
            const auto s = static_cast<std::size_t>(c.stream.index);
            const std::int32_t count = s < m.audioChannels.size() ? m.audioChannels[s] : 1;
            std::vector<std::int32_t> channels;
            for (std::int32_t k = 0; k < count; ++k) {
                channels.push_back(base + k);
            }
            pugi::xml_node source = node.append_child("audio-channel-source");
            source.append_attribute("srcCh") = list(channels).c_str();
            notes.push_back("it plays audio stream " + std::to_string(c.stream.index + 1) +
                            ", written as that stream's channels of the file");
            return;
        }
        const AudioChannelMap& map = *c.channels;
        std::vector<std::int32_t> sources;
        bool simple = true;
        for (const ChannelRoute& r : map.channels) {
            simple = simple && r.sources.size() == 1 && !r.muted && r.gainDb.isConstant() && r.gainDb.constant() == 0.0;
            sources.push_back(base + (r.sources.empty() ? 0 : r.sources.front()));
        }
        pugi::xml_node source = node.append_child("audio-channel-source");
        source.append_attribute("srcCh") = list(sources).c_str();
        if (map.format == ChannelFormat::Stereo && sources.size() == 2) {
            source.append_attribute("outCh") = "L, R";
        } else if (map.format == ChannelFormat::Surround51 && sources.size() == 6) {
            source.append_attribute("outCh") = "L, R, C, LFE, Ls, Rs";
        }
        if (!simple) {
            notes.push_back("its channel map's mixes, mutes or gains are not carried");
        }
    }

    void writeMulticamSources(pugi::xml_node node, const IrClip& c, const IrClip* partner, TrackKind kind) {
        std::map<std::size_t, std::pair<bool, bool>> angles; // index -> (video, audio)
        for (const std::size_t a : c.angles) {
            (kind == TrackKind::Video ? angles[a].first : angles[a].second) = true;
        }
        if (partner) {
            for (const std::size_t a : partner->angles) {
                angles[a].second = true;
            }
        }
        for (const auto& [index, use] : angles) {
            pugi::xml_node s = node.append_child("mc-source");
            s.append_attribute("angleID") = ("angle-" + std::to_string(index + 1)).c_str();
            s.append_attribute("srcEnable") = use.first && use.second ? "all" : use.first ? "video" : "audio";
        }
    }

    void writeMulticam(pugi::xml_node node, std::size_t index) {
        const IrSequence& seq = m_doc.sequences[index];
        node.append_attribute("format") = formatId(seq.frameRate, seq.width, seq.height).c_str();
        const Ticks tcStart = seq.startTimecode.value_or(Ticks{});
        node.append_attribute("tcStart") = fcpxTime(tcStart).c_str();
        node.append_attribute("tcFormat") = seq.dropFrame ? "DF" : "NDF";
        for (std::size_t a = 0; a < seq.multicam->angles.size(); ++a) {
            const IrMulticamAngle& angle = seq.multicam->angles[a];
            pugi::xml_node an = node.append_child("mc-angle");
            an.append_attribute("name") = angle.name.c_str();
            an.append_attribute("angleID") = ("angle-" + std::to_string(a + 1)).c_str();
            // The angle as a storyline: its video clips with the matching sound of its first audio track, gaps between.
            const IrTrack* video = angle.videoTrack && *angle.videoTrack < seq.videoTracks.size() ? &seq.videoTracks[*angle.videoTrack] : nullptr;
            const IrTrack* audio = !angle.audioTracks.empty() && angle.audioTracks.front() < seq.audioTracks.size()
                                       ? &seq.audioTracks[angle.audioTracks.front()]
                                       : nullptr;
            std::vector<std::pair<const IrClip*, const IrClip*>> items;
            std::set<const IrClip*> used;
            if (video) {
                for (const IrClip& v : video->clips) {
                    const IrClip* sound = nullptr;
                    if (audio) {
                        for (const IrClip& s : audio->clips) {
                            if (!used.contains(&s) && s.media == v.media && s.start == v.start && s.duration == v.duration &&
                                s.sourceIn == v.sourceIn) {
                                sound = &s;
                                used.insert(&s);
                                break;
                            }
                        }
                    }
                    items.emplace_back(&v, sound);
                }
            }
            if (audio) {
                for (const IrClip& s : audio->clips) {
                    if (!used.contains(&s)) {
                        items.emplace_back(nullptr, &s);
                    }
                }
            }
            std::sort(items.begin(), items.end(), [](const auto& x, const auto& y) {
                return (x.first ? x.first->start : x.second->start) < (y.first ? y.first->start : y.second->start);
            });
            Ticks cursor{};
            for (const auto& [v, s] : items) {
                const IrClip& c = v ? *v : *s;
                if (c.start < cursor) {
                    m_ctx.add(ReportOutcome::Dropped, ReportCategory::Clip,
                              "multicam angle clip '" + c.name + "' overlaps another in angle '" + angle.name + "'", c.location, seq.name);
                    continue;
                }
                if (c.start > cursor) {
                    pugi::xml_node gap = an.append_child("gap");
                    gap.append_attribute("name") = "Gap";
                    gap.append_attribute("offset") = fcpxTime(tcStart + cursor).c_str();
                    gap.append_attribute("duration") = fcpxTime(c.start - cursor).c_str();
                }
                pugi::xml_node clip = an.append_child("asset-clip");
                clip.append_attribute("ref") = assetId(c.media, seq).c_str();
                clip.append_attribute("offset") = fcpxTime(tcStart + c.start).c_str();
                clip.append_attribute("name") = c.name.c_str();
                clip.append_attribute("start") = fcpxTime(c.sourceIn).c_str();
                clip.append_attribute("duration") = fcpxTime(c.duration).c_str();
                if (!v || !s) {
                    clip.append_attribute("srcEnable") = v ? "video" : "audio";
                }
                cursor = c.start + c.duration;
            }
            for (std::size_t t = 1; t < angle.audioTracks.size(); ++t) {
                m_ctx.add(ReportOutcome::Dropped, ReportCategory::Track,
                          "angle '" + angle.name + "' of multicam '" + seq.name + "' has more than one audio track; FCPXML angles "
                                                                                 "carry one",
                          seq.location, seq.name);
            }
        }
        m_ctx.add(ReportOutcome::Carried, ReportCategory::SequenceSetting,
                  "multicam '" + seq.name + "' (" + std::to_string(seq.multicam->angles.size()) + " angles)", seq.location, seq.name);
    }

    void writeTitle(pugi::xml_node node, const IrTitle& t, std::vector<std::string>& notes) {
        const auto style = [&](const char* key) -> std::optional<double> {
            for (const EffectParam& p : t.style) {
                if (p.key == key) {
                    return p.value.constant();
                }
            }
            return std::nullopt;
        };
        const std::string styleId = "ts" + std::to_string(++m_nextTextStyle);
        pugi::xml_node text = node.append_child("text");
        pugi::xml_node run = text.append_child("text-style");
        run.append_attribute("ref") = styleId.c_str();
        run.text().set(t.text.c_str());
        pugi::xml_node def = node.append_child("text-style-def");
        def.append_attribute("id") = styleId.c_str();
        pugi::xml_node ts = def.append_child("text-style");
        ts.append_attribute("font") = (t.fontFamily.empty() ? std::string("Helvetica") : t.fontFamily).c_str();
        ts.append_attribute("fontSize") = number(style("size").value_or(63.0)).c_str();
        ts.append_attribute("fontFace") = (t.fontStyle.empty() ? std::string("Regular") : t.fontStyle).c_str();
        ts.append_attribute("fontColor") = (number(style("fill.red").value_or(1.0)) + " " + number(style("fill.green").value_or(1.0)) +
                                            " " + number(style("fill.blue").value_or(1.0)) + " " +
                                            number(style("fill.alpha").value_or(1.0)))
                                               .c_str();
        const auto align = std::llround(style("alignment").value_or(1.0));
        ts.append_attribute("alignment") = align <= 0 ? "left" : align >= 2 ? "right" : "center";
        std::vector<std::string> dropped;
        for (const EffectParam& p : t.style) {
            if (p.key != "size" && p.key != "alignment" && !p.key.starts_with("fill.")) {
                dropped.push_back(p.key);
            }
        }
        if (!dropped.empty()) {
            std::string list;
            for (const std::string& d : dropped) {
                list += (list.empty() ? "" : ", ") + d;
            }
            notes.push_back("title styling not written: " + list);
        }
        if (!t.approximation.empty()) {
            notes.push_back(t.approximation);
        }
    }

    // ---- transitions and markers ---------------------------------------------------------------------------------

    void writeTransition(pugi::xml_node spine, const IrSequence& seq, const IrTransition& t,
                         std::map<Ticks, const IrTransition*>& audioTransitions, bool head) {
        pugi::xml_node node = spine.append_child("transition");
        std::string name = "Cross Dissolve";
        std::string uid = "FxPlug:4731E73A-8DAC-4113-9A30-AE85B1761265";
        std::string note;
        switch (t.kind) {
        case TransitionKind::DipToBlack:
            name = "Fade To Black";
            uid = ".../Transitions.localized/Dissolves.localized/Fade To Color.localized/Fade To Color.motr";
            break;
        case TransitionKind::DipToWhite:
            name = "Fade To White";
            uid = ".../Transitions.localized/Dissolves.localized/Fade To Color.localized/Fade To Color.motr";
            break;
        case TransitionKind::Wipe:
            name = "Wipe";
            uid = ".../Transitions.localized/Wipes.localized/Wipe.localized/Wipe.motr";
            break;
        case TransitionKind::Dissolve:
        case TransitionKind::AudioCrossfade:
            break;
        case TransitionKind::Other:
            note = "'" + t.name + "' has no FCPXML equivalent; written as a cross dissolve";
            break;
        }
        node.append_attribute("name") = name.c_str();
        node.append_attribute("offset") = fcpxTime(m_tcStart + t.cut - t.before).c_str();
        node.append_attribute("duration") = fcpxTime(t.before + t.after).c_str();
        pugi::xml_node video = node.append_child("filter-video");
        video.append_attribute("ref") = effectId(name, uid).c_str();
        video.append_attribute("name") = name.c_str();
        if (const auto it = audioTransitions.find(t.cut); it != audioTransitions.end()) {
            if (it->second->before == t.before && it->second->after == t.after) {
                pugi::xml_node audio = node.append_child("filter-audio");
                audio.append_attribute("ref") = effectId("Audio Crossfade", "FFAudioTransition").c_str();
                audio.append_attribute("name") = "Audio Crossfade";
                audioTransitions.erase(it);
                m_ctx.add(ReportOutcome::Carried, ReportCategory::Transition, "audio crossfade at the cut of '" + name + "'",
                          t.location, seq.name);
            }
        }
        (void)head;
        m_ctx.add(note.empty() ? ReportOutcome::Carried : ReportOutcome::Approximated, ReportCategory::Transition,
                  name + (note.empty() ? std::string() : ": " + note), t.location, seq.name);
    }

    void writeMarker(pugi::xml_node parent, const IrMarker& m, Ticks local, const IrSequence& seq) {
        // FCP's colours: chapter markers orange; to-dos red (open) or green (done); other markers blue.
        const std::int32_t color = m.marker.colorIndex;
        const bool chapter = color == 3;
        pugi::xml_node node = parent.append_child(chapter ? "chapter-marker" : "marker");
        node.append_attribute("start") = fcpxTime(local).c_str();
        node.append_attribute("duration") = fcpxTime(m.marker.duration.value > 0 ? m.marker.duration : Ticks{}).c_str();
        node.append_attribute("value") = m.marker.name.c_str();
        if (const std::string note = markerCommentForExport(m.marker); !note.empty()) {
            node.append_attribute("note") = note.c_str();
        }
        if (color == 1 || color == 0) {
            node.append_attribute("completed") = color == 0 ? "1" : "0";
        }
        if (chapter) {
            node.append_attribute("posterOffset") = "0s";
        }
        const bool exactColor = color == 0 || color == 1 || color == 3 || color == 6;
        m_ctx.add(exactColor ? ReportOutcome::Carried : ReportOutcome::Approximated, ReportCategory::Marker,
                  "marker '" + m.marker.name + "'" + (exactColor ? "" : " (FCP has no such marker colour; it comes back blue)"),
                  m.location, seq.name);
    }

    const IrDocument& m_doc;
    const ExportContext& m_ctx;
    pugi::xml_node m_resources;
    pugi::xml_node m_formatsNode;
    pugi::xml_node m_firstMedia;
    int m_nextResource = 0;
    int m_nextTextStyle = 0;
    std::map<std::tuple<std::int32_t, std::int32_t, std::int32_t, std::int32_t>, std::string> m_formats;
    std::map<std::string, std::string> m_effects;
    std::map<std::size_t, std::string> m_assets;
    std::map<std::size_t, std::string> m_mediaIds;
    std::vector<std::size_t> m_pendingMedia;
    Ticks m_localStart;
    Ticks m_tcStart; ///< the tcStart of the sequence being written
};

} // namespace

std::string writeFcpXml(const IrDocument& document, const ExportContext& context) {
    FcpXmlWriter writer(document, context);
    return writer.write();
}

} // namespace wabcut::interchange::detail
