// ProjectController, 0.2 timeline parity: Clip > Group / Ungroup, Selection Follows Playhead and Go to Gap (Next /
// Previous in Sequence). The model side is ClipGroups.h and TimelineSelection.h.
//
// 0.2 UX rework (stream T): typed playhead entry (seekToText: absolute times in the display format, +/- offsets), the
// gap selection (a click in a gap, Delete / Ripple Delete closes it: edit::rippleDeleteGap) and the track header menu
// (Rename, Add Track(s) with a placement: edit::insertTrack, Delete Track, Delete Empty Tracks).

#include "ProjectController.h"

#include "BridgeIds.h"

#include <wabcut/model/AudioChannels.h>
#include <wabcut/model/ClipGroups.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/SequenceSettings.h>
#include <wabcut/model/Timecode.h>
#include <wabcut/model/TimelineSelection.h>

#include <QRegularExpression>

#include <algorithm>

namespace wabcut::app {

namespace {

constexpr int kMaxTracksPerAdd = 99;
constexpr qsizetype kMaxTrackName = 256;

bool isTimeSeparator(QChar c) {
    return c == u':' || c == u';' || c == u'.' || c == u',';
}

/// The index of the last separator in `text`, or -1.
qsizetype lastSeparator(const QString& text) {
    for (qsizetype i = text.size(); i-- > 0;) {
        if (isTimeSeparator(text[i]))
            return i;
    }
    return -1;
}

bool allDigits(const QString& text) {
    return !text.isEmpty() && std::all_of(text.begin(), text.end(), [](QChar c) { return c.isDigit(); });
}

/// Digits of the frames field at `rate`: 2, or 3 from 101 fps (frames 0..119 at 120 fps).
qsizetype framesDigits(Rate rate) {
    return timecodeBase(rate) > 100 ? 3 : 2;
}

/// Premiere's shorthand in typed timecode: an empty field is "00" ("1." = one second, "1.." = one minute).
QString filledFields(const QString& text) {
    if (std::none_of(text.begin(), text.end(), isTimeSeparator))
        return text;
    QStringList fields;
    QString field;
    for (const QChar c : text) {
        if (isTimeSeparator(c)) {
            fields << (field.isEmpty() ? QStringLiteral("00") : field);
            field.clear();
        } else {
            field += c;
        }
    }
    fields << (field.isEmpty() ? QStringLiteral("00") : field);
    return fields.join(u':');
}

/// An audio time as Show Audio Time Units displays it - "[[HH:]MM:]SS:samples" or "[[HH:]MM:]SS.mmm" - or a bare
/// sample count; nullopt when `text` is not one.
std::optional<Ticks> parseAudioTime(const QString& text, Rate audioRate, bool milliseconds) {
    if (allDigits(text)) {
        if (text.size() > 10)
            return std::nullopt;
        return framesToTicks(text.toLongLong(), audioRate);
    }
    const qsizetype last = lastSeparator(text);
    if (last <= 0)
        return std::nullopt;
    const QString tail = text.mid(last + 1);
    if (!allDigits(tail) || tail.size() > 9)
        return std::nullopt;
    static const QRegularExpression kSeparators(QStringLiteral("[:;.,]"));
    const QStringList parts = text.left(last).split(kSeparators);
    if (parts.size() > 3)
        return std::nullopt;
    std::int64_t seconds = 0;
    for (qsizetype i = 0; i < parts.size(); ++i) {
        const QString& part = parts[i];
        if (!allDigits(part) || part.size() > (i == 0 ? 3 : 2))
            return std::nullopt;
        const std::int64_t v = part.toLongLong();
        if (i > 0 && v >= 60)
            return std::nullopt;
        seconds = seconds * 60 + v;
    }
    Ticks t = Ticks::fromSeconds(seconds);
    if (milliseconds) {
        if (tail.size() > 3)
            return std::nullopt;
        t = t + Ticks{tail.leftJustified(3, u'0').toLongLong() * kTicksPerSecond / 1000};
    } else {
        const std::int64_t samples = tail.toLongLong();
        if (samples >= audioRate.num / std::max<std::int64_t>(1, audioRate.den))
            return std::nullopt;
        t = t + framesToTicks(samples, audioRate);
    }
    return t;
}

/// `body` (no sign) as a time in the sequence's display units: with audio time units an audio time where it cannot be
/// a timecode, a frame count with the Frames display format, feet+frames (or a frame count) with a feet format, else
/// a timecode (with the sequence's drop-frame labels unless it is an `offset`). nullopt when it is not a time.
std::optional<Ticks> parseTimeEntry(const Sequence& seq, const QString& body, bool offset, bool audioUnits) {
    if (body.isEmpty() || body.front() == u'+' || body.front() == u'-')
        return std::nullopt;
    const Rate rate = seq.frameRate.isValid() ? seq.frameRate : kRate30;
    const bool bare = allDigits(body);
    if (audioUnits) {
        const Rate audio = seq.audioRate.isValid() ? seq.audioRate : kRate48kHz;
        const bool ms = seq.audioDisplay == AudioTimeDisplay::Milliseconds;
        const qsizetype last = lastSeparator(body);
        // The audio reading first where a timecode cannot mean the same: a bare count, a sub-second field wider than
        // frames (samples), a '.' before it (milliseconds).
        const bool audioShaped = bare || (last >= 0 && (ms ? body[last] == u'.' : body.size() - last - 1 > framesDigits(rate)));
        if (audioShaped) {
            if (const auto t = parseAudioTime(body, audio, ms))
                return t;
        }
    }
    if (seq.videoDisplay == VideoTimeDisplay::Frames && bare)
        return body.size() > 7 ? std::nullopt : std::optional<Ticks>(framesToTicks(body.toLongLong(), rate));
    if (seq.videoDisplay == VideoTimeDisplay::Feet16mm || seq.videoDisplay == VideoTimeDisplay::Feet35mm) {
        const std::int64_t perFoot = seq.videoDisplay == VideoTimeDisplay::Feet16mm ? 40 : 16;
        static const QRegularExpression kFeet(QStringLiteral("^(\\d{1,6})\\+(\\d{1,3})$"));
        if (const auto m = kFeet.match(body); m.hasMatch())
            return framesToTicks(m.captured(1).toLongLong() * perFoot + m.captured(2).toLongLong(), rate);
        if (bare)
            return body.size() > 7 ? std::nullopt : std::optional<Ticks>(framesToTicks(body.toLongLong(), rate));
    }
    const bool dropFrame = !offset && seq.dropFrame && supportsDropFrame(rate);
    const std::string fields = filledFields(body).toStdString();
    auto parsed = parseTimecode(fields, rate, dropFrame);
    if (!parsed && dropFrame) {
        // A label drop-frame timecode skips (frames 0 and 1 of nine minutes in ten at 29.97, so "1.." and
        // "00:01:00:00" too): the next label that exists, frame `drop` of that minute, as Premiere does.
        if (const auto frames = parseTimecodeFrames(fields, rate, false); frames && *frames >= 0) {
            const std::int64_t perMinute = timecodeBase(rate) * 60;
            const std::int64_t drop = dropFramesPerMinute(rate);
            const std::int64_t minutes = *frames / perMinute;
            if (*frames % perMinute < drop && minutes % 10 != 0)
                parsed = framesToTicks(minutes * perMinute + drop - drop * (minutes - minutes / 10), rate);
        }
    }
    if (!parsed || *parsed < Ticks{})
        return std::nullopt;
    return *parsed;
}

QString shortName(TrackKind kind, std::size_t index) {
    return (kind == TrackKind::Video ? QStringLiteral("V") : QStringLiteral("A")) + QString::number(index + 1);
}

/// The tracks Delete Empty Tracks removes from `seq`: unlocked tracks of `kind` (nullopt: both) without clips, keeping
/// the lowest-numbered track of a kind whose every track is empty.
std::vector<TrackId> emptyTracksToDelete(const Sequence& seq, std::optional<TrackKind> kind) {
    std::vector<TrackId> out;
    for (const TrackKind k : {TrackKind::Video, TrackKind::Audio}) {
        if (kind && *kind != k)
            continue;
        const auto& tracks = seq.tracks(k);
        std::vector<TrackId> empty;
        for (const TrackPtr& t : tracks) {
            if (!t->locked && t->clips.empty())
                empty.push_back(t->id);
        }
        if (!empty.empty() && empty.size() == tracks.size())
            empty.erase(empty.begin()); // a sequence keeps a track of each kind
        out.insert(out.end(), empty.begin(), empty.end());
    }
    return out;
}

} // namespace

void ProjectController::groupSelection() {
    const std::vector<ClipId> clips = selectedClipIds();
    if (clips.size() < 2)
        return;
    const SequenceId seq = m_sequence;
    applyResult(QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Group"),
                [=](const ProjectState& s) { return edit::groupClips(s, seq, clips); });
    emit selectedClipChanged(); // selectionGrouped
}

void ProjectController::ungroupSelection() {
    const std::vector<ClipId> clips = selectedClipIds();
    const SequencePtr current = currentSequence();
    if (clips.empty() || !current || !edit::anyGrouped(*current, clips))
        return;
    const SequenceId seq = m_sequence;
    applyResult(QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Ungroup"),
                [=](const ProjectState& s) { return edit::ungroupClips(s, seq, clips); });
    emit selectedClipChanged();
}

bool ProjectController::selectionGrouped() const {
    const SequencePtr seq = currentSequence();
    return seq && edit::anyGrouped(*seq, m_selection);
}

void ProjectController::goToSequenceGap(bool forward) {
    const SequencePtr seq = currentSequence();
    if (!seq)
        return;
    if (const auto gap = edit::sequenceGapStart(*seq, m_playhead, forward))
        setPlayhead(*gap);
}

void ProjectController::setSelectionFollowsPlayhead(bool on) {
    if (m_selectionFollowsPlayhead == on)
        return;
    m_selectionFollowsPlayhead = on;
    emit selectionFollowsPlayheadChanged();
    if (on)
        followPlayheadSelection();
}

void ProjectController::followPlayheadSelection() {
    if (!m_selectionFollowsPlayhead)
        return;
    const SequencePtr seq = currentSequence();
    if (!seq)
        return;
    const std::vector<ClipId> under = edit::clipsUnderTime(*seq, m_playhead);
    setSelection(edit::withGroupedClips(*seq, under));
}

// ---- Typed playhead entry (0.2 UX) ----------------------------------------------------------------------------------

std::optional<Ticks> ProjectController::timeEntryTarget(const QString& text) const {
    const SequencePtr seq = currentSequence();
    QString body = text.trimmed();
    if (!seq || body.isEmpty())
        return std::nullopt;
    int sign = 0;
    if (body.front() == u'+' || body.front() == u'-') {
        sign = body.front() == u'+' ? 1 : -1;
        body = body.mid(1).trimmed();
    }
    const auto value = parseTimeEntry(*seq, body, sign != 0, m_audioTimeUnits);
    if (!value)
        return std::nullopt;
    if (sign == 0)
        return *value;
    const Ticks from = m_audioTimeUnits ? m_finePlayhead : m_playhead;
    return std::max(Ticks{}, sign > 0 ? from + *value : from - *value);
}

bool ProjectController::seekToText(const QString& text) {
    const auto target = timeEntryTarget(text);
    if (!target)
        return false;
    setPlayhead(*target);
    return true;
}

QString ProjectController::timeEntryText(const QString& text) const {
    const auto target = timeEntryTarget(text);
    if (!target)
        return {};
    const Ticks bounded = std::min(*target, contentDuration());
    return m_audioTimeUnits ? formatAudioTime(bounded) : formatTimecode(bounded);
}

// Every other time or length typed for the current sequence (Set Duration... of a transition, Clip Speed / Duration,
// caption and marker times) reads with the same reader as the playhead entry, in the sequence's Display Format - what
// formatTimecode() writes: a frame count with Frames, feet+frames (or a frame count) with a feet format, else timecode
// (Premiere's shorthand, drop-frame labels). So a field prefilled from formatTimecode() reads back as it shows. A
// leading '-' is a negative time (callers refuse those).
std::optional<Ticks> ProjectController::parseTimecodeText(const QString& text) const {
    const SequencePtr seq = currentSequence();
    QString body = text.trimmed();
    if (!seq) {
        const auto parsed = parseTimecode(body.toStdString(), kRate30, false);
        return parsed ? std::optional<Ticks>(*parsed) : std::nullopt;
    }
    const bool negative = body.startsWith(u'-');
    if (negative)
        body = body.mid(1).trimmed();
    const auto value = parseTimeEntry(*seq, body, false, false);
    if (!value)
        return std::nullopt;
    return negative ? Ticks{-value->value} : *value;
}

// ---- Gap selection (0.2 UX) -----------------------------------------------------------------------------------------

std::optional<ProjectController::GapSelection> ProjectController::gapAt(TrackId trackId, Ticks at) const {
    const SequencePtr seq = currentSequence();
    const Track* track = seq ? seq->findTrack(trackId) : nullptr;
    if (!track || at < Ticks{} || track->clipIndexAt(at))
        return std::nullopt;
    // edit::rippleDeleteGap's gap: from the clip before (or the sequence start) to the clip after; none after the last.
    const std::size_t next = track->firstClipAtOrAfter(at);
    if (next >= track->clips.size())
        return std::nullopt;
    const Ticks start = next > 0 ? track->clips[next - 1].end() : Ticks{};
    const Ticks end = track->clips[next].start();
    if (end <= start)
        return std::nullopt;
    return GapSelection{trackId, TimeRange::fromStartEnd(start, end)};
}

void ProjectController::setGapSelection(std::optional<GapSelection> gap) {
    if (gap == m_gap)
        return;
    m_gap = gap;
    emit selectedGapChanged();
}

bool ProjectController::selectGapAt(TrackId track, Ticks at) {
    const auto gap = gapAt(track, at);
    if (!gap)
        return false;
    setSelection({});
    if (m_selectedTransition) {
        m_selectedTransition = {};
        emit selectedTransitionChanged();
    }
    setGapSelection(gap);
    return true;
}

void ProjectController::clearGapSelection() {
    setGapSelection(std::nullopt);
}

QVariantMap ProjectController::selectedGap() const {
    const SequencePtr seq = currentSequence();
    const auto position = m_gap && seq ? seq->trackPosition(m_gap->track) : std::nullopt;
    if (!position)
        return {};
    return {
        {QStringLiteral("track"), trackKey(m_gap->track)},
        {QStringLiteral("trackName"), shortName(position->kind, position->index)},
        {QStringLiteral("start"), formatTimecode(m_gap->range.start)},
        {QStringLiteral("end"), formatTimecode(m_gap->range.end())},
        {QStringLiteral("duration"), formatTimecode(m_gap->range.duration)},
    };
}

bool ProjectController::rippleDeleteGap() {
    if (!m_gap)
        return false;
    const GapSelection gap = *m_gap;
    const SequenceId seq = m_sequence;
    return applyResult(QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Ripple Delete"),
                       [=](const ProjectState& s) { return edit::rippleDeleteGap(s, seq, gap.track, gap.range.start); });
}

// ---- Track header menu (0.2 UX) -------------------------------------------------------------------------------------

QVariantMap ProjectController::trackInfo(const QString& trackId) const {
    const auto id = parseTrackKey(trackId);
    const SequencePtr seq = currentSequence();
    const auto position = id && seq ? seq->trackPosition(*id) : std::nullopt;
    const Track* track = position ? seq->findTrack(*id) : nullptr;
    if (!track)
        return {};
    const bool video = position->kind == TrackKind::Video;
    const QString shortText = shortName(position->kind, position->index);
    QString reason;
    if (track->locked)
        reason = tr("Unlock %1 to delete it").arg(shortText);
    else if (seq->tracks(position->kind).size() <= 1)
        reason = video ? tr("A sequence keeps at least one video track") : tr("A sequence keeps at least one audio track");
    const auto emptyVideo = emptyTracksToDelete(*seq, TrackKind::Video).size();
    const auto emptyAudio = emptyTracksToDelete(*seq, TrackKind::Audio).size();
    const QString custom = QString::fromStdString(track->name);
    const int number = static_cast<int>(position->index) + 1;
    return {
        {QStringLiteral("id"), trackKey(track->id)},
        {QStringLiteral("kind"), video ? QStringLiteral("video") : QStringLiteral("audio")},
        {QStringLiteral("shortName"), shortText},
        {QStringLiteral("name"), !custom.isEmpty() ? custom : video ? tr("Video %1").arg(number) : tr("Audio %1").arg(number)},
        {QStringLiteral("customName"), custom},
        {QStringLiteral("index"), static_cast<int>(position->index)},
        {QStringLiteral("clipCount"), static_cast<int>(track->clips.size())},
        {QStringLiteral("locked"), track->locked},
        {QStringLiteral("syncLocked"), track->syncLocked},
        {QStringLiteral("targeted"), track->targeted},
        {QStringLiteral("canDelete"), reason.isEmpty()},
        {QStringLiteral("deleteReason"), reason},
        {QStringLiteral("emptyTracks"), static_cast<int>(emptyVideo + emptyAudio)},
        {QStringLiteral("emptyVideoTracks"), static_cast<int>(emptyVideo)},
        {QStringLiteral("emptyAudioTracks"), static_cast<int>(emptyAudio)},
    };
}

bool ProjectController::renameTrack(const QString& trackId, const QString& name) {
    const auto id = parseTrackKey(trackId);
    const SequencePtr seq = currentSequence();
    if (!id || !seq || !seq->trackPosition(*id))
        return false;
    const QString trimmed = name.trimmed();
    if (trimmed.size() > kMaxTrackName) {
        setError(tr("A track name has at most %1 characters").arg(kMaxTrackName));
        return false;
    }
    edit::TrackStateChange change;
    change.name = trimmed.toStdString();
    const SequenceId seqId = m_sequence;
    const TrackId track = *id;
    return applyEditFn(QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Rename Track"),
                       [&](const ProjectState& state) { return edit::setTrackState(state, seqId, track, change); });
}

QStringList ProjectController::addTracks(const QVariantMap& spec) {
    const SequencePtr seq = currentSequence();
    if (!seq)
        return {};
    bool videoOk = true;
    bool audioOk = true;
    const int video = spec.value(QStringLiteral("video"), 0).toInt(&videoOk);
    const int audio = spec.value(QStringLiteral("audio"), 0).toInt(&audioOk);
    if (!videoOk || !audioOk || video < 0 || audio < 0 || video > kMaxTracksPerAdd || audio > kMaxTracksPerAdd) {
        setError(tr("Add from 0 to %1 tracks of each kind").arg(kMaxTracksPerAdd));
        return {};
    }
    std::optional<ChannelFormat> format;
    if (const QString text = spec.value(QStringLiteral("audioFormat")).toString(); !text.isEmpty()) {
        format = channelFormatFromString(text.toStdString());
        if (!format) {
            setError(tr("Unknown audio track format: %1").arg(text));
            return {};
        }
    }
    // The track "after" / "before" refer to: `videoTrack` / `audioTrack` for one kind (the Add Tracks dialog places each
    // kind after its own track in one undo step), else `track` for both.
    const auto referenceFor = [&](TrackKind kind) {
        const QString own = spec.value(kind == TrackKind::Video ? QStringLiteral("videoTrack") : QStringLiteral("audioTrack"))
                                .toString();
        return parseTrackKey(own.isEmpty() ? spec.value(QStringLiteral("track")).toString() : own);
    };
    // Where the tracks of `kind` go: an index among that kind's tracks (0 = V1 / A1), or nullopt for a bad placement.
    const auto indexFor = [&](TrackKind kind, const QString& placement) -> std::optional<std::size_t> {
        const auto& tracks = seq->tracks(kind);
        const auto reference = referenceFor(kind);
        if (placement.isEmpty() || placement == QLatin1String("last"))
            return tracks.size();
        if (placement == QLatin1String("first"))
            return std::size_t{0};
        if (placement != QLatin1String("after") && placement != QLatin1String("before"))
            return std::nullopt;
        std::optional<std::size_t> at;
        if (reference) {
            if (const auto position = seq->trackPosition(*reference); position && position->kind == kind)
                at = position->index;
        }
        for (std::size_t i = tracks.size(); !at && i-- > 0;) {
            if (tracks[i]->targeted)
                at = i; // the highest-numbered targeted track (Premiere's "After Target Track")
        }
        if (!at)
            at = tracks.empty() ? std::size_t{0} : tracks.size() - 1;
        return placement == QLatin1String("after") ? std::min(*at + 1, tracks.size()) : *at;
    };
    const QString videoPlacement = spec.value(QStringLiteral("videoPlacement")).toString();
    const QString audioPlacement = spec.value(QStringLiteral("audioPlacement")).toString();
    const auto videoAt = indexFor(TrackKind::Video, videoPlacement);
    const auto audioAt = indexFor(TrackKind::Audio, audioPlacement);
    if ((video > 0 && !videoAt) || (audio > 0 && !audioAt)) {
        setError(tr("Unknown track placement: %1").arg(video > 0 && !videoAt ? videoPlacement : audioPlacement));
        return {};
    }
    if (video == 0 && audio == 0)
        return {};
    const SequenceId seqId = m_sequence;
    std::vector<TrackId> added;
    const char* label = video + audio == 1 ? QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Add Track")
                                           : QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Add Tracks");
    auto outcome = m_session->apply(label, [&](const ProjectState& state) -> std::expected<ProjectState, Error> {
        added.clear();
        ProjectState working = state;
        for (int i = 0; i < video; ++i) {
            auto created = edit::insertTrack(working, seqId, TrackKind::Video, *videoAt + static_cast<std::size_t>(i));
            if (!created)
                return std::unexpected(created.error());
            working = std::move(created->state);
            added.push_back(created->id);
        }
        for (int i = 0; i < audio; ++i) {
            auto created = edit::insertTrack(working, seqId, TrackKind::Audio, *audioAt + static_cast<std::size_t>(i));
            if (!created)
                return std::unexpected(created.error());
            working = std::move(created->state);
            added.push_back(created->id);
            if (format && *format != ChannelFormat::Stereo) {
                edit::TrackStateChange change;
                change.format = *format;
                auto formatted = edit::setTrackState(working, seqId, created->id, change);
                if (!formatted)
                    return std::unexpected(formatted.error());
                working = std::move(formatted->state);
            }
        }
        return working;
    });
    if (!outcome) {
        setError(QString::fromStdString(outcome.error().message));
        return {};
    }
    setError({});
    QStringList keys;
    for (const TrackId id : added)
        keys << trackKey(id);
    return keys;
}

bool ProjectController::deleteTrack(const QString& trackId) {
    const QVariantMap info = trackInfo(trackId);
    if (info.isEmpty())
        return false;
    if (!info.value(QStringLiteral("canDelete")).toBool()) {
        setError(info.value(QStringLiteral("deleteReason")).toString());
        return false;
    }
    const SequenceId seq = m_sequence;
    const TrackId track = *parseTrackKey(trackId);
    return applyEditFn(QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Delete Track"),
                       [&](const ProjectState& state) { return edit::removeTrack(state, seq, track); });
}

int ProjectController::deleteEmptyTracks(const QString& kind) {
    const SequencePtr current = currentSequence();
    if (!current)
        return 0;
    std::optional<TrackKind> only;
    if (kind == QLatin1String("video"))
        only = TrackKind::Video;
    else if (kind == QLatin1String("audio"))
        only = TrackKind::Audio;
    else if (!kind.isEmpty())
        return 0;
    const std::vector<TrackId> candidates = emptyTracksToDelete(*current, only);
    if (candidates.empty())
        return 0;
    const SequenceId seq = m_sequence;
    int removed = 0;
    auto outcome = m_session->apply(QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Delete Empty Tracks"),
                                    [&](const ProjectState& state) -> std::expected<ProjectState, Error> {
                                        removed = 0;
                                        ProjectState working = state;
                                        for (const TrackId id : candidates) {
                                            // A track the model keeps (a multicam angle's, one nested clips play) stays.
                                            auto result = edit::removeTrack(working, seq, id);
                                            if (!result)
                                                continue;
                                            working = std::move(result->state);
                                            ++removed;
                                        }
                                        if (removed == 0)
                                            return std::unexpected(Error{ErrorCode::InvalidArgument, "no empty track can be deleted"});
                                        return working;
                                    });
    if (!outcome) {
        setError(QString::fromStdString(outcome.error().message));
        return 0;
    }
    setError({});
    return removed;
}

} // namespace wabcut::app
