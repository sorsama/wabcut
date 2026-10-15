// CMX3600 edit decision list export, written the way Avid, Premiere Pro and Resolve conform it and so that
// EdlImporter.cpp reads it back:
//
//   TITLE: Main
//   FCM: NON-DROP FRAME
//
//   001  BL       V     C        00:00:00:00 00:00:01:00 01:00:00:00 01:00:01:00
//   002  A001C003 B     C        14:22:10:05 14:22:15:05 01:00:01:00 01:00:06:00
//   * FROM CLIP NAME: A001_C003.mov
//   * FROM FILE: D:/Shoot/A001_C003.mov
//   003  A001C003 V     C        14:22:15:05 14:22:15:05 01:00:06:00 01:00:06:00
//   003  INTERVIE V     D    030 00:10:00:00 00:10:05:00 01:00:06:00 01:00:11:00
//   * FROM CLIP NAME: A001_C003.mov
//   * TO CLIP NAME: Interview.mov
//   M2   INTERVIE       012.0                00:10:00:00
//   * LOC: 01:00:08:00 RED     Pick up here
//
// One video track (V1) and four audio channels (A1-A4). Times are whole frames at the sequence's rate, which an EDL
// does not name. Record timecode starts at 01:00:00:00 (the importer takes that hour away again); source timecode is
// the media's start timecode plus the clip's source position. A V1 clip and the A1 (and A2) clips linked to it with the
// same source and times are one event (B, AA/V); A1 and A2 twins are one AA event. A transition starts at its edit:
// the outgoing event ends where it starts, the incoming one begins there, and the event's first line names the
// outgoing source (black, BL, for a fade). Speed changes are M2 lines in frames per second (negative in reverse, 0 for
// a freeze frame). V1's gaps are black events from the sequence start, so the first event is in the first record hour.
//
// Reels are at most 8 letters and digits made from the source's name, one per source. The importer identifies media
// by "* FROM CLIP NAME:" (the full name) and by "* FROM FILE:" (the path) when an event has one; it reads no path for
// a transition's incoming clip, so a source that is ever one is written without its path everywhere (one identity per
// source). Lines end in CRLF.

#include "Exporters.h"
#include "TextUtil.h"
#include "TimeMath.h"

#include <wabcut/interchange/FileUrl.h>
#include <wabcut/model/Parameters.h>
#include <wabcut/model/Timecode.h>
#include <wabcut/model/TimeRemap.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace wabcut::interchange::detail {

namespace {

/// CMX3600 numbers events 001-999.
constexpr std::size_t kMaxEvents = 999;
/// Audio channels A1-A4: what the channel codes A, A2, A3, A4, AA, B and AA/V address.
constexpr std::size_t kAudioChannels = 4;
/// Reel names are at most 8 characters.
constexpr std::size_t kReelLength = 8;
constexpr const char* kBlack = "BL";
constexpr const char* kEndOfLine = "\r\n";

/// Locator colours in Wabcut's marker colour order (ImporterCommon.h), as markerColorFromName reads them back.
constexpr const char* kLocatorColors[] = {"GREEN", "RED", "MAGENTA", "ORANGE", "YELLOW", "WHITE", "BLUE", "CYAN"};

std::string padded(std::string text, std::size_t width) {
    if (text.size() < width) {
        text.append(width - text.size(), ' ');
    }
    return text;
}

std::string zeroPadded(std::int64_t value, std::size_t width) {
    std::string text = std::to_string(value);
    if (text.size() < width) {
        text.insert(0, width - text.size(), '0');
    }
    return text;
}

std::string frameCount(std::int64_t count) {
    return std::to_string(count) + (count == 1 ? " frame" : " frames");
}

std::string channelLabel(std::size_t channel) {
    return channel == 0 ? std::string("V1") : "A" + std::to_string(channel);
}

std::string withNotes(std::string reason, const std::vector<std::string>& notes) {
    for (const std::string& n : notes) {
        reason += "; " + n;
    }
    return reason;
}

/// `text` on one line: control characters (line breaks, tabs) become spaces.
std::string oneLine(std::string text) {
    for (char& ch : text) {
        if (static_cast<unsigned char>(ch) < 0x20 || ch == '\x7F') {
            ch = ' ';
        }
    }
    return text;
}

/// A locator's text as EdlImporter reads it back: its words separated by single spaces.
std::string locatorText(const std::string& text) {
    const std::string flat = oneLine(text);
    std::string out;
    for (const std::string_view word : splitWords(flat)) {
        if (!out.empty()) {
            out += ' ';
        }
        out += word;
    }
    return out;
}

/// `t` in whole frames at `r` (nearest); `exact` turns false when that rounds.
std::int64_t frames(Ticks t, Rate r, bool& exact) {
    const auto f = ticksToFrames(t, r, Rounding::Nearest);
    if (!f) {
        exact = false;
        return 0;
    }
    const auto back = framesToTicks(*f, r, Rounding::Nearest);
    exact = exact && back && *back == t;
    return *f;
}

/// A reel name for a source: the letters and digits of its name in upper case, without a file extension, at most 8
/// ("A001_C003.mov" -> "A001C003"). `whole` turns false when anything but the extension and the case was left out.
std::string reelBase(std::string_view name, bool& whole) {
    const auto dot = name.rfind('.');
    if (dot != std::string_view::npos && dot > 0 && name.size() - dot >= 3 && name.size() - dot <= 5) {
        const std::string_view extension = name.substr(dot + 1);
        if (std::any_of(extension.begin(), extension.end(),
                        [](char x) { return (x >= 'a' && x <= 'z') || (x >= 'A' && x <= 'Z'); })) {
            name = name.substr(0, dot);
        }
    }
    std::string reel;
    for (const char ch : name) {
        if (ch >= 'a' && ch <= 'z') {
            reel += static_cast<char>(ch - 'a' + 'A');
        } else if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')) {
            reel += ch;
        } else {
            whole = false;
        }
    }
    if (reel.size() > kReelLength) {
        reel.resize(kReelLength);
        whole = false;
    }
    if (reel.empty()) {
        reel = "AX"; // Premiere's reel for a source without one
        whole = false;
    }
    return reel;
}

/// Reel names EdlImporter reads as black.
bool isBlackReel(std::string_view reel) {
    return reel == "BL" || reel == "BLK" || reel == "BLACK";
}

bool sameSpeed(wabcut::Speed a, wabcut::Speed b) {
    return std::int64_t{a.num} * b.den == std::int64_t{b.num} * a.den;
}

