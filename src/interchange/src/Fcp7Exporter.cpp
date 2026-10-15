// Final Cut Pro 7 XML (xmeml v5) export, written the way Premiere Pro writes it so Premiere and Resolve read it
// best, and so Fcp7Importer.cpp reads it back exactly:
//
//   <xmeml version="5"><sequence id="sequence-1"> <duration/> <rate/> <name/>
//     <media><video><format/> <track MZ.TrackName MZ.TrackTargeted> clipitem | transitionitem | generatoritem ...
//              <enabled/> <locked/></track></video>
//            <audio><format/> <track currentExplodedTrackIndex totalExplodedTrackCount> ... </track></audio></media>
//     <timecode/> <in/> <out/> <marker/>...
//
// Times are whole frames at the sequence's rate; <pproTicksIn>/<pproTicksOut> carry each media clip's exact source
// range in ticks. A clip next to a transition has <start> or <end> -1 and its <in>/<out> include the transition's
// handles (Apple's FCP XML reference). Each media file is defined once (<file id> with its content) and referred to
// by id afterwards; a nested sequence is defined at its first clip. A multicam clip carries its whole <multiclip>
// (each clip has its own active angles). Premiere writes a stereo (5.1) track as consecutive mono tracks: so does
// this, so a stereo clip stays one clip on one track.

#include "EffectMap.h"
#include "Exporters.h"
#include "TimeMath.h"

#include <wabcut/interchange/FileUrl.h>
#include <wabcut/model/Timecode.h>
#include <wabcut/model/TimeRemap.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <pugixml.hpp>
#include <set>
#include <sstream>

namespace wabcut::interchange::detail {

namespace {

constexpr const char* kTrue = "TRUE";
constexpr const char* kFalse = "FALSE";

std::string number(double v) {
    if (std::isfinite(v) && v == std::floor(v) && std::abs(v) < 1e15) {
        return std::to_string(static_cast<std::int64_t>(v));
    }
    char buffer[40];
    std::snprintf(buffer, sizeof buffer, "%.10g", v);
    return buffer;
}

/// Premiere's marker colours in Wabcut's colour order (ImporterCommon.h's palette), as 0xAABBGGRR.
std::uint32_t pproColor(std::int32_t index) {
    static constexpr std::uint32_t kRgb[] = {0x2DC74E, 0xE02F2F, 0xB23CE0, 0xF28A1F,
                                             0xF2D71F, 0xF0F0F0, 0x2F6EE0, 0x2FD0E0};
    const std::uint32_t rgb = kRgb[static_cast<std::size_t>(std::clamp(index, 0, 7))];
    const std::uint32_t r = (rgb >> 16) & 0xFF;
    const std::uint32_t g = (rgb >> 8) & 0xFF;
    const std::uint32_t b = rgb & 0xFF;
    return 0xFF000000u | (b << 16) | (g << 8) | r;
}

/// An FCP7 <rate>: a whole timebase and the NTSC flag. Rates outside that family (12.5 fps) are rounded.
struct Timebase {
    std::int64_t timebase = 30;
    bool ntsc = false;
    bool exact = true;
};

Timebase timebaseOf(Rate rate) {
    const Rate r = rate.normalized();
    if (r.den == 1) {
        return {r.num, false, true};
    }
    if (r.den == 1001 && r.num % 1000 == 0) {
        return {r.num / 1000, true, true};
    }
    return {std::max<std::int64_t>(1, std::llround(static_cast<double>(r.num) / r.den)), false, false};
}

class Fcp7Writer {
public:
    Fcp7Writer(const IrDocument& doc, const ExportContext& ctx) : m_doc(doc), m_ctx(ctx) {}

private:
    // ---- small writers -------------------------------------------------------------------------------------------

    static pugi::xml_node text(pugi::xml_node parent, const char* name, const std::string& value) {
        pugi::xml_node n = parent.append_child(name);
        n.text().set(value.c_str());
        return n;
    }
    static pugi::xml_node integer(pugi::xml_node parent, const char* name, std::int64_t value) {
        return text(parent, name, std::to_string(value));
    }
    static pugi::xml_node boolean(pugi::xml_node parent, const char* name, bool value) {
        return text(parent, name, value ? kTrue : kFalse);
    }

    void rate(pugi::xml_node parent, Rate r, const IrSequence& seq, const SourceLocation& where) {
        const Timebase tb = timebaseOf(r);
        if (!tb.exact && m_roundedRates.insert(r.num * 1000003LL + r.den).second) {
            m_ctx.add(ReportOutcome::Approximated, ReportCategory::SequenceSetting,
                      "the rate " + rateLabel(r) + " fps has no FCP XML timebase; written as " + std::to_string(tb.timebase),
                      where, seq.name);
        }
        pugi::xml_node n = parent.append_child("rate");
        integer(n, "timebase", tb.timebase);
        boolean(n, "ntsc", tb.ntsc);
    }

    /// `t` in whole frames at `r` (nearest); `exact` turns false when that rounds.
    static std::int64_t frames(Ticks t, Rate r, bool& exact) {
        const auto f = ticksToFrames(t, r, Rounding::Nearest);
        if (!f) {
            exact = false;
            return 0;
        }
        const auto back = framesToTicks(*f, r, Rounding::Nearest);
        exact = exact && back && *back == t;
        return *f;
    }

    void timecode(pugi::xml_node parent, Ticks start, Rate r, bool dropFrame, const IrSequence& seq,
                  const SourceLocation& where) {
        pugi::xml_node tc = parent.append_child("timecode");
        rate(tc, r, seq, where);
        text(tc, "string", ticksToTimecode(start, r, dropFrame));
        bool exact = true;
        integer(tc, "frame", frames(start, r, exact));
        text(tc, "displayformat", dropFrame ? "DF" : "NDF");
    }

    // ---- sequences -----------------------------------------------------------------------------------------------

