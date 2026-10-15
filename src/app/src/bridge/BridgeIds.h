#pragma once

// Opaque ids for QML (docs/architecture.md, principle 9): uint64 model ids never cross into QML as numbers.
// QML sees short strings ("c42" = clip 42, "t7" = track 7, "s3" = sequence 3, "x5" = transition 5, "e9" = effect
// instance 9) that it only compares and passes back; the bridge parses them again.

#include <wabcut/model/Ids.h>

#include <QString>

#include <optional>

namespace wabcut::app {

namespace detail {

template <class IdT>
QString formatId(QChar prefix, IdT id) {
    return id.isValid() ? QString(prefix) + QString::number(id.value) : QString();
}

template <class IdT>
std::optional<IdT> parseId(QChar prefix, const QString& text) {
    if (text.size() < 2 || text.front() != prefix)
        return std::nullopt;
    bool ok = false;
    const quint64 value = QStringView(text).mid(1).toULongLong(&ok);
    if (!ok || value == 0)
        return std::nullopt;
    return IdT{value};
}

} // namespace detail

inline QString clipKey(ClipId id) { return detail::formatId(QLatin1Char('c'), id); }
inline QString trackKey(TrackId id) { return detail::formatId(QLatin1Char('t'), id); }
inline QString sequenceKey(SequenceId id) { return detail::formatId(QLatin1Char('s'), id); }
inline QString mediaKey(MediaId id) { return detail::formatId(QLatin1Char('m'), id); }
inline QString transitionKey(TransitionId id) { return detail::formatId(QLatin1Char('x'), id); }
inline QString effectKey(EffectInstanceId id) { return detail::formatId(QLatin1Char('e'), id); }
inline QString captionTrackKey(CaptionTrackId id) { return detail::formatId(QLatin1Char('k'), id); }
inline QString speakerKey(SpeakerId id) { return detail::formatId(QLatin1Char('v'), id); }

inline std::optional<ClipId> parseClipKey(const QString& text) { return detail::parseId<ClipId>(QLatin1Char('c'), text); }
inline std::optional<TrackId> parseTrackKey(const QString& text) {
    return detail::parseId<TrackId>(QLatin1Char('t'), text);
}
inline std::optional<SequenceId> parseSequenceKey(const QString& text) {
    return detail::parseId<SequenceId>(QLatin1Char('s'), text);
}
inline std::optional<TransitionId> parseTransitionKey(const QString& text) {
    return detail::parseId<TransitionId>(QLatin1Char('x'), text);
}
inline std::optional<EffectInstanceId> parseEffectKey(const QString& text) {
    return detail::parseId<EffectInstanceId>(QLatin1Char('e'), text);
}
inline std::optional<CaptionTrackId> parseCaptionTrackKey(const QString& text) {
    return detail::parseId<CaptionTrackId>(QLatin1Char('k'), text);
}
inline std::optional<SpeakerId> parseSpeakerKey(const QString& text) {
    return detail::parseId<SpeakerId>(QLatin1Char('v'), text);
}

} // namespace wabcut::app