/// The speed EdlImporter reads back from an M2 field of `scaled` / `unit` frames per second at `rate` (its rule: the
/// whole percentage when one is within 0.05 fps, otherwise thousandths of a frame per second).
std::optional<wabcut::Speed> m2Speed(std::int64_t scaled, std::int64_t unit, Rate rate) {
    const double fps = static_cast<double>(scaled) / static_cast<double>(unit);
    const double rateFps = rate.toDouble();
    const auto percent = static_cast<std::int64_t>(std::llround(fps / rateFps * 100.0));
    const auto thousandths = static_cast<std::int64_t>(std::llround(fps * 1000.0));
    if (thousandths == 0) {
        return wabcut::Speed{0, 1};
    }
    if (percent > 0 && std::abs(static_cast<double>(percent) / 100.0 * rateFps - fps) <= 0.05 + 1e-9) {
        return speedFromRatio(percent, 100);
    }
    return speedFromRatio(thousandths * rate.den, std::int64_t{1000} * rate.num);
}

/// "012.5" / "012.500": a non-negative value of `scaled` / `unit` (10 or 1000) with at least three whole digits.
std::string fixedPoint(std::int64_t scaled, std::int64_t unit) {
    const std::size_t decimals = unit == 10 ? 1 : 3;
    std::string fraction = std::to_string(scaled % unit);
    fraction.insert(0, decimals - std::min(decimals, fraction.size()), '0');
    return zeroPadded(scaled / unit, 3) + "." + fraction;
}

/// The EDL effect a transition is written as (D or W001), and what that changes.
struct EdlEffect {
    const char* code = "D";
    std::string note;
};

EdlEffect effectOf(const IrTransition& t) {
    switch (t.kind) {
    case TransitionKind::Dissolve:
        return {"D", {}};
    case TransitionKind::AudioCrossfade:
        if (t.name == "Constant Gain") {
            return {"D", "an EDL audio dissolve is read back as constant power"};
        }
        return {"D", {}};
    case TransitionKind::DipToBlack:
    case TransitionKind::DipToWhite:
        return {"D", "an EDL has no dips: written as a dissolve"};
    case TransitionKind::Wipe:
        return {"W001", "written as SMPTE wipe 001 (horizontal); its pattern and direction are not carried"};
    case TransitionKind::Other:
        break;
    }
    return {"D", "it has no EDL equivalent: written as a dissolve"};
}

class EdlWriter {
public:
    EdlWriter(const IrDocument& doc, const ExportContext& ctx)
        : m_doc(doc), m_ctx(ctx), m_seq(doc.sequences.front()) {
        if (m_seq.frameRate.isValid()) {
            m_rate = m_seq.frameRate.normalized();
        }
        m_dropFrame = m_seq.dropFrame && supportsDropFrame(m_rate);
        // X.1: record timecode starts at the sequence's start timecode (00:00:00:00 without one, as Premiere writes
        // it). The importer takes the hour it starts in as the start timecode again: a start between hours comes back
        // at the hour before it.
        bool exact = true;
        m_recordStart = m_seq.startTimecode ? frames(*m_seq.startTimecode, m_rate, exact) : 0;
        // An hour of frames as timecode counts it (a drop-frame hour drops 108 labels).
        const auto hourLabel = parseTimecodeFrames(m_dropFrame ? "01:00:00;00" : "01:00:00:00", m_rate, m_dropFrame);
        const std::int64_t hour = hourLabel ? *hourLabel : timecodeBase(m_rate) * 3600;
        if (!exact || (hour > 0 && m_recordStart % hour != 0)) {
            m_ctx.add(ReportOutcome::Approximated, ReportCategory::SequenceSetting,
                      "the start timecode is not on a whole hour: Wabcut reads the EDL back starting at the hour before it",
                      m_seq.location, m_seq.name);
        }
    }

private:
    /// The outgoing source a transition's first line names.
    struct Side {
        std::string reel = kBlack;
        std::string name;             ///< empty for black
        std::int64_t sourceFrame = 0; ///< source timecode as a frame count
    };

    /// One channel's part of an event: a clip, or black, over a record range.
    struct Edit {
        std::size_t channel = 0;      ///< 0 = V1, 1-4 = A1-A4
        const IrClip* clip = nullptr; ///< null: black
        std::string sourceKey;        ///< the source's identity (a media item or a sequence); empty for black
        std::string reel = kBlack;
        std::string name;             ///< the clip name comment; empty for black
        std::string path;             ///< the source's file, when known
        std::int64_t recIn = 0;       ///< record frames from the sequence start
        std::int64_t recOut = 0;
        std::int64_t srcIn = 0;       ///< source timecode as a frame count
        std::string effect = "C";     ///< C, D or W001
        std::int64_t effectFrames = 0;
        std::optional<Side> from;     ///< a transition's outgoing source
        std::string motion;           ///< the M2 line's speed field; empty at 100 % forward
        const IrTransition* transition = nullptr;
        std::vector<std::string> transitionNotes;
        /// What the event changes about the clip (Approximated when not empty).
        std::vector<std::string> notes;
        /// The clip's speed entry, when it is retimed.
        std::optional<std::pair<ReportOutcome, std::string>> speed;
    };

    struct Event {
        /// The channels it carries; its lines are written from the first (merged edits differ only in channel).
        std::vector<const Edit*> edits;
        std::string channel;
        std::size_t rank = 0; ///< order among events that start together: video first, then by audio channel
        std::string number;   ///< empty for an event past the 999th
        std::vector<std::string> locators;
    };

    /// What a clip plays, as the EDL names it.
    struct Source {
        std::string key;
        std::string name;
        std::string path;
        bool sequence = false; ///< a nested or multicam sequence
    };

    /// A clip's constant playback: a time remap flattened to its average speed.
    struct Playback {
        wabcut::Speed ratio{1, 1};
        bool reversed = false;
        Ticks sourceIn;   ///< the lowest source time it plays
        std::string note; ///< set when a time remap was flattened
    };

    struct Motion {
        std::string field;
        bool exact = false;
        wabcut::Speed readBack{1, 1};
    };

    // ---- small helpers -------------------------------------------------------------------------------------------

    std::string recordTc(std::int64_t frame) const {
        return framesToTimecode(m_recordStart + frame, m_rate, m_dropFrame);
    }
    std::string sourceTc(std::int64_t frame) const { return framesToTimecode(frame, m_rate, m_dropFrame); }

    void report(ReportOutcome outcome, ReportCategory category, std::string reason, const SourceLocation& where) const {
        m_ctx.add(outcome, category, std::move(reason), where, m_seq.name);
    }
    /// Carried, or Approximated with the notes appended.
    void reportNotes(ReportCategory category, std::string reason, const std::vector<std::string>& notes,
                     const SourceLocation& where) const {
        report(notes.empty() ? ReportOutcome::Carried : ReportOutcome::Approximated, category,
               withNotes(std::move(reason), notes), where);
    }