    void writeSequence(pugi::xml_node parent, std::size_t index) {
        const IrSequence& seq = m_doc.sequences[index];
        pugi::xml_node node = parent.append_child("sequence");
        node.append_attribute("id") = seq.key.c_str();
        m_defined.insert(index);
        bool exact = true;
        const Ticks duration = seq.duration.value_or(Ticks{});
        integer(node, "duration", frames(duration, seq.frameRate, exact));
        rate(node, seq.frameRate, seq, seq.location);
        text(node, "name", seq.name);
        pugi::xml_node media = node.append_child("media");
        pugi::xml_node video = media.append_child("video");
        {
            pugi::xml_node sc = video.append_child("format").append_child("samplecharacteristics");
            rate(sc, seq.frameRate, seq, seq.location);
            integer(sc, "width", seq.width);
            integer(sc, "height", seq.height);
            boolean(sc, "anamorphic", false);
            text(sc, "pixelaspectratio", "square");
            text(sc, "fielddominance", "none");
        }
        for (std::size_t i = 0; i < seq.videoTracks.size(); ++i) {
            writeTrack(video, seq, seq.videoTracks[i], i, 0, 1);
        }
        pugi::xml_node audio = media.append_child("audio");
        {
            pugi::xml_node sc = audio.append_child("format").append_child("samplecharacteristics");
            integer(sc, "depth", 16);
            integer(sc, "samplerate", seq.audioRate.num / std::max(seq.audioRate.den, 1));
        }
        for (std::size_t i = 0; i < seq.audioTracks.size(); ++i) {
            const int channels = channelsOf(seq.audioTracks[i]);
            for (int k = 0; k < channels; ++k) {
                writeTrack(audio, seq, seq.audioTracks[i], i, k, channels);
            }
        }
        timecode(node, seq.startTimecode.value_or(Ticks{}), seq.frameRate, seq.dropFrame, seq, seq.location);
        if (seq.markIn) {
            integer(node, "in", frames(*seq.markIn, seq.frameRate, exact));
        }
        if (seq.markOut) {
            integer(node, "out", frames(*seq.markOut, seq.frameRate, exact));
        }
        for (const IrMarker& m : seq.markers) {
            writeMarker(node, m, seq);
        }
        report(exact ? ReportOutcome::Carried : ReportOutcome::Approximated, ReportCategory::SequenceSetting,
               rateLabel(seq.frameRate) + " fps" + (seq.dropFrame ? " drop-frame" : "") + ", " + std::to_string(seq.width) +
                   "x" + std::to_string(seq.height) + (exact ? "" : "; its duration or marks were rounded to whole frames"),
               seq.location, seq);
    }

    /// The markers of a clip's source (Premiere's clip markers), in its media frames; reported once per source.
    void writeMediaMarkers(pugi::xml_node clip, const IrMedia& media, Rate r) {
        for (const IrMarker& m : media.markers) {
            bool exact = true;
            pugi::xml_node node = clip.append_child("marker");
            text(node, "comment", markerCommentForExport(m.marker));
            text(node, "name", m.marker.name);
            const std::int64_t in = frames(m.marker.start, r, exact);
            integer(node, "in", in);
            integer(node, "out", m.marker.duration.value > 0 ? frames(m.marker.start + m.marker.duration, r, exact) : -1);
            integer(node, "pproColor", static_cast<std::int64_t>(pproColor(m.marker.colorIndex)));
            if (m_mediaMarkersReported.insert(&m).second) {
                m_ctx.add(exact ? ReportOutcome::Carried : ReportOutcome::Approximated, ReportCategory::Marker,
                          "clip marker '" + m.marker.name + "' of '" + media.name + "'" + (exact ? "" : " (moved to a whole frame)"),
                          m.location, {});
            }
        }
    }

    void writeMarker(pugi::xml_node parent, const IrMarker& m, const IrSequence& seq) {
        bool exact = true;
        pugi::xml_node node = parent.append_child("marker");
        text(node, "comment", markerCommentForExport(m.marker));
        text(node, "name", m.marker.name);
        const std::int64_t in = frames(m.marker.start, seq.frameRate, exact);
        integer(node, "in", in);
        integer(node, "out", m.marker.duration.value > 0 ? frames(m.marker.start + m.marker.duration, seq.frameRate, exact) : -1);
        integer(node, "pproColor", static_cast<std::int64_t>(pproColor(m.marker.colorIndex)));
        report(exact ? ReportOutcome::Carried : ReportOutcome::Approximated, ReportCategory::Marker,
               "marker '" + m.marker.name + "'" + (exact ? "" : " (moved to a whole frame)"), m.location, seq);
    }

    /// How many mono tracks Premiere writes for an audio track: the most channels any of its clips plays.
    int channelsOf(const IrTrack& track) const {
        int channels = 1;
        for (const IrClip& c : track.clips) {
            channels = std::max(channels, clipChannels(c));
        }
        return std::min(channels, 8);
    }

    int clipChannels(const IrClip& c) const {
        if (c.channels && c.channels->channelCount() == 1) {
            return 1;
        }
        if (c.media == kNoMedia || c.stream.kind != StreamKind::Audio) {
            return c.sequence != kNoSequence ? 2 : 1; // a nested sequence's mix is stereo
        }
        const IrMedia& m = m_doc.media[c.media];
        const auto s = static_cast<std::size_t>(c.stream.index);
        return s < m.audioChannels.size() ? std::clamp(m.audioChannels[s], 1, 8) : 1;
    }

    // ---- tracks --------------------------------------------------------------------------------------------------

    struct Item {
        Ticks start;
        int order; // a transition before the clip it leads into
        const IrClip* clip = nullptr;
        const IrTransition* transition = nullptr;
    };

