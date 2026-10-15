// CMX3600 edit decision lists.
//
//   TITLE: My Sequence
//   FCM: DROP FRAME
//   001  A001     V     C        01:00:00:00 01:00:05:00 01:00:00:00 01:00:05:00
//   * FROM CLIP NAME: A001.mov
//   002  A001     V     C        00:00:10:00 00:00:10:00 01:00:05:00 01:00:05:00   <- outgoing side of a
//   dissolve 002  B002     B     D    030 00:00:20:00 00:00:25:00 01:00:05:00 01:00:10:00   <- incoming clip,
//   30-frame dissolve
//   * FROM CLIP NAME: A001.mov
//   * TO CLIP NAME: B002.mov
//   M2   B002       050.0                00:00:20:00                                <- motion effect (speed
//   in fps)
//   * LOC: 01:00:07:00 RED     Marker text                                         <- locator (Avid/Resolve)
//
// Channels: V = V1; A, A2, A3, A4 = audio tracks; AA = A1+A2; B = V+A1; AA/V (or A/V) = V+A1+A2. The EDL's
// one video track becomes V1. Record timecodes are rebased so the hour the edit starts in is 0. EDLs have no
// frame rate (ImportOptions::edlFrameRate) and no media start timecode: source timecodes are kept as
// absolute positions until the media's start timecode is known (see TimelineBuilder).

#include "ImporterCommon.h"
#include "Importers.h"
#include "TimeMath.h"

#include <wabcut/model/Timecode.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

namespace wabcut::interchange::detail {

namespace {

struct EdlLine {
    std::int32_t line = 0;
    std::string reel;
    std::string channel;
    /// Extra audio channels from an Avid "AUD 3 4" line after the event.
    std::vector<int> extraAudio;
    std::string transition; // C, D, Wnnn, K, KB, KO
    std::int64_t transitionFrames = 0;
    std::string srcIn, srcOut, recIn, recOut;
};

struct EdlLocator {
    std::int32_t line = 0;
    std::string timecode;
    std::string color;
    std::string text;
};

struct EdlMotion {
    std::int32_t line = 0;
    std::string reel;
    double fps = 0.0;
};

struct EdlEvent {
    std::string number;
    std::vector<EdlLine> lines;
    std::string fromClip;
    std::string toClip;
    /// "* FROM CLIP:" (Avid) / "* FROM FILE:" (Nucoda): a media path.
    std::string sourcePath;
    /// "* SOURCE FILE:" (Avid): a source name, not a path.
    std::string sourceName;
    std::vector<EdlMotion> motions;
};

bool isDigits(std::string_view s) {
    return !s.empty() && s.find_first_not_of("0123456789") == std::string_view::npos;
}

bool isTimecode(std::string_view w) {
    if (w.size() != 11) {
        return false;
    }
    for (std::size_t i = 0; i < w.size(); ++i) {
        const bool sep = i == 2 || i == 5 || i == 8;
        if (sep ? std::string_view(":;.,").find(w[i]) == std::string_view::npos
                : (w[i] < '0' || w[i] > '9')) {
            return false;
        }
    }
    return true;
}

std::string afterColon(std::string_view comment) {
    const auto colon = comment.find(':');
    return colon == std::string_view::npos ? std::string() : std::string(trim(comment.substr(colon + 1)));
}

bool isBlackReel(std::string_view reel) {
    return iequals(reel, "BL") || iequals(reel, "BLK") || iequals(reel, "BLACK");
}

struct Channels {
    bool video = false;
    std::vector<int> audio; // 1-based audio channel numbers
    bool valid = true;
};

Channels parseChannels(std::string_view field) {
    Channels c;
    const std::string f = toLowerAscii(field);
    if (f == "v") {
        c.video = true;
    } else if (f == "a" || f == "a1") {
        c.audio = {1};
    } else if (f == "aa") {
        c.audio = {1, 2};
    } else if (f == "b" || f == "a/v" || f == "v/a") {
        c.video = true;
        c.audio = {1};
    } else if (f == "aa/v" || f == "v/aa") {
        c.video = true;
        c.audio = {1, 2};
    } else if (f == "none") {
        // no channels
    } else if (f.size() >= 2 && f[0] == 'a' && isDigits(std::string_view(f).substr(1))) {
        const auto n = parseInt64(std::string_view(f).substr(1));
        if (n && *n >= 1 && *n <= 64) {
            c.audio = {static_cast<int>(*n)};
        } else {
            c.valid = false;
        }
    } else {
        c.valid = false;
    }
    return c;
}

class EdlParser {
public:
    EdlParser(const ParseContext& context, IrDocument& doc) : m_ctx(context), m_doc(doc), m_media(doc) {}