    std::string noun(const IrClip& c) const {
        if (c.sequence != kNoSequence && c.sequence < m_doc.sequences.size()) {
            return m_doc.sequences[c.sequence].multicam ? "multicam clip" : "nested clip";
        }
        return "clip";
    }

    static void line(std::string& out, std::string text) {
        while (!text.empty() && text.back() == ' ') {
            text.pop_back();
        }
        out += text;
        out += kEndOfLine;
    }

    /// "002  A001C003 V     C        <source in> <source out> <record in> <record out>"
    std::string eventLine(const std::string& number, const std::string& reel, const std::string& channel,
                          const std::string& effect, std::int64_t effectFrames, std::int64_t srcIn, std::int64_t length,
                          std::int64_t recIn) const {
        return number + "  " + padded(reel, kReelLength) + " " + padded(channel, 5) + " " + padded(effect, 4) + " " +
               (effect == "C" ? std::string(3, ' ') : zeroPadded(effectFrames, 3)) + " " + sourceTc(srcIn) + " " +
               sourceTc(srcIn + length) + " " + recordTc(recIn) + " " + recordTc(recIn + length);
    }

    // ---- sources -------------------------------------------------------------------------------------------------

    std::optional<Source> sourceOf(const IrClip& c) const {
        if (c.title) {
            return std::nullopt;
        }
        if (c.sequence != kNoSequence && c.sequence < m_doc.sequences.size()) {
            const IrSequence& nested = m_doc.sequences[c.sequence];
            Source s;
            s.key = "sequence-" + std::to_string(c.sequence);
            s.name = nested.name;
            s.sequence = true;
            if (nested.multicam) {
                std::string angles;
                for (const std::size_t a : c.angles) {
                    s.key += "-" + std::to_string(a);
                    const std::string angle =
                        a < nested.multicam->angles.size() ? nested.multicam->angles[a].name : std::string();
                    angles += (angles.empty() ? "" : ", ") +
                              (angle.empty() ? "angle " + std::to_string(a + 1) : angle);
                }
                if (!angles.empty()) {
                    s.name += " (" + angles + ")";
                }
            }
            return s;
        }
        if (c.sequence == kNoSequence && c.media != kNoMedia && c.media < m_doc.media.size()) {
            const IrMedia& m = m_doc.media[c.media];
            Source s;
            s.key = "media-" + std::to_string(c.media);
            s.name = m.name.empty() ? std::string(fileNameOf(m.reference.path)) : m.name;
            s.path = m.reference.path;
            return s;
        }
        return std::nullopt;
    }

    /// The source's reel: made from its name, unique per source (a number replaces its end when another source has
    /// it) and never a name EDL readers take for black.
    const std::string& reelFor(const Source& s) {
        if (const auto it = m_reels.find(s.key); it != m_reels.end()) {
            return it->second;
        }
        bool whole = true;
        const std::string base = reelBase(s.name, whole);
        std::string reel = base;
        for (std::size_t n = 2; m_usedReels.contains(reel) || isBlackReel(reel); ++n) {
            const std::string suffix = std::to_string(n);
            reel = base.substr(0, std::min(base.size(), kReelLength - suffix.size())) + suffix;
            whole = false;
        }
        m_usedReels.insert(reel);
        m_wholeReel.emplace(s.key, whole);
        return m_reels.emplace(s.key, std::move(reel)).first->second;
    }

    // ---- speed ---------------------------------------------------------------------------------------------------

    Playback playbackOf(const IrClip& c) const {
        Playback p;
        p.sourceIn = c.sourceIn;
        if (c.remap) {
            // Variable speed: the clip's first and last positions, played at the constant speed between them.
            const Ticks first = remapPositionAt(c.remap->curve, c.remap->curveIn);
            const Ticks last = remapPositionAt(c.remap->curve, c.remap->curveIn + c.duration);
            const Ticks span = last < first ? first - last : last - first;
            p.sourceIn = std::min(first, last);
            p.reversed = last < first;
            const std::string what = "has variable speed (" + std::to_string(c.remap->curve.keys.size()) +
                                     " keys), which an EDL cannot describe: ";
            std::optional<wabcut::Speed> average;
            if (span.value > 0 && c.duration.value > 0) {
                average = speedFromRatio(span.value, c.duration.value);
                if (!average) {
                    if (const auto closest = speedFromDouble(static_cast<double>(span.value) /
                                                             static_cast<double>(c.duration.value))) {
                        average = closest->speed;
                    }
                }
            }
            if (span.value == 0) {
                p.ratio = {0, 1};
                p.reversed = false;
                p.note = what + "written as a freeze frame";
            } else if (!average) {
                p.reversed = false;
                p.sourceIn = first;
                p.note = what + "written at 100%";
            } else {
                p.ratio = *average;
                p.note = what + "written at its average speed, " + speedLabel(*average) +
                         (p.reversed ? " in reverse" : "");
            }
            return p;
        }
        if (c.speed) {
            p.ratio = c.speed->ratio;
            p.reversed = c.speed->reversed && c.speed->ratio.num != 0;
        }
        return p;
    }

    /// The M2 field for a playback: frames per second of the record rate with one decimal as CMX writes it, three when
    /// one does not read back as the same speed.
    Motion motionOf(const Playback& p) const {
        Motion m;
        if (p.ratio.num == 0) {
            m.field = "000.0";
            m.exact = true;
            m.readBack = {0, 1};
            return m;
        }
        const std::int64_t num = std::int64_t{p.ratio.num} * m_rate.num;
        const std::int64_t den = std::int64_t{p.ratio.den} * m_rate.den;
        for (const std::int64_t unit : {std::int64_t{10}, std::int64_t{1000}}) {
            const auto scaled = mulDiv(num, unit, den, Rounding::Nearest);
            if (!scaled) {
                break;
            }
            const auto back = m2Speed(*scaled, unit, m_rate);
            m.field = (p.reversed ? "-" : "") + fixedPoint(*scaled, unit);
            m.readBack = back.value_or(wabcut::Speed{1, 1});
            m.exact = back.has_value() && sameSpeed(*back, p.ratio);
            if (m.exact) {
                break;
            }
        }
        return m;
    }

    /// "is a freeze frame", "plays at 50% in reverse".
    static std::string speedText(const Playback& p) {
        return p.ratio.num == 0 ? std::string("is a freeze frame")
                                : "plays at " + speedLabel(p.ratio) + (p.reversed ? " in reverse" : "");
    }

    // ---- edits ---------------------------------------------------------------------------------------------------