    void writeTrack(pugi::xml_node parent, const IrSequence& seq, const IrTrack& track, std::size_t index, int exploded,
                    int explodedCount) {
        pugi::xml_node node = parent.append_child("track");
        if (!track.name.empty()) {
            node.append_attribute("MZ.TrackName") = track.name.c_str();
        }
        if (track.targeted) {
            node.append_attribute("MZ.TrackTargeted") = *track.targeted ? 1 : 0;
        }
        if (track.kind == TrackKind::Audio && explodedCount > 1) {
            node.append_attribute("currentExplodedTrackIndex") = exploded;
            node.append_attribute("totalExplodedTrackCount") = explodedCount;
            node.append_attribute("premiereTrackType") = explodedCount == 2 ? "Stereo" : explodedCount == 6 ? "5.1" : "Multichannel";
        }
        std::vector<Item> items;
        for (const IrClip& c : track.clips) {
            items.push_back({c.start, 1, &c, nullptr});
        }
        for (const IrTransition& t : track.transitions) {
            items.push_back({t.cut - t.before, 0, nullptr, &t});
        }
        std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
            return std::tie(a.start, a.order) < std::tie(b.start, b.order);
        });
        for (const Item& item : items) {
            if (item.transition) {
                writeTransition(node, seq, track, exploded, *item.transition);
            } else if (exploded == 0 || exploded < clipChannels(*item.clip)) {
                writeClip(node, seq, track, *item.clip, exploded);
            }
        }
        const bool enabled = track.enabled && !(track.kind == TrackKind::Audio && track.muted);
        boolean(node, "enabled", enabled);
        boolean(node, "locked", track.locked);
        if (track.kind == TrackKind::Audio) {
            integer(node, "outputchannelindex", explodedCount > 1 ? exploded + 1 : 1);
        }
        if (exploded == 0) {
            const std::string label = (track.kind == TrackKind::Video ? "V" : "A") + std::to_string(index + 1);
            if (track.kind == TrackKind::Audio && track.muted && track.enabled) {
                report(ReportOutcome::Approximated, ReportCategory::Track, label + " is muted: written with its output off",
                       track.location, seq);
            } else {
                report(ReportOutcome::Carried, ReportCategory::Track,
                       label + (track.name.empty() ? std::string() : " '" + track.name + "'") +
                           (explodedCount > 1 ? " (" + std::to_string(explodedCount) + " channels, as Premiere writes them)"
                                              : std::string()),
                       track.location, seq);
            }
        }
    }

    /// The transition whose cut is at `t` on the track, on the side that matters.
    static const IrTransition* transitionAt(const IrTrack& track, Ticks t) {
        for (const IrTransition& tr : track.transitions) {
            if (tr.cut == t) {
                return &tr;
            }
        }
        return nullptr;
    }

    /// A transition; on the second (third, ...) channel track of a stereo (5.1) track again, unreported.
    void writeTransition(pugi::xml_node parent, const IrSequence& seq, const IrTrack& track, int exploded,
                         const IrTransition& t) {
        bool exact = true;
        pugi::xml_node node = parent.append_child("transitionitem");
        const std::int64_t start = frames(t.cut - t.before, seq.frameRate, exact);
        const std::int64_t end = frames(t.cut + t.after, seq.frameRate, exact);
        integer(node, "start", start);
        integer(node, "end", end);
        const bool hasOut = std::any_of(track.clips.begin(), track.clips.end(), [&](const IrClip& c) { return c.start + c.duration == t.cut; });
        const bool hasIn = std::any_of(track.clips.begin(), track.clips.end(), [&](const IrClip& c) { return c.start == t.cut; });
        std::string alignment = "center";
        if (!hasIn) {
            alignment = "end-black";
        } else if (!hasOut) {
            alignment = "start-black";
        } else if (t.before.value == 0) {
            alignment = "start";
        } else if (t.after.value == 0) {
            alignment = "end";
        }
        text(node, "alignment", alignment);
        integer(node, "cutPointTicks", t.before.value);
        rate(node, seq.frameRate, seq, t.location);
        pugi::xml_node effect = node.append_child("effect");
        std::string name;
        std::string id;
        std::string category = "Dissolve";
        std::string note;
        switch (t.kind) {
        case TransitionKind::AudioCrossfade:
            name = t.name == "Constant Gain" ? "Cross Fade (0dB)" : "Cross Fade (+3dB)";
            id = t.name == "Constant Gain" ? "KGAudioTransCrossFade0dB" : "KGAudioTransCrossFade3dB";
            category = "";
            break;
        case TransitionKind::DipToBlack:
            name = "Dip to Black";
            id = "DipToColorDissolve";
            break;
        case TransitionKind::DipToWhite:
            name = "Dip to White";
            id = "DipToColorDissolve";
            break;
        case TransitionKind::Wipe:
            name = "Wipe";
            id = "Wipe";
            category = "Wipe";
            break;
        case TransitionKind::Dissolve:
            name = "Cross Dissolve";
            id = "Cross Dissolve";
            break;
        case TransitionKind::Other:
            name = "Cross Dissolve";
            id = "Cross Dissolve";
            note = "'" + t.name + "' has no FCP XML equivalent; written as a cross dissolve";
            break;
        }
        text(effect, "name", name);
        text(effect, "effectid", id);
        if (!category.empty()) {
            text(effect, "effectcategory", category);
        }
        text(effect, "effecttype", "transition");
        text(effect, "mediatype", track.kind == TrackKind::Video ? "video" : "audio");
        if (!exact) {
            note += std::string(note.empty() ? "" : "; ") + "its extent was rounded to whole frames";
        }
        if (exploded == 0) {
            report(note.empty() ? ReportOutcome::Carried : ReportOutcome::Approximated, ReportCategory::Transition,
                   name + (note.empty() ? std::string() : ": " + note), t.location, seq);
        }
    }

    // ---- clips ---------------------------------------------------------------------------------------------------

    std::string clipId(const IrClip& c) {
        const auto [it, inserted] = m_clipIds.emplace(&c, std::string());
        if (inserted) {
            it->second = "clipitem-" + std::to_string(++m_nextClip);
        }
        return it->second;
    }