    std::expected<void, Error> parse(std::string_view data);

private:
    SourceLocation at(std::int32_t line, const std::string& element) const {
        return {m_ctx.fileLabel, element, line};
    }
    void report(ReportOutcome o, ReportCategory c, std::string reason, SourceLocation location) {
        m_doc.report.add(o, c, std::move(reason), std::move(location), m_seq.name);
    }
    std::optional<Ticks> tc(const std::string& text) const {
        const auto frames = parseTimecodeFrames(text, m_rate, m_dropFrame);
        if (!frames) {
            return std::nullopt;
        }
        return framesToTicksChecked(*frames, m_rate);
    }
    IrTrack& audioTrack(int channel);
    void convert(const EdlEvent& event, Ticks base);
    void mergeDualMono();

    const ParseContext& m_ctx;
    IrDocument& m_doc;
    MediaTable m_media;
    IrSequence m_seq;
    Rate m_rate = kRate24;
    bool m_dropFrame = false;
    std::int64_t m_nextLink = 0;
    std::vector<EdlLocator> m_locators;
    std::vector<EdlEvent> m_events;
};

IrTrack& EdlParser::audioTrack(int channel) {
    while (static_cast<int>(m_seq.audioTracks.size()) < channel) {
        IrTrack t;
        t.kind = TrackKind::Audio;
        t.location = at(0, "A" + std::to_string(m_seq.audioTracks.size() + 1));
        m_seq.audioTracks.push_back(std::move(t));
    }
    return m_seq.audioTracks[static_cast<std::size_t>(channel - 1)];
}

std::expected<void, Error> EdlParser::parse(std::string_view data) {
    if (data.find('\0') != std::string_view::npos) {
        return std::unexpected(parseError("an EDL is text; this file contains binary data"));
    }
    std::optional<bool> fcmDrop;
    bool fcmChanged = false;
    std::int32_t unrecognised = 0;
    std::int32_t lineNumber = 0;
    bool sawTimecodeSemicolon = false;
    for (const std::string_view raw : splitLines(data)) {
        ++lineNumber;
        const std::string_view line = trim(raw);
        if (line.empty()) {
            continue;
        }
        if (istartsWith(line, "TITLE:")) {
            m_seq.name = afterColon(line);
            continue;
        }
        if (istartsWith(line, "FCM:")) {
            const std::string mode = toLowerAscii(afterColon(line));
            const bool drop = mode.find("drop") != std::string::npos && mode.find("non") == std::string::npos;
            if (fcmDrop && *fcmDrop != drop) {
                fcmChanged = true;
            }
            if (!fcmDrop) {
                fcmDrop = drop;
            }
            continue;
        }
        if (line.front() == '*') {
            const std::string_view comment = trim(line.substr(1));
            if (istartsWith(comment, "LOC:")) {
                const auto words = splitWords(trim(comment.substr(4)));
                if (!words.empty()) {
                    EdlLocator loc;
                    loc.line = lineNumber;
                    loc.timecode = std::string(words[0]);
                    std::size_t textStart = 1;
                    if (words.size() > 1) {
                        loc.color = std::string(words[1]);
                        textStart = 2;
                    }
                    for (std::size_t i = textStart; i < words.size(); ++i) {
                        loc.text += (i > textStart ? " " : "") + std::string(words[i]);
                    }
                    m_locators.push_back(std::move(loc));
                }
                continue;
            }
            if (m_events.empty()) {
                continue; // header comment
            }
            EdlEvent& ev = m_events.back();
            if (istartsWith(comment, "FROM CLIP NAME")) {
                ev.fromClip = afterColon(comment);
            } else if (istartsWith(comment, "TO CLIP NAME")) {
                ev.toClip = afterColon(comment);
            } else if (istartsWith(comment, "CLIP NAME")) {
                if (ev.fromClip.empty()) {
                    ev.fromClip = afterColon(comment);
                }
            } else if (istartsWith(comment, "FROM CLIP") || istartsWith(comment, "FROM FILE")) {
                ev.sourcePath = afterColon(comment);
            } else if (istartsWith(comment, "SOURCE FILE")) {
                ev.sourceName = afterColon(comment);
            }
            continue;
        }
        const auto words = splitWords(line);
        if (words.size() >= 2 && iequals(words[0], "AUD")) {
            // Avid: "AUD  3    4" adds audio channels 3 and 4 to the event above (its channel field is NONE).
            if (!m_events.empty() && !m_events.back().lines.empty()) {
                for (std::size_t i = 1; i < words.size(); ++i) {
                    if (const auto n = parseInt64(words[i]); n && *n >= 1 && *n <= 64) {
                        m_events.back().lines.back().extraAudio.push_back(static_cast<int>(*n));
                    }
                }
            }
            continue;
        }
        if (words.size() >= 3 && iequals(words[0], "M2")) {
            if (!m_events.empty()) {
                EdlMotion motion;
                motion.line = lineNumber;
                motion.reel = std::string(words[1]);
                motion.fps = parseDouble(words[2]).value_or(0.0);
                m_events.back().motions.push_back(std::move(motion));
            }
            continue;
        }
        if (words.size() >= 8 && isDigits(words[0]) && isTimecode(words[words.size() - 1]) &&
            isTimecode(words[words.size() - 2]) && isTimecode(words[words.size() - 3]) &&
            isTimecode(words[words.size() - 4])) {
            EdlLine l;
            l.line = lineNumber;
            l.reel = std::string(words[1]);
            l.channel = std::string(words[2]);
            l.transition = std::string(words[3]);
            const std::size_t n = words.size();
            if (n >= 9) {
                l.transitionFrames = parseInt64(words[4]).value_or(0);
            }
            l.srcIn = std::string(words[n - 4]);
            l.srcOut = std::string(words[n - 3]);
            l.recIn = std::string(words[n - 2]);
            l.recOut = std::string(words[n - 1]);
            for (std::size_t i = n - 4; i < n; ++i) {
                sawTimecodeSemicolon = sawTimecodeSemicolon || words[i].find(';') != std::string_view::npos;
            }
            const std::string number(words[0]);
            if (m_events.empty() || m_events.back().number != number) {
                m_events.push_back({number, {}, {}, {}, {}, {}});
            }
            m_events.back().lines.push_back(std::move(l));
            continue;
        }
        ++unrecognised;
    }
    if (m_events.empty()) {
        return std::unexpected(parseError("no CMX3600 events found"));
    }
    if (m_seq.name.empty()) {
        m_seq.name = "EDL";
    }
    m_seq.location = at(1, "header");

    // Frame rate and drop-frame: the EDL only says drop or non-drop.
    m_dropFrame = fcmDrop.value_or(sawTimecodeSemicolon);
    const auto option = m_ctx.options ? m_ctx.options->edlFrameRate : std::nullopt;
    m_rate = option ? option->normalized() : (m_dropFrame ? kRate29_97 : kRate24);
    if (m_dropFrame && !supportsDropFrame(m_rate)) {
        report(ReportOutcome::Approximated, ReportCategory::SequenceSetting,
               "drop-frame timecode does not exist at " + rateLabel(m_rate) + " fps; read as non-drop",
               m_seq.location);
        m_dropFrame = false;
    }
    m_seq.frameRate = m_rate;
    m_seq.dropFrame = m_dropFrame;
    if (!option) {
        report(ReportOutcome::Approximated, ReportCategory::SequenceSetting,
               "EDLs carry no frame rate; " + rateLabel(m_rate) + " fps" +
                   (m_dropFrame ? " drop-frame" : "") + " assumed (set the EDL frame rate to change it)",
               m_seq.location);
    }
    report(ReportOutcome::Approximated, ReportCategory::SequenceSetting,
           "EDLs carry no frame size; 1920x1080 assumed", m_seq.location);
    if (fcmChanged) {
        report(ReportOutcome::Approximated, ReportCategory::SequenceSetting,
               "the EDL switches between drop and non-drop frame; all of it was read as " +
                   std::string(m_dropFrame ? "drop-frame" : "non-drop"),
               m_seq.location);
    }
    if (unrecognised > 0) {
        report(ReportOutcome::Dropped, ReportCategory::Effect,
               std::to_string(unrecognised) + " line(s) that are not CMX3600 events or comments were ignored",
               m_seq.location);
    }

    IrTrack v1;
    v1.kind = TrackKind::Video;
    v1.location = at(0, "V");
    m_seq.videoTracks.push_back(std::move(v1));

    // Rebase record time: the timecode hour the edit starts in becomes 0 (sequences usually start at
    // 01:00:00:00). The hour is taken from the label: a drop-frame hour is not a whole number of seconds.
    std::optional<Ticks> earliest;
    std::string earliestLabel;
    for (const EdlEvent& e : m_events) {
        for (const EdlLine& l : e.lines) {
            if (const auto t = tc(l.recIn); t && (!earliest || *t < *earliest)) {
                earliest = *t;
                earliestLabel = l.recIn;
            }
        }
    }
    Ticks base{};
    if (earliest) {
        if (const auto hour = parseInt64(std::string_view(earliestLabel).substr(0, 2)); hour && *hour > 0) {
            char label[16];
            std::snprintf(label, sizeof label, "%02d:00:00%c00", static_cast<int>(*hour),
                          m_dropFrame ? ';' : ':');
            base = tc(label).value_or(Ticks{});
        }
    }
    if (base.value > 0) {
        // X.1: the hour the record timecode starts in is the sequence's start timecode (Sequence::startTimecode).
        m_seq.startTimecode = base;
    }
    for (const EdlEvent& e : m_events) {
        convert(e, base);
    }
    mergeDualMono();

    for (const EdlLocator& loc : m_locators) {
        const auto t = tc(loc.timecode);
        IrMarker marker;
        marker.location = at(loc.line, "LOC");
        if (!t) {
            report(ReportOutcome::Dropped, ReportCategory::Marker,
                   "locator with an unreadable timecode '" + loc.timecode + "'", marker.location);
            continue;
        }
        marker.marker.start = *t - base;
        marker.marker.name = loc.text;
        marker.marker.colorIndex = markerColorFromName(loc.color);
        m_seq.markers.push_back(std::move(marker));
    }
    std::sort(m_seq.markers.begin(), m_seq.markers.end(),
              [](const IrMarker& a, const IrMarker& b) { return a.marker.start < b.marker.start; });

    if (!m_seq.audioTracks.empty()) {
        report(ReportOutcome::Approximated, ReportCategory::AudioMapping,
               "EDL audio channels became track" +
                   (m_seq.audioTracks.size() == 1 ? std::string(" A1")
                                                  : "s A1-A" + std::to_string(m_seq.audioTracks.size())) +
                   "; each audio clip plays its media's first audio stream (EDLs do not name streams)",
               m_seq.location);
    }
    m_doc.sequences.push_back(std::move(m_seq));
    return {};
}

void EdlParser::convert(const EdlEvent& event, Ticks base) {
    // A two-line dissolve/wipe/key event: the first (cut, usually zero-length) line is the outgoing source.
    // Two cut lines under one number are a split edit (e.g. video and audio with different times).
    const bool pair = event.lines.size() >= 2 && !iequals(event.lines[1].transition, "C");
    // Clips this event created, per media, to link video with audio afterwards.
    struct Created {
        TrackKind kind;
        std::size_t track;
        std::size_t clip;
    };
    std::map<std::size_t, std::vector<Created>> created;
    for (std::size_t i = 0; i < event.lines.size(); ++i) {
        const EdlLine& l = event.lines[i];
        const std::string element = "event " + event.number;
        const SourceLocation location = at(l.line, element);
        const auto recIn = tc(l.recIn);
        const auto recOut = tc(l.recOut);
        const auto srcIn = tc(l.srcIn);
        const auto srcOut = tc(l.srcOut);
        if (!recIn || !recOut || !srcIn || !srcOut) {
            report(ReportOutcome::Dropped, ReportCategory::Clip,
                   "event " + event.number + " has a timecode that does not exist at " + rateLabel(m_rate) +
                       " fps" + (m_dropFrame ? " drop-frame" : ""),
                   location);
            continue;
        }
        if (*recOut <= *recIn) {
            continue; // the outgoing side of a transition (or an empty event)
        }
        const std::string transition = toLowerAscii(l.transition);
        Channels channels = parseChannels(l.channel);
        for (const int extra : l.extraAudio) {
            if (std::find(channels.audio.begin(), channels.audio.end(), extra) == channels.audio.end()) {
                channels.audio.push_back(extra);
            }
        }
        if (!channels.valid) {
            report(ReportOutcome::Dropped, ReportCategory::Clip,
                   "event " + event.number + " has an unknown channel field '" + l.channel + "'", location);
            continue;
        }
        const Ticks start = *recIn - base;
        const Ticks duration = *recOut - *recIn;
        if (isBlackReel(l.reel)) {
            m_seq.videoTracks.front().gaps.push_back({{start, duration}, location});
            continue;
        }
        if (transition == "kb" || transition == "k" || transition == "ko") {
            if (transition != "kb") {
                report(ReportOutcome::Dropped, ReportCategory::Effect,
                       "key (foreground '" + l.reel + "') in event " + event.number + " is not supported yet",
                       location);
                continue;
            }
        }

        std::string clipName = pair && i > 0 ? event.toClip : event.fromClip;
        if (clipName.empty() && !pair) {
            clipName = event.toClip;
        }
        // Media identity: the source path when given, else the clip name, else the source name, else the
        // reel.
        IrMedia media;
        bool decoded = false;
        const bool incoming = pair && i > 0;
        if (!event.sourcePath.empty() && !incoming) {
            media.reference = referenceFrom(event.sourcePath, clipName, decoded);
            if (decoded) {
                media.key = "file:" + media.reference.path;
            }
        }
        if (media.key.empty()) {
            if (clipName.empty() && !event.sourceName.empty() && !incoming) {
                clipName = event.sourceName;
            }
            media.reference.name = clipName.empty() ? l.reel : clipName;
            media.reference.original = media.reference.name;
            media.key = clipName.empty() ? "reel:" + l.reel : "clip:" + clipName;
        }
        media.name =
            !clipName.empty() ? clipName : (media.reference.name.empty() ? l.reel : media.reference.name);
        media.absoluteSourceTimecode = true;
        media.stillImage = isStillImageName(media.name);
        media.location = location;
        const std::size_t mediaIndex = m_media.add(std::move(media));

        auto makeClip = [&](TrackKind kind) {
            IrClip c;
            c.name = m_doc.media[mediaIndex].name;
            c.media = mediaIndex;
            c.stream = {streamKindOf(kind), 0};
            c.start = start;
            c.duration = duration;
            c.sourceIn = *srcIn;
            c.location = location;
            return c;
        };
        std::vector<Created> lineClips; // this line's clips, for its motion effect
        if (channels.video) {
            m_doc.media[mediaIndex].hasVideo = true;
            IrTrack& v = m_seq.videoTracks.front();
            v.clips.push_back(makeClip(TrackKind::Video));
            created[mediaIndex].push_back({TrackKind::Video, 0, v.clips.size() - 1});
            lineClips.push_back(created[mediaIndex].back());
        }
        if (!channels.audio.empty()) {
            if (m_doc.media[mediaIndex].audioChannels.empty()) {
                m_doc.media[mediaIndex].audioChannels.push_back(0);
            }
            // AA (A1+A2) is one stereo source: one clip on the first channel's track.
            IrTrack& a = audioTrack(channels.audio.front());
            a.clips.push_back(makeClip(TrackKind::Audio));
            created[mediaIndex].push_back(
                {TrackKind::Audio, static_cast<std::size_t>(channels.audio.front() - 1), a.clips.size() - 1});
            lineClips.push_back(created[mediaIndex].back());
            if (channels.audio.size() > 1) {
                std::string list;
                for (const int ch : channels.audio) {
                    list += (list.empty() ? "A" : "+A") + std::to_string(ch);
                }
                report(ReportOutcome::Approximated, ReportCategory::AudioMapping,
                       "channels " + list + " of '" + m_doc.media[mediaIndex].name +
                           "' became one clip on A" + std::to_string(channels.audio.front()),
                       location);
            }
        }

        if ((transition == "d" || (transition.size() == 4 && transition[0] == 'w')) &&
            l.transitionFrames > 0) {
            IrTransition t;
            t.kind = transition == "d" ? TransitionKind::Dissolve : TransitionKind::Wipe;
            t.name = transition == "d" ? "Dissolve" : "Wipe " + l.transition.substr(1);
            t.cut = start;
            t.after = framesToTicksChecked(l.transitionFrames, m_rate).value_or(Ticks{});
            t.location = location;
            if (channels.video) {
                m_seq.videoTracks.front().transitions.push_back(t);
            }
            if (!channels.audio.empty()) {
                t.kind = TransitionKind::AudioCrossfade;
                audioTrack(channels.audio.front()).transitions.push_back(t);
            }
        }
        for (const EdlMotion& motion : event.motions) {
            if (!iequals(motion.reel, l.reel)) {
                continue;
            }
            // M2: the speed in frames per second of the record rate (negative: reverse, 0: freeze). The clip
            // keeps its record duration and plays duration x fps / rate of media from its source In.
            IrSpeed sp;
            sp.location = at(motion.line, element);
            // CMX writes the rate with one decimal (059.9 for 200% at 29.97 fps): a whole percentage within half
            // that step is the speed the editor set.
            const double fps = std::abs(motion.fps);
            const double rateFps = m_rate.toDouble();
            const auto percent = static_cast<std::int64_t>(std::llround(fps / rateFps * 100.0));
            const auto thousandths = static_cast<std::int64_t>(std::llround(fps * 1000.0));
            std::optional<wabcut::Speed> ratio;
            if (thousandths == 0) {
                ratio = wabcut::Speed{0, 1};
            } else if (percent > 0 && std::abs(static_cast<double>(percent) / 100.0 * rateFps - fps) <= 0.05 + 1e-9) {
                ratio = speedFromRatio(percent, 100);
            } else {
                ratio = speedFromRatio(thousandths * m_rate.den, std::int64_t{1000} * m_rate.num);
                if (ratio && std::abs(fps * 1000.0 - static_cast<double>(thousandths)) > 1e-6) {
                    sp.approximation = "the M2 speed " + std::to_string(motion.fps) + " fps was rounded to " +
                                       speedLabel(*ratio);
                }
            }
            if (ratio) {
                sp.ratio = *ratio;
                sp.reversed = ratio->num != 0 && motion.fps < 0.0;
            } else {
                report(ReportOutcome::Approximated, ReportCategory::Speed,
                       "'" + m_doc.media[mediaIndex].name + "' in event " + event.number +
                           " has an unusable M2 speed; imported at 100%",
                       sp.location);
                continue;
            }
            if (sp.ratio.num == sp.ratio.den && !sp.reversed) {
                continue; // 100%
            }
            for (const Created& c : lineClips) {
                auto& tracks = c.kind == TrackKind::Video ? m_seq.videoTracks : m_seq.audioTracks;
                tracks[c.track].clips[c.clip].speed = sp;
            }
        }
    }
    for (const auto& [mediaIndex, clips] : created) {
        const bool video = std::any_of(clips.begin(), clips.end(),
                                       [](const Created& c) { return c.kind == TrackKind::Video; });
        const bool audio = std::any_of(clips.begin(), clips.end(),
                                       [](const Created& c) { return c.kind == TrackKind::Audio; });
        if (!video || !audio) {
            continue;
        }
        const std::int64_t group = m_nextLink++;
        for (const Created& c : clips) {
            auto& tracks = c.kind == TrackKind::Video ? m_seq.videoTracks : m_seq.audioTracks;
            tracks[c.track].clips[c.clip].linkGroup = group;
        }
    }
}

void EdlParser::mergeDualMono() {
    // "A" and "A2" events with the same source and times are the two sides of one stereo source; like AA,
    // they become one clip on A1.
    if (m_seq.audioTracks.size() < 2) {
        return;
    }
    IrTrack& a1 = m_seq.audioTracks[0];
    IrTrack& a2 = m_seq.audioTracks[1];
    std::vector<IrClip> kept;
    for (IrClip& c : a2.clips) {
        const auto twin = std::find_if(a1.clips.begin(), a1.clips.end(), [&](const IrClip& o) {
            return o.media == c.media && o.start == c.start && o.duration == c.duration &&
                   o.sourceIn == c.sourceIn;
        });
        if (twin == a1.clips.end()) {
            kept.push_back(std::move(c));
            continue;
        }
        report(ReportOutcome::Approximated, ReportCategory::AudioMapping,
               "A1 and A2 of '" + c.name + "' at " + timecodeLabel(c.start, m_rate, m_dropFrame) +
                   " became one clip on A1",
               c.location);
    }
    a2.clips = std::move(kept);
}

} // namespace

std::expected<IrDocument, Error> parseEdl(std::string_view data, const ParseContext& context) {
    // EDLs carry no encoding; Avid and older tools write Windows-1252. Text that is not UTF-8 is read as that.
    std::string converted;
    if (!isValidUtf8(data)) {
        converted = toUtf8Lenient(data);
        data = converted;
    }
    IrDocument doc;
    EdlParser parser(context, doc);
    if (auto r = parser.parse(data); !r) {
        return std::unexpected(r.error());
    }
    return doc;
}

} // namespace wabcut::interchange::detail