    Edit clipEdit(const IrClip& c, const Source& s, std::size_t channel, const IrTransition* head,
                  const IrTransition* tail) {
        Edit e;
        e.channel = channel;
        e.clip = &c;
        e.sourceKey = s.key;
        e.reel = reelFor(s);
        e.name = oneLine(s.name.empty() ? e.reel : s.name);
        e.path = s.path;
        // A transition starts at its edit: the event begins where the one at the clip's head starts and ends where the
        // one at its tail starts.
        const Ticks headBefore = head ? head->before : Ticks{};
        const Ticks tailBefore = tail ? tail->before : Ticks{};
        bool exact = true;
        e.recIn = frames(c.start - headBefore, m_rate, exact);
        e.recOut = frames(c.start + c.duration - tailBefore, m_rate, exact);
        const Playback p = playbackOf(c);
        // The lowest source time over the event (direction-aware: a reversed clip shows its lowest frames last).
        std::optional<Ticks> low = p.sourceIn;
        if (p.ratio.num != 0) {
            low = p.reversed ? checkedAdd(p.sourceIn, toSourceTicks(tailBefore, p.ratio))
                             : checkedSub(p.sourceIn, toSourceTicks(headBefore, p.ratio));
        }
        if (low && !s.sequence) {
            low = checkedAdd(*low, m_doc.media[c.media].startTimecode.value_or(Ticks{}));
        }
        std::int64_t src = 0;
        if (!low) {
            exact = false;
        } else if (s.sequence) {
            src = m_recordStart + frames(*low, m_rate, exact); // the nested sequence's own record timecode
        } else {
            src = frames(*low, m_rate, exact);
        }
        if (!exact) {
            e.notes.push_back("its position, length or source In was rounded to whole frames");
        }
        if (src < 0) {
            e.notes.push_back("its source timecode would be before " + sourceTc(0) + "; written as " + sourceTc(0));
            src = 0;
        }
        e.srcIn = src;

        if (c.speed || c.remap) {
            const std::string who = "'" + c.name + "' ";
            if (p.ratio.num == p.ratio.den && !p.reversed) {
                // 100 % forward: no M2 line (a time remap whose average is 100 %).
                e.speed.emplace(c.remap ? ReportOutcome::Approximated : ReportOutcome::Carried,
                                who + (c.remap ? p.note : std::string("plays at 100%")));
            } else {
                const Motion m = motionOf(p);
                e.motion = m.field;
                const std::string m2 = " (M2 " + m.field + " fps)";
                if (m.field.empty()) {
                    e.speed.emplace(ReportOutcome::Approximated,
                                    who + speedText(p) + "; that has no M2 line, so it is written at 100%");
                } else if (c.remap) {
                    e.speed.emplace(ReportOutcome::Approximated, who + p.note + m2);
                } else if (m.exact) {
                    e.speed.emplace(ReportOutcome::Carried, who + speedText(p) + m2);
                } else {
                    e.speed.emplace(ReportOutcome::Approximated, who + speedText(p) + ": its M2 speed " + m.field +
                                                                     " fps reads back as " + speedLabel(m.readBack));
                }
            }
        }
        if (!c.enabled) {
            e.notes.push_back("it is disabled, which an EDL cannot say: written as an active event");
        }
        if (s.sequence) {
            const bool multicam = m_doc.sequences[c.sequence].multicam.has_value();
            e.notes.push_back(std::string(multicam ? "an EDL has no multicam clips: written as one source named after "
                                                     "its multicam sequence and angle"
                                                   : "an EDL cannot nest sequences: written as one source named after "
                                                     "the sequence") +
                              ", with that sequence's own record timecode (from " + recordTc(0) +
                              ") as source timecode; it is read back as offline media");
        } else if (!c.name.empty() && c.name != s.name) {
            e.notes.push_back("its name is not carried: EDL events are named after their source ('" + e.name + "')");
        }
        if (c.stream.kind == StreamKind::Audio && c.stream.index > 0) {
            e.notes.push_back("it plays audio stream " + std::to_string(c.stream.index + 1) +
                              ", which an EDL cannot name (read back as the first)");
        }
        if (c.channels) {
            e.notes.push_back("its channel mapping is not carried");
        }
        return e;
    }

    /// The transition at a clip's head: the event's effect, and its first line naming the outgoing source (black when
    /// there is none or it is not written). One shorter than a frame is reported Dropped and the event is a cut.
    void attachHead(Edit& e, const IrTransition& t, const IrClip* outgoing,
                    const std::map<const IrClip*, Side>& sides) const {
        bool exact = true;
        const std::int64_t begin = frames(t.cut - t.before, m_rate, exact);
        const std::int64_t end = frames(t.cut + t.after, m_rate, exact);
        if (end <= begin) {
            report(ReportOutcome::Dropped, ReportCategory::Transition,
                   "'" + t.name + "' is shorter than a frame at " + rateLabel(m_rate) + " fps: written as a cut",
                   t.location);
            return;
        }
        const EdlEffect code = effectOf(t);
        e.transition = &t;
        e.effect = code.code;
        e.effectFrames = end - begin;
        if (!code.note.empty()) {
            e.transitionNotes.push_back(code.note);
        }
        if (!exact) {
            e.transitionNotes.push_back("its extent was rounded to whole frames");
        }
        if (t.before.value > 0) {
            e.transitionNotes.push_back("an EDL transition starts at its edit: the edit is written " +
                                        framesLabel(t.before, m_rate) + " before the cut (the picture is the same)");
        }
        Side side;
        if (outgoing) {
            if (const auto it = sides.find(outgoing); it != sides.end()) {
                side = it->second;
            } else {
                e.transitionNotes.push_back("its outgoing clip '" + outgoing->name +
                                            "' is not in the EDL: written from black");
            }
        }
        e.from = side;
    }

    /// A transition at a clip's tail that no written clip follows: an event from that clip to black.
    std::optional<Edit> blackIncoming(const IrTransition& t, std::size_t channel, const Side& from,
                                      const IrClip* incoming) const {
        bool exact = true;
        const std::int64_t begin = frames(t.cut - t.before, m_rate, exact);
        const std::int64_t end = frames(t.cut + t.after, m_rate, exact);
        if (end <= begin) {
            report(ReportOutcome::Dropped, ReportCategory::Transition,
                   "'" + t.name + "' is shorter than a frame at " + rateLabel(m_rate) + " fps", t.location);
            return std::nullopt;
        }
        const EdlEffect code = effectOf(t);
        Edit e;
        e.channel = channel;
        e.recIn = begin;
        e.recOut = end;
        e.effect = code.code;
        e.effectFrames = end - begin;
        e.from = from;
        e.transition = &t;
        if (!code.note.empty()) {
            e.transitionNotes.push_back(code.note);
        }
        if (!exact) {
            e.transitionNotes.push_back("its extent was rounded to whole frames");
        }
        if (incoming) {
            e.transitionNotes.push_back("its incoming clip '" + incoming->name +
                                        "' is not in the EDL: written to black");
        }
        e.transitionNotes.push_back("an EDL fade to black ends the outgoing event where it starts, and Wabcut reads it "
                                    "back as a cut to black there");
        return e;
    }