    void writeClip(pugi::xml_node parent, const IrSequence& seq, const IrTrack& track, const IrClip& c, int exploded) {
        const bool title = c.title.has_value();
        pugi::xml_node node = parent.append_child(title ? "generatoritem" : "clipitem");
        // Exploded audio tracks: each channel is its own clipitem.
        const std::string id = exploded == 0 ? clipId(c) : clipId(c) + "-" + std::to_string(exploded + 1);
        node.append_attribute("id") = id.c_str();
        bool exact = true;
        const Rate r = seq.frameRate;
        // Speed: media frames per clip frame.
        const double factor = c.speed && c.speed->ratio.num > 0 ? static_cast<double>(c.speed->ratio.num) / c.speed->ratio.den : 1.0;
        const IrTransition* head = transitionAt(track, c.start);
        const IrTransition* tail = transitionAt(track, c.start + c.duration);
        const Ticks headHandle = head ? head->before : Ticks{};
        const Ticks tailHandle = tail ? tail->after : Ticks{};
        text(node, "name", c.name);
        boolean(node, "enabled", c.enabled);
        if (c.media != kNoMedia && m_doc.media[c.media].duration && !m_doc.media[c.media].stillImage) {
            integer(node, "duration", frames(*m_doc.media[c.media].duration, r, exact));
        } else if (title) {
            integer(node, "duration", frames(c.duration, r, exact));
        }
        rate(node, r, seq, c.location);
        integer(node, "start", head ? -1 : frames(c.start, r, exact));
        integer(node, "end", tail ? -1 : frames(c.start + c.duration, r, exact));
        // <in>/<out> count the clip's frames at its speed from the media's start (a remapped clip: its curve time);
        // they include the handles of transitions at its ends.
        const Ticks inAt = c.sourceIn - Ticks{std::llround(static_cast<double>(headHandle.value) * factor)};
        std::int64_t in = 0;
        if (c.remap) {
            in = frames(c.remap->curveIn - headHandle, r, exact);
        } else if (c.sequence != kNoSequence || title) {
            in = frames(inAt, r, exact);
        } else {
            in = factor > 0 ? std::llround(static_cast<double>(frames(inAt, r, exact)) / factor) : frames(inAt, r, exact);
        }
        const std::int64_t length = frames(headHandle + c.duration + tailHandle, r, exact);
        integer(node, "in", in);
        integer(node, "out", in + length);
        if (c.media != kNoMedia || c.sequence != kNoSequence) {
            integer(node, "pproTicksIn", inAt.value);
            integer(node, "pproTicksOut",
                    (inAt + Ticks{std::llround(static_cast<double>((headHandle + c.duration + tailHandle).value) * factor)}).value);
        }
        std::vector<std::string> notes;
        if (!exact) {
            notes.push_back("its position or length was rounded to whole frames");
        }
        if (title) {
            writeTitle(node, *c.title, notes);
        } else if (c.sequence != kNoSequence) {
            writeNested(node, c, track.kind, notes);
        } else if (c.media != kNoMedia) {
            writeFileRef(node, c.media, seq, c);
        }
        if (!title) {
            pugi::xml_node st = node.append_child("sourcetrack");
            text(st, "mediatype", track.kind == TrackKind::Video ? "video" : "audio");
            integer(st, "trackindex", sourceTrackIndex(c, exploded));
        }
        if (exploded == 0 && c.media != kNoMedia && !title) {
            writeMediaMarkers(node, m_doc.media[c.media], r);
        }
        if (exploded == 0) {
            writeSpeed(node, c, r, seq, notes);
            writeProperties(node, c, track.kind, seq, notes);
            writeEffects(node, c, track.kind, r, notes);
            if (c.channels && track.kind == TrackKind::Audio && c.channels->channelCount() != 1) {
                notes.push_back("its channel mapping (Modify > Audio Channels) is not carried");
            }
        }
        if (c.linkGroup >= 0) {
            m_linkNodes.push_back({node, &c, exploded, track.kind});
        }
        if (exploded == 0) {
            const std::string what = std::string(title ? "title" : c.sequence != kNoSequence ? (c.angles.empty() ? "nested clip" : "multicam clip") : "clip") +
                                     " '" + c.name + "'";
            std::string reason = what;
            for (const std::string& n : notes) {
                reason += "; " + n;
            }
            report(notes.empty() ? ReportOutcome::Carried : ReportOutcome::Approximated, ReportCategory::Clip, std::move(reason),
                   c.location, seq);
        }
    }

    /// <sourcetrack><trackindex>: the channel of the file across its audio streams (1-based), or 1 for video.
    std::int64_t sourceTrackIndex(const IrClip& c, int exploded) const {
        if (c.stream.kind != StreamKind::Audio || c.media == kNoMedia) {
            return 1 + exploded;
        }
        const IrMedia& m = m_doc.media[c.media];
        std::int64_t before = 0;
        for (std::size_t s = 0; s < m.audioChannels.size() && s < static_cast<std::size_t>(c.stream.index); ++s) {
            before += m.audioChannels[s];
        }
        int channel = exploded;
        if (c.channels && c.channels->channelCount() == 1 && !c.channels->channels.front().sources.empty()) {
            channel = c.channels->channels.front().sources.front();
        }
        return before + channel + 1;
    }

    void writeFileRef(pugi::xml_node parent, std::size_t mediaIndex, const IrSequence& seq, const IrClip& c) {
        const IrMedia& m = m_doc.media[mediaIndex];
        pugi::xml_node file = parent.append_child("file");
        const std::string id = "file-" + std::to_string(mediaIndex + 1);
        file.append_attribute("id") = id.c_str();
        if (!m_filesWritten.insert(mediaIndex).second) {
            return; // defined at its first clip
        }
        text(file, "name", m.name);
        if (!m.reference.path.empty()) {
            text(file, "pathurl", pathToFileUrl(m.reference.path, FileUrlStyle::Localhost));
        }
        const Rate fileRate = m.frameRate.value_or(seq.frameRate);
        rate(file, fileRate, seq, c.location);
        bool exact = true;
        if (m.duration && !m.stillImage) {
            integer(file, "duration", frames(*m.duration, fileRate, exact));
        }
        if (m.startTimecode) {
            timecode(file, *m.startTimecode, fileRate, false, seq, c.location);
        }
        pugi::xml_node media = file.append_child("media");
        if (m.hasVideo) {
            pugi::xml_node sc = media.append_child("video").append_child("samplecharacteristics");
            rate(sc, fileRate, seq, c.location);
            integer(sc, "width", m.width);
            integer(sc, "height", m.height);
        }
        for (const std::int32_t channels : m.audioChannels) {
            pugi::xml_node audio = media.append_child("audio");
            pugi::xml_node sc = audio.append_child("samplecharacteristics");
            integer(sc, "depth", 16);
            integer(sc, "samplerate", m.sampleRate.num / std::max(m.sampleRate.den, 1));
            integer(audio, "channelcount", channels);
        }
        report(m.reference.path.empty() ? ReportOutcome::Approximated : ReportOutcome::Carried, ReportCategory::Media,
               "'" + m.name + "'" + (m.reference.path.empty() ? " (no file path)" : " (" + m.reference.path + ")"), m.location, {});
    }

    void writeNested(pugi::xml_node parent, const IrClip& c, TrackKind kind, std::vector<std::string>& notes) {
        const IrSequence& nested = m_doc.sequences[c.sequence];
        if (nested.multicam) {
            writeMulticlip(parent, c, kind);
            return;
        }
        if (c.speed || c.remap) {
            notes.push_back("it plays its nested sequence at another speed, which FCP XML nests cannot");
        }
        if (m_defined.contains(c.sequence)) {
            pugi::xml_node ref = parent.append_child("sequence");
            ref.append_attribute("id") = nested.key.c_str();
            return;
        }
        writeSequence(parent, c.sequence);
    }

    void writeMulticlip(pugi::xml_node parent, const IrClip& c, TrackKind kind) {
        const IrSequence& source = m_doc.sequences[c.sequence];
        pugi::xml_node mc = parent.append_child("multiclip");
        const std::string id = "multiclip-" + std::to_string(c.sequence + 1);
        mc.append_attribute("id") = id.c_str();
        text(mc, "name", source.name);
        boolean(mc, "collapsed", false);
        int sync = 0;
        switch (source.multicam->sync) {
        case MulticamSync::InPoints:
            sync = 1;
            break;
        case MulticamSync::OutPoints:
            sync = 2;
            break;
        case MulticamSync::Timecode:
            sync = 3;
            break;
        default:
            sync = 0;
            break;
        }
        integer(mc, "synctype", sync);
        const bool first = m_defined.insert(c.sequence).second;
        for (std::size_t a = 0; a < source.multicam->angles.size(); ++a) {
            const IrMulticamAngle& angle = source.multicam->angles[a];
            pugi::xml_node an = mc.append_child("angle");
            pugi::xml_node clip = an.append_child("clip");
            const std::string clipId = id + "-angle-" + std::to_string(a + 1);
            clip.append_attribute("id") = clipId.c_str();
            text(clip, "name", angle.name);
            pugi::xml_node media = clip.append_child("media");
            pugi::xml_node video = media.append_child("video");
            if (angle.videoTrack && *angle.videoTrack < source.videoTracks.size()) {
                writeAngleTrack(video, source, source.videoTracks[*angle.videoTrack], *angle.videoTrack, first);
            }
            pugi::xml_node audio = media.append_child("audio");
            for (const std::size_t t : angle.audioTracks) {
                if (t < source.audioTracks.size()) {
                    writeAngleTrack(audio, source, source.audioTracks[t], t, first);
                }
            }
            const bool active = std::find(c.angles.begin(), c.angles.end(), a) != c.angles.end();
            if (kind == TrackKind::Video) {
                boolean(an, "activevideoangle", active);
            } else {
                boolean(an, "activeaudioangle", active);
            }
        }
    }

    /// A multicam angle's track: its clips without links (the angle's clips belong to the source sequence).
    void writeAngleTrack(pugi::xml_node parent, const IrSequence& source, const IrTrack& track, std::size_t index, bool report) {
        pugi::xml_node node = parent.append_child("track");
        if (!track.name.empty()) {
            node.append_attribute("MZ.TrackName") = track.name.c_str();
        }
        if (track.targeted) {
            node.append_attribute("MZ.TrackTargeted") = *track.targeted ? 1 : 0;
        }
        for (const IrClip& c : track.clips) {
            if (c.title || c.sequence != kNoSequence || c.media == kNoMedia) {
                continue;
            }
            bool exact = true;
            pugi::xml_node clip = node.append_child("clipitem");
            if (report) {
                // The first definition of the multiclip: its clips get ids, so their links can name them.
                clip.append_attribute("id") = clipId(c).c_str();
                if (c.linkGroup >= 0) {
                    m_linkNodes.push_back({clip, &c, 0, track.kind});
                }
            }
            text(clip, "name", c.name);
            boolean(clip, "enabled", c.enabled);
            rate(clip, source.frameRate, source, c.location);
            integer(clip, "start", frames(c.start, source.frameRate, exact));
            integer(clip, "end", frames(c.start + c.duration, source.frameRate, exact));
            const std::int64_t in = frames(c.sourceIn, source.frameRate, exact);
            integer(clip, "in", in);
            integer(clip, "out", in + frames(c.duration, source.frameRate, exact));
            integer(clip, "pproTicksIn", c.sourceIn.value);
            writeFileRef(clip, c.media, source, c);
            pugi::xml_node st = clip.append_child("sourcetrack");
            text(st, "mediatype", track.kind == TrackKind::Video ? "video" : "audio");
            integer(st, "trackindex", sourceTrackIndex(c, 0));
            if (report && !exact) {
                m_ctx.add(ReportOutcome::Approximated, ReportCategory::Clip,
                          "multicam angle clip '" + c.name + "' was rounded to whole frames", c.location, source.name);
            }
        }
        (void)index;
        boolean(node, "enabled", track.enabled);
        boolean(node, "locked", track.locked);
    }