    /// One channel's clips and transitions as edits, in record order.
    void buildChannel(const IrTrack& track, std::size_t channel) {
        std::vector<const IrClip*> clips;
        for (const IrClip& c : track.clips) {
            clips.push_back(&c);
        }
        std::stable_sort(clips.begin(), clips.end(),
                         [](const IrClip* a, const IrClip* b) { return a->start < b->start; });
        std::map<std::int64_t, const IrClip*> startsAt;
        std::map<std::int64_t, const IrClip*> endsAt;
        for (const IrClip* c : clips) {
            startsAt.emplace(c->start.value, c);
            endsAt.emplace((c->start + c->duration).value, c);
        }
        std::map<std::int64_t, const IrTransition*> cuts;
        for (const IrTransition& tr : track.transitions) {
            cuts.emplace(tr.cut.value, &tr);
        }
        const auto transitionAt = [&cuts](Ticks when) -> const IrTransition* {
            const auto it = cuts.find(when.value);
            return it == cuts.end() ? nullptr : it->second;
        };
        const std::string label = channelLabel(channel);
        std::set<const IrTransition*> handled;
        std::map<const IrClip*, Side> sides;
        for (const IrClip* c : clips) {
            const std::optional<Source> s = sourceOf(*c);
            if (!s) {
                report(ReportOutcome::Dropped, ReportCategory::Clip,
                       std::string(c->title ? "title '" : "'") + c->name + "' on " + label + ": " +
                           (c->title ? "an EDL cannot describe a title" : "its source is missing") +
                           "; its time is left " + (channel == 0 ? "black" : "silent"),
                       c->location);
                continue;
            }
            const IrTransition* head = transitionAt(c->start);
            const IrTransition* tail = transitionAt(c->start + c->duration);
            Edit e = clipEdit(*c, *s, channel, head, tail);
            sides[c] = Side{e.reel, e.name, e.srcIn + (e.recOut - e.recIn)};
            if (head) {
                handled.insert(head);
                const auto out = endsAt.find(head->cut.value);
                attachHead(e, *head, out == endsAt.end() ? nullptr : out->second, sides);
            }
            if (e.recOut > e.recIn) {
                m_edits[channel].push_back(std::move(e));
            } else {
                if (e.transition) {
                    report(ReportOutcome::Dropped, ReportCategory::Transition,
                           "'" + e.transition->name + "': the clip it leads into is shorter than a frame",
                           e.transition->location);
                }
                if (tail) {
                    report(ReportOutcome::Approximated, ReportCategory::Clip,
                           noun(*c) + " '" + c->name +
                               "' lies entirely inside the transition after it: the EDL names it only as that "
                               "transition's outgoing source",
                           c->location);
                } else {
                    report(ReportOutcome::Dropped, ReportCategory::Clip,
                           noun(*c) + " '" + c->name + "' on " + label + " is shorter than a frame at " +
                               rateLabel(m_rate) + " fps",
                           c->location);
                }
            }
            if (tail) {
                const auto in = startsAt.find(tail->cut.value);
                const bool followed = in != startsAt.end() && sourceOf(*in->second).has_value();
                if (!followed) {
                    handled.insert(tail);
                    const IrClip* unwritten = in == startsAt.end() ? nullptr : in->second;
                    if (auto black = blackIncoming(*tail, channel, sides[c], unwritten)) {
                        m_edits[channel].push_back(std::move(*black));
                    }
                }
            }
        }
        for (const IrTransition& tr : track.transitions) {
            if (!handled.contains(&tr)) {
                report(ReportOutcome::Dropped, ReportCategory::Transition,
                       "'" + tr.name + "' on " + label + ": no clip of the EDL starts or ends at it", tr.location);
            }
        }
    }

    /// V1's gaps as black (BL) events from the sequence start: to its length when V1 has events, otherwise up to the
    /// first event (so the first record hour is the first one) or, for an empty sequence, its length.
    void fillBlack() {
        std::vector<Edit>& video = m_edits[0];
        bool exact = true;
        const std::int64_t length = m_seq.duration ? frames(*m_seq.duration, m_rate, exact) : 0;
        std::int64_t fillTo = length;
        for (const Edit& e : video) {
            fillTo = std::max(fillTo, e.recOut);
        }
        if (video.empty()) {
            std::optional<std::int64_t> first;
            for (std::size_t ch = 1; ch <= kAudioChannels; ++ch) {
                for (const Edit& e : m_edits[ch]) {
                    first = first ? std::min(*first, e.recIn) : e.recIn;
                }
            }
            if (first) {
                fillTo = *first;
            }
        }
        std::vector<Edit> black;
        const auto gap = [&black](std::int64_t from, std::int64_t to) {
            Edit e;
            e.recIn = from;
            e.recOut = to;
            black.push_back(std::move(e));
        };
        std::int64_t at = 0;
        for (const Edit& e : video) {
            if (e.recIn > at) {
                gap(at, e.recIn);
            }
            at = std::max(at, e.recOut);
        }
        if (fillTo > at) {
            gap(at, fillTo);
        }
        video.insert(video.end(), black.begin(), black.end());
        std::stable_sort(video.begin(), video.end(), [](const Edit& a, const Edit& b) { return a.recIn < b.recIn; });
    }

    // ---- events --------------------------------------------------------------------------------------------------

    /// What edits of two channels must share to be one event.
    static std::string mergeKey(const Edit& e) {
        std::string key = e.sourceKey + '\n' + std::to_string(e.recIn) + '\n' + std::to_string(e.recOut) + '\n' +
                          std::to_string(e.srcIn) + '\n' + e.effect + '\n' + std::to_string(e.effectFrames) + '\n' +
                          e.motion;
        if (e.from) {
            key += '\n' + e.from->reel + '\n' + std::to_string(e.from->sourceFrame);
        }
        return key;
    }

    /// What EdlImporter compares to merge an A1 and an A2 event into one clip.
    static std::string twinKey(const Edit& e) {
        return e.sourceKey + '\n' + std::to_string(e.recIn) + '\n' + std::to_string(e.recOut) + '\n' +
               std::to_string(e.srcIn);
    }