    void writeTitle(pugi::xml_node parent, const IrTitle& t, std::vector<std::string>& notes) {
        const auto style = [&](const char* key) -> std::optional<double> {
            for (const EffectParam& p : t.style) {
                if (p.key == key) {
                    return p.value.constant();
                }
            }
            return std::nullopt;
        };
        const bool outline = style("stroke.width").value_or(0.0) > 0.0;
        boolean(parent, "anamorphic", false);
        text(parent, "alphatype", "black");
        pugi::xml_node effect = parent.append_child("effect");
        text(effect, "name", outline ? "Outline Text" : "Text");
        text(effect, "effectid", outline ? "Outline Text" : "Text");
        text(effect, "effectcategory", "Text");
        text(effect, "effecttype", "generator");
        text(effect, "mediatype", "video");
        const auto parameter = [&](const char* id, const char* name) {
            pugi::xml_node p = effect.append_child("parameter");
            text(p, "parameterid", id);
            text(p, "name", name);
            return p;
        };
        std::string value;
        for (const char ch : t.text) {
            value += ch == '\n' ? '\r' : ch; // FCP7 separates lines with carriage returns
        }
        text(parameter("str", "Text"), "value", value);
        if (!t.fontFamily.empty()) {
            text(parameter("fontname", "Font"), "value", t.fontFamily);
        }
        if (const auto size = style("size")) {
            text(parameter("fontsize", "Size"), "value", number(*size));
        }
        if (!t.fontStyle.empty()) {
            const std::string s = t.fontStyle;
            const int code = s == "Bold" ? 2 : s == "Italic" ? 3 : s == "Bold Italic" ? 4 : 1;
            if (code == 1 && s != "Regular" && s != "Plain") {
                notes.push_back("the font style '" + s + "' was written as plain");
            }
            text(parameter("fontstyle", "Style"), "value", std::to_string(code));
        }
        if (const auto align = style("alignment")) {
            text(parameter("fontalign", "Alignment"), "value", std::to_string(std::clamp<std::int64_t>(std::llround(*align), 0, 2) + 1));
        }
        const auto color = [&](const char* prefix, const char* id, const char* name) {
            const auto r = style((std::string(prefix) + ".red").c_str());
            const auto g = style((std::string(prefix) + ".green").c_str());
            const auto b = style((std::string(prefix) + ".blue").c_str());
            const auto a = style((std::string(prefix) + ".alpha").c_str());
            if (!r && !g && !b && !a) {
                return;
            }
            pugi::xml_node v = parameter(id, name).append_child("value");
            const auto channel = [](std::optional<double> x, double fallback) {
                return std::to_string(std::clamp<std::int64_t>(std::llround(x.value_or(fallback) * 255.0), 0, 255));
            };
            text(v, "alpha", channel(a, 1.0));
            text(v, "red", channel(r, 1.0));
            text(v, "green", channel(g, 1.0));
            text(v, "blue", channel(b, 1.0));
        };
        color("fill", "fontcolor", "Font Color");
        if (outline) {
            text(parameter("linewidth", "Line Width"), "value", number(*style("stroke.width")));
            color("stroke", "linecolor", "Line Color");
        }
        std::vector<std::string> dropped;
        for (const EffectParam& p : t.style) {
            const std::string& k = p.key;
            const bool carried = k == "size" || k == "alignment" || k.starts_with("fill.") ||
                                 (outline && (k == "stroke.width" || k.starts_with("stroke.")));
            if (!carried) {
                dropped.push_back(k);
            }
        }
        if (!dropped.empty()) {
            std::string list;
            for (const std::string& d : dropped) {
                list += (list.empty() ? "" : ", ") + d;
            }
            notes.push_back("title styling FCP XML has no place for was dropped: " + list);
        }
        if (!t.approximation.empty()) {
            notes.push_back(t.approximation);
        }
    }

    // ---- speed, motion, opacity, audio levels --------------------------------------------------------------------

    static pugi::xml_node filterEffect(pugi::xml_node clip, const char* name, const char* id, const char* category,
                                       const char* type, const char* mediaType) {
        pugi::xml_node effect = clip.append_child("filter").append_child("effect");
        text(effect, "name", name);
        text(effect, "effectid", id);
        text(effect, "effectcategory", category);
        text(effect, "effecttype", type);
        text(effect, "mediatype", mediaType);
        return effect;
    }

    void writeSpeed(pugi::xml_node clip, const IrClip& c, Rate r, const IrSequence& seq, std::vector<std::string>& notes) {
        if (!c.speed && !c.remap) {
            return;
        }
        if (c.sequence != kNoSequence) {
            return; // reported with the nest
        }
        pugi::xml_node effect = filterEffect(clip, "Time Remap", "timeremap", "motion", "motion", "video");
        const auto param = [&](const char* id, const std::string& value) {
            pugi::xml_node p = effect.append_child("parameter");
            p.append_attribute("authoringApp") = "PremierePro";
            text(p, "parameterid", id);
            text(p, "name", id);
            text(p, "value", value);
            return p;
        };
        if (c.remap) {
            param("variablespeed", "1");
            param("speed", "100");
            param("reverse", kFalse);
            pugi::xml_node graph = effect.append_child("parameter");
            text(graph, "parameterid", "graphdict");
            text(graph, "name", "Time Graph");
            bool whole = true;
            bool curved = false;
            for (const Keyframe& k : c.remap->curve.keys) {
                pugi::xml_node key = graph.append_child("keyframe");
                integer(key, "when", frames(k.time, r, whole));
                bool valueExact = true;
                integer(key, "value", frames(Ticks{static_cast<std::int64_t>(std::llround(k.value))}, r, valueExact));
                whole = whole && valueExact;
                if (k.interpolation != Interpolation::Linear) {
                    curved = true;
                }
                if (k.interpolation != Interpolation::Linear && k.interpolation != Interpolation::Hold) {
                    // A smooth key (read back as a Bezier key with tangents through its neighbours); a time graph
                    // has no steps, so a hold key stays a corner.
                    pugi::xml_node interp = key.append_child("interpolation");
                    text(interp, "name", "FCPCurve");
                }
            }
            if (!whole) {
                notes.push_back("its time remap keys were moved to whole frames");
            }
            if (curved) {
                notes.push_back("its time remap's eased keys were written as smooth keys (their handles are not kept)");
            }
            m_ctx.add(ReportOutcome::Carried, ReportCategory::Speed,
                      "'" + c.name + "' variable speed (" + std::to_string(c.remap->curve.keys.size()) + " keys)", c.location, seq.name);
            return;
        }
        const IrSpeed& s = *c.speed;
        const double percent = s.ratio.num == 0 ? 0.0 : 100.0 * s.ratio.num / s.ratio.den;
        param("variablespeed", "0");
        param("speed", number(percent));
        param("reverse", s.reversed ? kTrue : kFalse);
        param("frameblending", kFalse);
        m_ctx.add(ReportOutcome::Carried, ReportCategory::Speed,
                  "'" + c.name + "' " + (s.ratio.num == 0 ? std::string("freeze frame") : speedLabel(s.ratio) + (s.reversed ? " reversed" : "")),
                  c.location, seq.name);
    }