    /// The edits as events: a V1 clip with the A1 (and A2) clips linked to it that match it (B, AA/V), A1 with its A2
    /// twin (AA), everything else alone; in record order.
    std::vector<Event> makeEvents() {
        std::array<std::vector<bool>, kAudioChannels + 1> used;
        for (std::size_t ch = 0; ch <= kAudioChannels; ++ch) {
            used[ch].assign(m_edits[ch].size(), false);
        }
        const auto indexOf = [this](std::size_t ch) {
            std::unordered_map<std::string, std::size_t> index;
            for (std::size_t i = 0; i < m_edits[ch].size(); ++i) {
                if (m_edits[ch][i].clip) {
                    index.emplace(mergeKey(m_edits[ch][i]), i);
                }
            }
            return index;
        };
        const auto a1 = indexOf(1);
        const auto a2 = indexOf(2);
        std::vector<Event> events;
        for (const Edit& v : m_edits[0]) {
            Event ev;
            ev.edits.push_back(&v);
            if (v.clip && v.clip->linkGroup >= 0) {
                const std::string key = mergeKey(v);
                const auto linked = [&](const std::unordered_map<std::string, std::size_t>& candidates,
                                        std::size_t audio) -> const Edit* {
                    const auto it = candidates.find(key);
                    if (it == candidates.end() || used[audio][it->second] ||
                        m_edits[audio][it->second].clip->linkGroup != v.clip->linkGroup) {
                        return nullptr;
                    }
                    used[audio][it->second] = true;
                    return &m_edits[audio][it->second];
                };
                if (const Edit* first = linked(a1, 1)) {
                    ev.edits.push_back(first);
                    if (const Edit* second = linked(a2, 2)) {
                        ev.edits.push_back(second);
                    }
                }
            }
            ev.channel = ev.edits.size() == 1 ? "V" : ev.edits.size() == 2 ? "B" : "AA/V";
            events.push_back(std::move(ev));
        }
        for (std::size_t i = 0; i < m_edits[1].size(); ++i) {
            if (used[1][i]) {
                continue;
            }
            const Edit& a = m_edits[1][i];
            Event ev;
            ev.edits.push_back(&a);
            ev.rank = 1;
            if (a.clip) {
                if (const auto it = a2.find(mergeKey(a)); it != a2.end() && !used[2][it->second]) {
                    used[2][it->second] = true;
                    ev.edits.push_back(&m_edits[2][it->second]);
                }
            }
            ev.channel = ev.edits.size() == 1 ? "A" : "AA";
            events.push_back(std::move(ev));
        }
        for (std::size_t ch = 2; ch <= kAudioChannels; ++ch) {
            for (std::size_t i = 0; i < m_edits[ch].size(); ++i) {
                if (used[ch][i]) {
                    continue;
                }
                Event ev;
                ev.edits.push_back(&m_edits[ch][i]);
                ev.rank = ch;
                ev.channel = channelLabel(ch);
                events.push_back(std::move(ev));
            }
        }
        std::stable_sort(events.begin(), events.end(), [](const Event& a, const Event& b) {
            const std::int64_t at = a.edits.front()->recIn;
            const std::int64_t bt = b.edits.front()->recIn;
            return at != bt ? at < bt : a.rank < b.rank;
        });
        for (std::size_t i = 0; i < events.size() && i < kMaxEvents; ++i) {
            events[i].number = zeroPadded(static_cast<std::int64_t>(i + 1), 3);
        }
        return events;
    }

    /// Each marker as a "* LOC:" line under the last written event that starts at or before it (else the first).
    /// Returns the lines that have no event to go under.
    std::vector<std::string> placeMarkers(std::vector<Event>& events) {
        std::vector<const IrMarker*> markers;
        for (const IrMarker& m : m_seq.markers) {
            markers.push_back(&m);
        }
        std::stable_sort(markers.begin(), markers.end(),
                         [](const IrMarker* a, const IrMarker* b) { return a->marker.start < b->marker.start; });
        const auto written = static_cast<std::ptrdiff_t>(std::count_if(
            events.begin(), events.end(), [](const Event& ev) { return !ev.number.empty(); }));
        std::vector<std::string> unplaced;
        for (const IrMarker* m : markers) {
            bool exact = true;
            const std::int64_t frame = frames(m->marker.start, m_rate, exact);
            std::vector<std::string> notes;
            if (!exact) {
                notes.push_back("moved to a whole frame");
            }
            std::string color = "GREEN";
            if (m->marker.colorIndex >= 0 && m->marker.colorIndex < 8) {
                color = kLocatorColors[static_cast<std::size_t>(m->marker.colorIndex)];
            } else {
                notes.push_back("its colour was written as green");
            }
            const std::string text = locatorText(m->marker.name);
            if (text != m->marker.name) {
                notes.push_back("its name was written on one line with single spaces");
            }
            if (m->marker.duration.value > 0) {
                notes.push_back("its duration is not carried (a locator is one frame)");
            }
            if (!m->marker.comment.empty()) {
                notes.push_back("its comment is not carried");
            }
            if (m->marker.type != MarkerType::Comment) {
                notes.push_back("its type (" + std::string(markerTypeName(m->marker.type)) + ") is not carried");
            }
            const std::string loc = "* LOC: " + recordTc(frame) + " " + padded(color, 7) + " " + text;
            if (written == 0) {
                unplaced.push_back(loc);
            } else {
                const auto after =
                    std::upper_bound(events.begin(), events.begin() + written, frame,
                                     [](std::int64_t f, const Event& ev) { return f < ev.edits.front()->recIn; });
                const auto target = after == events.begin() ? events.begin() : after - 1;
                target->locators.push_back(loc);
            }
            m_markerReports.emplace_back(withNotes("marker '" + m->marker.name + "' at " + recordTc(frame) + " (" +
                                                       color + ")",
                                                   notes),
                                         m);
            m_markerApproximated.push_back(!notes.empty());
        }
        return unplaced;
    }

    void writeEvent(std::string& out, const Event& ev) const {
        const Edit& e = *ev.edits.front();
        const std::int64_t length = e.recOut - e.recIn;
        if (e.from) {
            line(out, eventLine(ev.number, e.from->reel, ev.channel, "C", 0, e.from->sourceFrame, 0, e.recIn));
        }
        line(out, eventLine(ev.number, e.reel, ev.channel, e.effect, e.effectFrames, e.srcIn, length, e.recIn));
        if (e.from) {
            if (!e.from->name.empty()) {
                line(out, "* FROM CLIP NAME: " + e.from->name);
            }
            if (!e.name.empty()) {
                line(out, "* TO CLIP NAME: " + e.name);
            }
        } else if (!e.name.empty()) {
            line(out, "* FROM CLIP NAME: " + e.name);
            if (!e.path.empty() && !m_incoming.contains(e.sourceKey)) {
                line(out, "* FROM FILE: " + oneLine(e.path));
            }
        }
        if (!e.motion.empty()) {
            line(out, "M2   " + padded(e.reel, kReelLength) + " " + padded(e.motion, 21) + sourceTc(e.srcIn));
        }
        for (const std::string& loc : ev.locators) {
            line(out, loc);
        }
    }

    // ---- reports -------------------------------------------------------------------------------------------------

    void reportSequence() const {
        const std::string timing = rateLabel(m_rate) + " fps " + (m_dropFrame ? "drop-frame" : "non-drop");
        report(ReportOutcome::Approximated, ReportCategory::SequenceSetting,
               "'" + m_seq.name + "' as the EDL's TITLE, " + timing + " record timecode from " + recordTc(0) +
                   "; an EDL does not name its frame rate (import it at " + rateLabel(m_rate) +
                   " fps) and has no frame size (" + std::to_string(m_seq.width) + "x" + std::to_string(m_seq.height) +
                   " is not written)",
               m_seq.location);
        if (m_seq.dropFrame && !m_dropFrame) {
            report(ReportOutcome::Approximated, ReportCategory::SequenceSetting,
                   "drop-frame timecode does not exist at " + rateLabel(m_rate) + " fps; written as non-drop",
                   m_seq.location);
        }
        if (m_seq.markIn || m_seq.markOut) {
            report(ReportOutcome::Dropped, ReportCategory::SequenceSetting,
                   "its In/Out marks are not carried: an EDL has none", m_seq.location);
        }
    }

    void reportLength(std::int64_t contentEnd) const {
        if (!m_seq.duration) {
            return;
        }
        bool exact = true;
        const std::int64_t length = frames(*m_seq.duration, m_rate, exact);
        if (length <= contentEnd) {
            return;
        }
        std::int64_t blackEnd = 0;
        for (const Edit& e : m_edits[0]) {
            blackEnd = std::max(blackEnd, e.recOut);
        }
        const std::string tail = frameCount(length - contentEnd);
        if (blackEnd >= length) {
            report(ReportOutcome::Approximated, ReportCategory::SequenceSetting,
                   "its last " + tail + " (after the last clip) are written as black (BL); Wabcut reads a sequence's "
                                        "length back from its clips",
                   m_seq.location);
        } else {
            report(ReportOutcome::Dropped, ReportCategory::SequenceSetting,
                   "its last " + tail + " (after the last clip) are not carried: an EDL ends with its last event",
                   m_seq.location);
        }
    }

    /// V1 or A1-A4, before its clips.
    void reportChannel(const IrTrack& t, std::size_t channel, std::size_t lastAudio) const {
        const std::string label = channelLabel(channel);
        if (channel > lastAudio && t.clips.empty()) {
            report(ReportOutcome::Dropped, ReportCategory::Track,
                   label + " is empty: an EDL has only the channels its events use", t.location);
            return;
        }
        std::vector<std::string> notes;
        if (!t.name.empty()) {
            notes.push_back("its name '" + t.name + "' is not carried");
        }
        if (!t.enabled) {
            notes.push_back("it is turned off, which an EDL cannot say: its events are written as active");
        } else if (t.kind == TrackKind::Audio && t.muted) {
            notes.push_back("it is muted, which an EDL cannot say: its events are written as active");
        }
        if (t.locked) {
            notes.push_back("its lock is not carried");
        }
        reportNotes(ReportCategory::Track,
                    label + (channel == 0 ? " as the EDL's video (V)" : " as EDL audio channel " + label), notes,
                    t.location);
    }

    /// A track the EDL has no channel for, its clips and transitions.
    void dropTrack(const IrTrack& t, const std::string& label, const std::string& why) const {
        report(ReportOutcome::Dropped, ReportCategory::Track,
               label + (t.name.empty() ? std::string() : " '" + t.name + "'") + ": " + why, t.location);
        for (const IrClip& c : t.clips) {
            report(ReportOutcome::Dropped, ReportCategory::Clip, noun(c) + " '" + c.name + "' on " + label + ": " + why,
                   c.location);
        }
        for (const IrTransition& tr : t.transitions) {
            report(ReportOutcome::Dropped, ReportCategory::Transition, "'" + tr.name + "' on " + label + ": " + why,
                   tr.location);
        }
    }

    /// Once per media item, at its first event.
    void reportMedia(const Edit& e) {
        if (!m_reportedMedia.insert(e.sourceKey).second) {
            return;
        }
        const IrMedia& m = m_doc.media[e.clip->media];
        std::vector<std::string> notes;
        std::string reason = "'" + m.name + "' as reel " + e.reel;
        if (const auto whole = m_wholeReel.find(e.sourceKey); whole != m_wholeReel.end() && !whole->second) {
            notes.push_back("a reel name is at most 8 letters and digits; the full name is in the clip name comments");
        }
        if (m.reference.path.empty()) {
            notes.push_back("it has no file path: EDL readers find it by name");
        } else if (m_incoming.contains(e.sourceKey)) {
            notes.push_back("its path is not written (EDL readers name a transition's incoming clip by name only): it "
                            "is found by name");
        } else {
            reason += " (" + m.reference.path + ")";
        }
        if (m.startTimecode) {
            bool exact = true;
            reason += ", source timecode from its start timecode " + sourceTc(frames(*m.startTimecode, m_rate, exact));
        } else {
            reason += ", source timecode from " + sourceTc(0) + " (no start timecode is known)";
        }
        if (m.hasVideo && !m.stillImage && m.frameRate && !(*m.frameRate == m_rate)) {
            notes.push_back("its source timecode is counted at the sequence's " + rateLabel(m_rate) +
                            " fps, not its own " + rateLabel(*m.frameRate) + " fps (an EDL has one rate)");
        }
        m_ctx.add(notes.empty() ? ReportOutcome::Carried : ReportOutcome::Approximated, ReportCategory::Media,
                  withNotes(std::move(reason), notes), m.location, {});
    }

    void reportTransition(const Event& ev, const Edit& e) const {
        const IrTransition& t = *e.transition;
        reportNotes(ReportCategory::Transition,
                    "'" + t.name + "' (" + framesLabel(t.before + t.after, m_rate) + ") as " + e.effect + " " +
                        zeroPadded(e.effectFrames, 3) + " in event " + ev.number,
                    e.transitionNotes, t.location);
    }

    void reportEffects(const IrClip& c) const {
        if (!c.effects.empty()) {
            std::string list;
            for (const EffectInstance& fx : c.effects) {
                list += (list.empty() ? "" : ", ") + fx.effect;
            }
            report(ReportOutcome::Dropped, ReportCategory::Effect,
                   "'" + c.name + "': its effects are not carried (an EDL has none): " + list, c.location);
        }
        if (c.properties) {
            std::string list;
            for (const ParameterInfo& info : allParameters()) {
                if (!c.properties->isDefault(info.id)) {
                    list += (list.empty() ? "" : ", ") + std::string(info.displayName);
                }
            }
            if (!list.empty()) {
                report(ReportOutcome::Dropped, ReportCategory::Effect,
                       "'" + c.name + "': its " + list + " settings are not carried (an EDL has no clip properties)",
                       c.location);
            }
        }
    }