    /// A parameter with its value, or its keyframes (`when` in media frames, the frame space of <in>/<out>).
    void parameterValue(pugi::xml_node p, const ParamValue& v, double scale, double offset, Rate r, bool& exact,
                        bool decibels = false) {
        const auto convert = [&](double x) {
            if (decibels) {
                return x <= -96.0 ? 0.0 : std::pow(10.0, x / 20.0);
            }
            return x * scale + offset;
        };
        if (const Curve* curve = v.curve()) {
            for (const Keyframe& k : curve->keys) {
                pugi::xml_node key = p.append_child("keyframe");
                integer(key, "when", frames(k.time, r, exact));
                text(key, "value", number(convert(k.value)));
                if (k.interpolation != Interpolation::Linear) {
                    pugi::xml_node interp = key.append_child("interpolation");
                    text(interp, "name", k.interpolation == Interpolation::Hold ? "FCPHold" : "bezier");
                }
            }
            return;
        }
        text(p, "value", number(convert(v.constant())));
    }

    void writeProperties(pugi::xml_node clip, const IrClip& c, TrackKind kind, const IrSequence& seq,
                         std::vector<std::string>& notes) {
        if (!c.properties) {
            return;
        }
        const ClipProperties& props = *c.properties;
        const ClipProperties defaults;
        const Rate r = seq.frameRate;
        bool exact = true;
        bool eased = false;
        const auto note = [&](const ParamValue& v) {
            if (const Curve* curve = v.curve()) {
                eased = eased || std::any_of(curve->keys.begin(), curve->keys.end(), [](const Keyframe& k) {
                    return k.interpolation != Interpolation::Linear && k.interpolation != Interpolation::Hold;
                });
            }
        };
        const auto param = [&](pugi::xml_node effect, const char* id, const char* name) {
            pugi::xml_node p = effect.append_child("parameter");
            p.append_attribute("authoringApp") = "PremierePro";
            text(p, "parameterid", id);
            text(p, "name", name);
            return p;
        };
        if (kind == TrackKind::Video) {
            const bool motion = !(props[ParamId::Scale] == defaults[ParamId::Scale]) ||
                                !(props[ParamId::Rotation] == defaults[ParamId::Rotation]) ||
                                !(props[ParamId::PositionX] == defaults[ParamId::PositionX]) ||
                                !(props[ParamId::PositionY] == defaults[ParamId::PositionY]) ||
                                !(props[ParamId::AnchorX] == defaults[ParamId::AnchorX]) ||
                                !(props[ParamId::AnchorY] == defaults[ParamId::AnchorY]);
            if (motion) {
                pugi::xml_node effect = filterEffect(clip, "Basic Motion", "basic", "motion", "motion", "video");
                parameterValue(param(effect, "scale", "Scale"), props[ParamId::Scale], 100.0, 0.0, r, exact);
                note(props[ParamId::Scale]);
                parameterValue(param(effect, "rotation", "Rotation"), props[ParamId::Rotation], 1.0, 0.0, r, exact);
                note(props[ParamId::Rotation]);
                // The centre as a fraction of the frame from its middle; anchor likewise (Premiere's normalised points).
                const auto point = [&](const char* id, const char* name, ParamId x, ParamId y, double w, double h) {
                    if (props[x] == defaults[x] && props[y] == defaults[y]) {
                        return;
                    }
                    pugi::xml_node p = param(effect, id, name);
                    if (props[x].isCurve() || props[y].isCurve()) {
                        notes.push_back(std::string("its ") + name + " keyframes became the first key's value");
                    }
                    pugi::xml_node v = p.append_child("value");
                    text(v, "horiz", number(props[x].constant() / w));
                    text(v, "vert", number(props[y].constant() / h));
                };
                point("center", "Center", ParamId::PositionX, ParamId::PositionY, std::max(seq.width, 1), std::max(seq.height, 1));
                point("centerOffset", "Anchor Point", ParamId::AnchorX, ParamId::AnchorY, std::max(seq.width, 1),
                      std::max(seq.height, 1));
            }
            if (!(props[ParamId::Opacity] == defaults[ParamId::Opacity])) {
                pugi::xml_node effect = filterEffect(clip, "Opacity", "opacity", "motion", "motion", "video");
                parameterValue(param(effect, "opacity", "opacity"), props[ParamId::Opacity], 100.0, 0.0, r, exact);
                note(props[ParamId::Opacity]);
            }
        } else {
            if (!(props[ParamId::VolumeDb] == defaults[ParamId::VolumeDb])) {
                pugi::xml_node effect = filterEffect(clip, "Audio Levels", "audiolevels", "audiolevels", "audiolevels", "audio");
                parameterValue(param(effect, "level", "Level"), props[ParamId::VolumeDb], 1.0, 0.0, r, exact, true);
                note(props[ParamId::VolumeDb]);
            }
            if (!(props[ParamId::Pan] == defaults[ParamId::Pan])) {
                pugi::xml_node effect = filterEffect(clip, "Audio Pan", "audiopan", "audiopan", "audiopan", "audio");
                parameterValue(param(effect, "pan", "Pan"), props[ParamId::Pan], 1.0, 0.0, r, exact);
                note(props[ParamId::Pan]);
            }
        }
        if (!exact) {
            notes.push_back("keyframes were moved to whole frames");
        }
        if (eased) {
            notes.push_back("eased keyframes were written as Bezier keys without their handles");
        }
    }