    void reportEvent(const Event& ev) {
        if (ev.number.empty()) {
            for (const Edit* x : ev.edits) {
                if (x->transition) {
                    report(ReportOutcome::Dropped, ReportCategory::Transition,
                           "'" + x->transition->name + "': a CMX3600 EDL has at most 999 events",
                           x->transition->location);
                }
                if (x->clip) {
                    report(ReportOutcome::Dropped, ReportCategory::Clip,
                           noun(*x->clip) + " '" + x->clip->name + "': a CMX3600 EDL has at most 999 events",
                           x->clip->location);
                }
            }
            return;
        }
        std::map<std::int64_t, std::size_t> linked;
        for (const Edit* x : ev.edits) {
            if (x->clip && x->clip->linkGroup >= 0) {
                ++linked[x->clip->linkGroup];
            }
        }
        for (const Edit* x : ev.edits) {
            if (x->transition) {
                reportTransition(ev, *x);
            }
            if (!x->clip) {
                continue;
            }
            const IrClip& c = *x->clip;
            if (c.sequence == kNoSequence) {
                reportMedia(*x);
            }
            std::vector<std::string> notes = x->notes;
            if (c.linkGroup >= 0) {
                const auto size = m_linkSizes.find(c.linkGroup);
                if (size != m_linkSizes.end() && size->second > linked[c.linkGroup]) {
                    notes.push_back("its link to clips in other events is not carried (an EDL event links only the "
                                    "channels it names)");
                }
            }
            if (x->channel == 2 && m_a1Twins.contains(twinKey(*x))) {
                notes.push_back("Wabcut reads A1 and A2 of one source with the same times back as one A1 clip");
            }
            reportNotes(ReportCategory::Clip,
                        noun(c) + " '" + c.name + "' as event " + ev.number + " (" + ev.channel + ", reel " +
                            x->reel + ")",
                        notes, c.location);
            if (x->speed) {
                report(x->speed->first, ReportCategory::Speed, x->speed->second, c.location);
            }
            reportEffects(c);
        }
    }

    const IrDocument& m_doc;
    const ExportContext& m_ctx;
    const IrSequence& m_seq;
    Rate m_rate = kRate30;
    bool m_dropFrame = false;
    /// The frame count of 01:00:00:00, where record timecode starts.
    std::int64_t m_recordStart = 0;
    /// Edits per channel: [0] V1, [1]-[4] A1-A4.
    std::array<std::vector<Edit>, kAudioChannels + 1> m_edits;
    std::map<std::string, std::string> m_reels; ///< source key -> reel
    std::set<std::string> m_usedReels;
    std::map<std::string, bool> m_wholeReel;  ///< source key -> its reel is its whole name
    std::set<std::string> m_incoming;         ///< sources that are a written transition's incoming clip
    std::set<std::string> m_reportedMedia;
    std::set<std::string> m_a1Twins;          ///< twinKey of every A1 clip edit
    std::map<std::int64_t, std::size_t> m_linkSizes;
    std::vector<std::pair<std::string, const IrMarker*>> m_markerReports;
    std::vector<bool> m_markerApproximated;

public:
    std::string finish() {
        reportSequence();
        std::size_t lastAudio = 0; // the highest channel with clips: EdlImporter creates every channel up to it
        for (std::size_t i = 0; i < m_seq.audioTracks.size() && i < kAudioChannels; ++i) {
            if (!m_seq.audioTracks[i].clips.empty()) {
                lastAudio = i + 1;
            }
        }
        for (std::size_t i = 0; i < m_seq.videoTracks.size(); ++i) {
            if (i == 0) {
                reportChannel(m_seq.videoTracks[i], 0, lastAudio);
                buildChannel(m_seq.videoTracks[i], 0);
            } else {
                dropTrack(m_seq.videoTracks[i], "V" + std::to_string(i + 1), "an EDL has one video track");
            }
        }
        for (std::size_t i = 0; i < m_seq.audioTracks.size(); ++i) {
            if (i < kAudioChannels) {
                reportChannel(m_seq.audioTracks[i], i + 1, lastAudio);
                buildChannel(m_seq.audioTracks[i], i + 1);
            } else {
                dropTrack(m_seq.audioTracks[i], "A" + std::to_string(i + 1), "an EDL has four audio channels (A1-A4)");
            }
        }
        for (const auto* tracks : {&m_seq.videoTracks, &m_seq.audioTracks}) {
            for (const IrTrack& t : *tracks) {
                for (const IrClip& c : t.clips) {
                    if (c.linkGroup >= 0) {
                        ++m_linkSizes[c.linkGroup];
                    }
                }
            }
        }
        std::int64_t contentEnd = 0;
        for (const std::vector<Edit>& edits : m_edits) {
            for (const Edit& e : edits) {
                contentEnd = std::max(contentEnd, e.recOut);
            }
        }
        fillBlack();
        reportLength(contentEnd);
        for (const Edit& e : m_edits[1]) {
            if (e.clip) {
                m_a1Twins.insert(twinKey(e));
            }
        }

        std::vector<Event> events = makeEvents();
        if (events.empty()) {
            report(ReportOutcome::Approximated, ReportCategory::SequenceSetting,
                   "the sequence has nothing an EDL can hold: the EDL has no events, which EDL readers refuse",
                   m_seq.location);
        }
        for (const Event& ev : events) {
            if (!ev.number.empty() && ev.edits.front()->from && ev.edits.front()->clip) {
                m_incoming.insert(ev.edits.front()->sourceKey);
            }
        }
        const std::vector<std::string> unplaced = placeMarkers(events);

        std::string out;
        line(out, "TITLE: " + oneLine(m_seq.name));
        line(out, std::string("FCM: ") + (m_dropFrame ? "DROP FRAME" : "NON-DROP FRAME"));
        out += kEndOfLine;
        for (const std::string& loc : unplaced) {
            line(out, loc);
        }
        for (const Event& ev : events) {
            if (!ev.number.empty()) {
                writeEvent(out, ev);
            }
        }

        for (const Event& ev : events) {
            reportEvent(ev);
        }
        for (std::size_t i = 0; i < m_markerReports.size(); ++i) {
            report(m_markerApproximated[i] ? ReportOutcome::Approximated : ReportOutcome::Carried,
                   ReportCategory::Marker, m_markerReports[i].first, m_markerReports[i].second->location);
        }
        return out;
    }
};

} // namespace

std::string writeEdl(const IrDocument& document, const ExportContext& context) {
    if (document.sequences.empty()) {
        return {};
    }
    EdlWriter writer(document, context);
    return writer.finish();
}

} // namespace wabcut::interchange::detail