    /// The clip's effect stack as filters: Gaussian Blur, Crop and Drop Shadow as FCP7's own (Premiere and Resolve
    /// apply them), every other effect as a filter named by its Wabcut id that only Wabcut reads (EffectMap.h).
    void writeEffects(pugi::xml_node clip, const IrClip& c, TrackKind kind, Rate r, std::vector<std::string>& notes) {
        std::vector<std::string> ownOnly;
        std::vector<std::string> partial;
        bool exact = true;
        for (const EffectInstance& e : c.effects) {
            pugi::xml_node filter = clip.append_child("filter");
            if (!e.enabled) {
                boolean(filter, "enabled", false);
            }
            pugi::xml_node effect = filter.append_child("effect");
            const EffectDescriptor* descriptor = EffectRegistry::builtin().find(e.effect);
            const auto paramOf = [&](std::string_view key) -> const EffectParam* {
                for (const EffectParam& p : e.params) {
                    if (p.key == key) {
                        return &p;
                    }
                }
                return nullptr;
            };
            const auto defaultOf = [&](std::string_view key) {
                if (descriptor) {
                    for (const EffectParamInfo& info : descriptor->params) {
                        if (info.key == key) {
                            return info.defaultValue;
                        }
                    }
                }
                return 0.0;
            };
            const auto parameter = [&](std::string_view id, std::string_view name) {
                pugi::xml_node p = effect.append_child("parameter");
                p.append_attribute("authoringApp") = "PremierePro";
                text(p, "parameterid", std::string(id));
                text(p, "name", std::string(name));
                return p;
            };
            if (const NativeEffect* native = nativeEffectFor(e.effect)) {
                text(effect, "name", std::string(native->fcpName));
                text(effect, "effectid", std::string(native->fcpId));
                text(effect, "effectcategory", std::string(native->category));
                text(effect, "effecttype", std::string(native->type));
                text(effect, "mediatype", "video");
                for (const NativeParam& np : native->params) {
                    const EffectParam* p = paramOf(np.key);
                    parameterValue(parameter(np.fcpId, np.fcpName), p ? p->value : ParamValue(defaultOf(np.key)), np.scale, 0.0,
                                   r, exact);
                }
                if (native->shadowColor) {
                    pugi::xml_node v = parameter("color", "Color").append_child("value");
                    const auto channel = [&](const char* key) {
                        const EffectParam* p = paramOf(key);
                        if (p && p->value.isCurve()) {
                            partial.push_back(e.effect + " " + key + " keyframes");
                        }
                        const double x = p ? p->value.constant() : defaultOf(key);
                        return std::to_string(std::clamp<std::int64_t>(std::llround(x * 255.0), 0, 255));
                    };
                    text(v, "alpha", "255");
                    text(v, "red", channel("red"));
                    text(v, "green", channel("green"));
                    text(v, "blue", channel("blue"));
                }
                for (const EffectParam& p : e.params) {
                    const bool mapped = std::any_of(native->params.begin(), native->params.end(),
                                                    [&](const NativeParam& np) { return np.key == p.key; }) ||
                                        (native->shadowColor && (p.key == "red" || p.key == "green" || p.key == "blue"));
                    if (!mapped && !(p.value.isConstant() && p.value.constant() == defaultOf(p.key))) {
                        partial.push_back(e.effect + " " + p.key);
                    }
                }
            } else {
                text(effect, "name", descriptor ? std::string(descriptor->displayName) : e.effect);
                text(effect, "effectid", e.effect);
                text(effect, "effectcategory", "Wabcut");
                text(effect, "effecttype", "filter");
                text(effect, "mediatype", kind == TrackKind::Video ? "video" : "audio");
                for (const EffectParam& p : e.params) {
                    parameterValue(parameter(p.key, p.key), p.value, 1.0, 0.0, r, exact);
                }
                ownOnly.push_back(e.effect);
            }
            if (!e.masks.empty() || !e.data.empty()) {
                partial.push_back(e.effect + (e.masks.empty() ? " data" : " masks"));
            }
        }
        const auto list = [](const std::vector<std::string>& items) {
            std::string out;
            for (const std::string& i : items) {
                out += (out.empty() ? "" : ", ") + i;
            }
            return out;
        };
        if (!ownOnly.empty()) {
            notes.push_back("effects only Wabcut reads back (other editors skip them): " + list(ownOnly));
        }
        if (!partial.empty()) {
            notes.push_back("effect settings FCP XML has no place for: " + list(partial));
        }
        if (!exact) {
            notes.push_back("effect keyframes were moved to whole frames");
        }
    }

    // ---- links ---------------------------------------------------------------------------------------------------

    struct LinkNode {
        pugi::xml_node node;
        const IrClip* clip;
        int exploded;
        TrackKind kind;
    };

    /// Every clipitem of a link group (and each channel of an exploded stereo clip) lists the others.
    void writeLinks() {
        std::map<std::int64_t, std::vector<const LinkNode*>> groups;
        for (const LinkNode& l : m_linkNodes) {
            groups[l.clip->linkGroup].push_back(&l);
        }
        for (const LinkNode& l : m_linkNodes) {
            // Premiere lists every member, itself included.
            for (const LinkNode* other : groups[l.clip->linkGroup]) {
                pugi::xml_node owner = l.node;
                pugi::xml_node link = owner.append_child("link");
                text(link, "linkclipref", other->node.attribute("id").value());
                text(link, "mediatype", other->kind == TrackKind::Video ? "video" : "audio");
            }
        }
    }

    void report(ReportOutcome outcome, ReportCategory category, std::string reason, const SourceLocation& where,
                const IrSequence& seq) {
        m_ctx.add(outcome, category, std::move(reason), where, seq.name);
    }

    const IrDocument& m_doc;
    const ExportContext& m_ctx;
    std::set<std::size_t> m_defined;
    std::set<std::size_t> m_filesWritten;
    std::set<std::int64_t> m_roundedRates;
    std::set<const IrMarker*> m_mediaMarkersReported;
    std::map<const IrClip*, std::string> m_clipIds;
    std::vector<LinkNode> m_linkNodes;
    int m_nextClip = 0;

public:
    std::string finish() {
        pugi::xml_document xml;
        pugi::xml_node root = xml.append_child("xmeml");
        root.append_attribute("version") = "5";
        writeSequence(root, 0);
        writeLinks();
        std::ostringstream out;
        out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE xmeml>\n";
        xml.save(out, "\t", pugi::format_indent | pugi::format_no_declaration, pugi::encoding_utf8);
        return out.str();
    }
};

} // namespace

std::string writeFcp7Xml(const IrDocument& document, const ExportContext& context) {
    Fcp7Writer writer(document, context);
    return writer.finish();
}

} // namespace wabcut::interchange::detail
