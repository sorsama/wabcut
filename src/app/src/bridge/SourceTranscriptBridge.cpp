#include "SourceTranscriptBridge.h"

#include "ProjectController.h"
#include "SourceMonitorBridge.h"

#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/command/SourceServices.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/Timecode.h>

#include <QClipboard>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QQmlEngine>
#include <QStringList>

#include <algorithm>

namespace wabcut::app {

namespace {

/// A new paragraph after a pause this long, or after this many words (TranscriptBridge's paragraphs).
constexpr Ticks kParagraphPause = Ticks::fromSeconds(2);
constexpr std::size_t kParagraphWords = 80;

QString fromStd(std::string_view text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }

} // namespace

SourceTranscriptBridge::SourceTranscriptBridge(QObject* parent)
    : QObject(parent), m_paragraphs(new TranscriptParagraphModel(this)) {
    m_marksTimer.setSingleShot(true);
    connect(&m_marksTimer, &QTimer::timeout, this, [this] { applyMarks(); });
}

SourceTranscriptBridge::~SourceTranscriptBridge() = default;

SourceTranscriptBridge* SourceTranscriptBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* bridge = new SourceTranscriptBridge;
    if (qmlEngine) {
        bridge->setController(qmlEngine->singletonInstance<ProjectController*>("Wabcut", "ProjectController"));
        bridge->setTranscriptBridge(qmlEngine->singletonInstance<TranscriptBridge*>("Wabcut", "Transcript"));
        bridge->setSourceMonitor(qmlEngine->singletonInstance<SourceMonitorBridge*>("Wabcut", "SourceMonitor"));
    }
    return bridge;
}

void SourceTranscriptBridge::setController(ProjectController* controller) {
    if (m_controller == controller)
        return;
    if (m_controller)
        disconnect(m_controller, nullptr, this, nullptr);
    m_controller = controller;
    if (m_controller) {
        // Corrections, redactions and relinks change what the words read; the marks this bridge sets change nothing
        // here (rebuild keeps the selection when the words are the same).
        connect(m_controller, &ProjectController::revisionChanged, this, &SourceTranscriptBridge::rebuild);
        // Another project: TranscriptBridge looks its transcripts up again first.
        connect(m_controller, &ProjectController::mediaChanged, this, &SourceTranscriptBridge::rebuild, Qt::QueuedConnection);
    }
    rebuild();
}

void SourceTranscriptBridge::setSourceMonitor(SourceMonitorBridge* source) {
    if (m_source == source)
        return;
    if (m_source)
        disconnect(m_source, nullptr, this, nullptr);
    m_source = source;
    if (m_source) {
        connect(m_source, &SourceMonitorBridge::sourceChanged, this, &SourceTranscriptBridge::rebuild);
        connect(m_source, &SourceMonitorBridge::positionChanged, this, &SourceTranscriptBridge::updateCurrentWord);
    }
    rebuild();
}

void SourceTranscriptBridge::setTranscriptBridge(TranscriptBridge* transcript) {
    if (m_transcript == transcript)
        return;
    if (m_transcript)
        disconnect(m_transcript, nullptr, this, nullptr);
    m_transcript = transcript;
    if (m_transcript) {
        // A transcription finished (its transcripts join the session's), or the cache was looked up again.
        connect(m_transcript, &TranscriptBridge::transcriptChanged, this, &SourceTranscriptBridge::rebuild);
    }
    rebuild();
}

std::shared_ptr<ai::edit::TranscriptProvider> SourceTranscriptBridge::provider() const {
    if (!m_transcript || !m_transcript->services())
        return nullptr;
    return m_transcript->services()->transcripts;
}

// ---- The source and its words ---------------------------------------------------------------------------------

QString SourceTranscriptBridge::state() const {
    if (!m_source || !m_source->loaded())
        return QStringLiteral("none");
    if (m_source->preview())
        return QStringLiteral("preview");
    return m_streams.isEmpty() ? QStringLiteral("noAudio") : QStringLiteral("ready");
}

QString SourceTranscriptBridge::mediaName() const { return m_source && m_source->loaded() ? m_source->name() : QString(); }

QString SourceTranscriptBridge::language() const { return fromStd(m_words.language()); }

int SourceTranscriptBridge::defaultStream(const MediaItem& item) const {
    if (const auto it = m_streamChoice.find(item.id.value); it != m_streamChoice.end())
        return it->second;
    const auto chain = provider();
    for (int s = 0; chain && s < static_cast<int>(item.info.audio.size()); ++s) {
        if (chain->transcript(item, s))
            return s;
    }
    // The creator's voice in a multi-track recording (Mic, Voice, Commentary), not the mix, the game or voice chat.
    const std::vector<edit::SourceTrackName> names = edit::sourceTrackNames(item.info);
    for (std::size_t s = 0; s < names.size(); ++s) {
        if (names[s].role == TrackRole::Dialogue && names[s].name.rfind("Mic", 0) == 0)
            return static_cast<int>(s);
    }
    return 0;
}

void SourceTranscriptBridge::rebuildStreams(const MediaItem* item) {
    QVariantList list;
    if (item && !item->isGenerator()) {
        const auto chain = provider();
        const std::vector<edit::SourceTrackName> names = edit::sourceTrackNames(item->info);
        for (std::size_t s = 0; s < item->info.audio.size(); ++s) {
            QString label = QStringLiteral("A%1").arg(static_cast<int>(s) + 1);
            if (s < names.size() && !names[s].name.empty())
                label += QStringLiteral(" · ") + QString::fromStdString(names[s].name);
            list.append(QVariantMap{{QStringLiteral("index"), static_cast<int>(s)},
                                    {QStringLiteral("label"), label},
                                    {QStringLiteral("hasTranscript"), chain && chain->transcript(*item, static_cast<int>(s)) != nullptr}});
        }
    }
    m_streams = std::move(list);
}

void SourceTranscriptBridge::rebuild() { refresh(false); }

void SourceTranscriptBridge::refresh(bool force) {
    const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : nullptr;
    const MediaId media = m_source && m_source->loaded() && !m_source->preview() ? m_source->mediaId() : MediaId{};
    const MediaItem* item = snap && media.isValid() ? snap->findMedia(media) : nullptr;
    const bool newSource = media != m_media;
    if (newSource) {
        m_media = media;
        m_stream = item ? defaultStream(*item) : 0;
        setMessage({});
    }
    if (item)
        m_rate = command::sourceTimecodeRate(*snap, *item);
    const QVariantList streamsBefore = m_streams;
    rebuildStreams(item);
    if (m_stream >= static_cast<int>(m_streams.size()))
        m_stream = 0;
    const auto chain = provider();
    ai::edit::MediaTranscript next = item && chain ? ai::edit::MediaTranscript::build(*snap, media, m_stream, *chain)
                                                   : ai::edit::MediaTranscript{};
    if (newSource || force || m_streams != streamsBefore)
        emit sourceChanged();
    // The marks this bridge sets (and any other edit that leaves the words as they read) keep the selection.
    if (!newSource && !force && next.sameAs(m_words))
        return;
    m_words = std::move(next);
    // Other words (another clip or stream, a correction, a redaction, a transcription): the run means nothing now.
    m_range.reset();
    m_anchor.reset();
    m_marksPending = false;
    m_marksTimer.stop();
    rebuildParagraphs();
    search();
    updateCurrentWord();
    bumpMarks();
    emit transcriptChanged();
}

void SourceTranscriptBridge::setStream(int stream) {
    if (stream == m_stream || stream < 0 || stream >= static_cast<int>(m_streams.size()))
        return;
    m_stream = stream;
    if (m_media.isValid())
        m_streamChoice[m_media.value] = stream;
    refresh(true);
}

QString SourceTranscriptBridge::timecodeOf(Ticks t) const { return QString::fromStdString(ticksToTimecode(t, m_rate, false)); }

void SourceTranscriptBridge::rebuildParagraphs() {
    std::vector<TranscriptParagraphModel::Paragraph> rows;
    const std::size_t n = m_words.size();
    for (std::size_t i = 0; i < n; ++i) {
        const ai::edit::MediaWord& w = m_words[i];
        const ai::edit::MediaWord* previous = rows.empty() ? nullptr : &m_words[static_cast<std::size_t>(rows.back().last)];
        const bool start = !previous || w.speaker != previous->speaker || w.source.start - previous->source.end() >= kParagraphPause
                        || static_cast<std::size_t>(rows.back().last - rows.back().first + 1) >= kParagraphWords;
        if (start) {
            TranscriptParagraphModel::Paragraph p;
            p.first = static_cast<int>(i);
            p.speakerIndex = w.speaker.value_or(-1);
            p.speaker = w.speaker ? tr("Speaker %1").arg(*w.speaker + 1) : QString();
            p.timeText = timecodeOf(w.source.start);
            rows.push_back(std::move(p));
        }
        TranscriptParagraphModel::Paragraph& p = rows.back();
        p.last = static_cast<int>(i);
        // A space after the word when its language writes one before the next word (never in Thai).
        std::string text(m_words.text(i));
        if (i + 1 < n) {
            const std::string pair = m_words.textOf(i, i + 1);
            if (pair.size() > text.size() + m_words.text(i + 1).size())
                text += ' ';
        }
        p.words.append(QVariantMap{{QStringLiteral("i"), static_cast<int>(i)}, {QStringLiteral("text"), fromStd(text)}});
    }
    m_paragraphs->reset(std::move(rows));
}

void SourceTranscriptBridge::updateCurrentWord() {
    int current = -1;
    if (m_source && !m_words.empty()) {
        const Ticks t = m_source->positionTicks();
        // Fix round 1: a zero-length word is current on the frame that holds it (the position is on frames).
        if (const auto i = m_words.wordAt(t); i) {
            const TimeRange r = m_words[*i].source;
            if (r.contains(t) || (r.isEmpty() && snapToFrame(r.start, m_rate) == snapToFrame(t, m_rate)))
                current = static_cast<int>(*i);
        }
    }
    if (current != m_currentWord) {
        m_currentWord = current;
        emit currentWordChanged();
    }
}

// ---- Selection and marks --------------------------------------------------------------------------------------

void SourceTranscriptBridge::bumpMarks() {
    ++m_markRevision;
    emit marksChanged();
}

void SourceTranscriptBridge::setMessage(const QString& message) {
    if (message == m_message)
        return;
    m_message = message;
    emit messageChanged();
}

bool SourceTranscriptBridge::isSelected(int word) const {
    return word >= 0 && m_range && static_cast<std::size_t>(word) >= m_range->first && static_cast<std::size_t>(word) <= m_range->second;
}

bool SourceTranscriptBridge::isMatch(int word) const {
    return word >= 0 && m_matchWords.contains(static_cast<std::size_t>(word));
}

QString SourceTranscriptBridge::selectionInfo() const {
    if (!m_range)
        return {};
    const auto count = static_cast<int>(m_range->second - m_range->first + 1);
    const Ticks length = m_words[m_range->second].source.end() - m_words[m_range->first].source.start;
    return tr("%n word(s)", nullptr, count) + QStringLiteral(" · ") + timecodeOf(std::max(length, Ticks{}));
}

void SourceTranscriptBridge::setRange(std::size_t first, std::size_t last, std::size_t anchor) {
    m_range = std::pair{std::min(first, last), std::max(first, last)};
    m_anchor = anchor;
    m_marksPending = true;
    if (m_marksDelay <= 0)
        applyMarks();
    else
        m_marksTimer.start(m_marksDelay);
    bumpMarks();
}

void SourceTranscriptBridge::seekToSelection() {
    if (!m_source || !m_range)
        return;
    m_source->seek(seekPointIn(m_words[m_range->first].source, m_rate));
}

void SourceTranscriptBridge::clickWord(int word, int modifiers) {
    if (word < 0 || static_cast<std::size_t>(word) >= m_words.size())
        return;
    const auto w = static_cast<std::size_t>(word);
    const auto mods = Qt::KeyboardModifiers(modifiers);
    // One run of words: Shift and Ctrl (Cmd) both extend it from the anchor (marks are one range).
    if (mods.testAnyFlags(Qt::ShiftModifier | Qt::ControlModifier | Qt::MetaModifier) && m_anchor && *m_anchor < m_words.size())
        setRange(*m_anchor, w, *m_anchor);
    else
        setRange(w, w, w);
    seekToSelection();
}

void SourceTranscriptBridge::selectWords(int first, int last) {
    const int n = static_cast<int>(m_words.size());
    if (n == 0)
        return;
    const auto a = static_cast<std::size_t>(std::clamp(first, 0, n - 1));
    const auto b = static_cast<std::size_t>(std::clamp(last, 0, n - 1));
    const bool moved = !m_range || std::min(a, b) != m_range->first;
    setRange(a, b, a);
    if (moved)
        seekToSelection();
}

void SourceTranscriptBridge::clearSelection() {
    if (!m_range)
        return;
    m_range.reset();
    m_anchor.reset();
    m_marksPending = false;
    m_marksTimer.stop();
    bumpMarks();
}

QString SourceTranscriptBridge::applyMarks(bool force) {
    m_marksTimer.stop();
    if ((!m_marksPending && !force) || !m_range || !m_source)
        return {};
    // In at the start of the first word's frame, Out at the end of the last word's (SourceMonitorBridge snaps them).
    const Ticks in = m_words[m_range->first].source.start;
    const Ticks out = std::max(m_words[m_range->second].source.end(), in + Ticks{1});
    if (!m_source->setMarks(in, out)) {
        m_marksPending = true; // fix round 1: still waiting, so the next apply tries again
        setMessage(m_source->message());
        return m_message;
    }
    m_marksPending = false;
    setMessage({});
    return {};
}

int SourceTranscriptBridge::selectionFirst() const { return m_range ? static_cast<int>(m_range->first) : -1; }

int SourceTranscriptBridge::paragraphOfWord(int word) const {
    const auto& rows = m_paragraphs->paragraphs();
    if (word < 0 || rows.empty())
        return -1;
    const auto it = std::lower_bound(rows.begin(), rows.end(), word,
                                     [](const TranscriptParagraphModel::Paragraph& p, int w) { return p.last < w; });
    if (it == rows.end() || it->first > word)
        return -1;
    return static_cast<int>(it - rows.begin());
}

QString SourceTranscriptBridge::selectionSearchText() const {
    return m_range ? fromStd(m_words.textOf(m_range->first, m_range->second)) : QString();
}

QString SourceTranscriptBridge::copySelection(bool withTimecodes) {
    if (!m_range) {
        setMessage(tr("Select words first"));
        return m_message;
    }
    QStringList lines;
    if (!withTimecodes) {
        lines << fromStd(m_words.textOf(m_range->first, m_range->second));
    } else {
        // One line per paragraph part: "[00:00:01:02] Speaker 1: text" (no speaker when it is not known).
        for (const TranscriptParagraphModel::Paragraph& p : m_paragraphs->paragraphs()) {
            if (p.last < 0)
                continue;
            const std::size_t a = std::max(m_range->first, static_cast<std::size_t>(std::max(p.first, 0)));
            const std::size_t b = std::min(m_range->second, static_cast<std::size_t>(p.last));
            if (a > b)
                continue;
            QString line = QLatin1Char('[') + timecodeOf(m_words[a].source.start) + QStringLiteral("] ");
            if (!p.speaker.isEmpty())
                line += p.speaker + QStringLiteral(": ");
            lines << line + fromStd(m_words.textOf(a, b));
        }
    }
    QClipboard* clipboard = qobject_cast<QGuiApplication*>(QCoreApplication::instance()) ? QGuiApplication::clipboard() : nullptr;
    if (!clipboard) {
        setMessage(tr("The clipboard is not available"));
        return m_message;
    }
    clipboard->setText(lines.join(QLatin1Char('\n')));
    setMessage({});
    return {};
}

// ---- Search -------------------------------------------------------------------------------------------------------

void SourceTranscriptBridge::setSearchQuery(const QString& query) {
    if (query == m_query)
        return;
    m_query = query;
    search();
    bumpMarks();
}

void SourceTranscriptBridge::search() {
    m_matches.clear();
    m_matchWords.clear();
    m_matchIndex = -1;
    const QString needle = m_query.trimmed().toCaseFolded();
    if (!needle.isEmpty() && !m_words.empty()) {
        // The text as shown, with each word's character offset (TranscriptBridge::search): a query can span words,
        // and Thai (no spaces) matches inside runs of word units.
        QString text;
        std::vector<int> starts;
        for (const auto& p : m_paragraphs->paragraphs()) {
            for (const QVariant& w : p.words) {
                starts.push_back(static_cast<int>(text.size()));
                text += w.toMap().value(QStringLiteral("text")).toString().toCaseFolded();
            }
            text += QLatin1Char('\n');
        }
        for (qsizetype at = text.indexOf(needle); at >= 0; at = text.indexOf(needle, at + needle.size())) {
            const auto first = std::upper_bound(starts.begin(), starts.end(), static_cast<int>(at)) - starts.begin() - 1;
            const auto last = std::upper_bound(starts.begin(), starts.end(), static_cast<int>(at + needle.size() - 1))
                            - starts.begin() - 1;
            if (first < 0)
                continue;
            m_matches.push_back({static_cast<std::size_t>(first), static_cast<std::size_t>(last)});
            for (auto i = first; i <= last; ++i)
                m_matchWords.insert(static_cast<std::size_t>(i));
        }
    }
    emit searchChanged();
}

void SourceTranscriptBridge::nextMatch(int direction) {
    if (m_matches.empty())
        return;
    const int n = static_cast<int>(m_matches.size());
    m_matchIndex = m_matchIndex < 0 ? (direction < 0 ? n - 1 : 0) : ((m_matchIndex + (direction < 0 ? -1 : 1)) % n + n) % n;
    const ai::edit::WordRange r = m_matches[static_cast<std::size_t>(m_matchIndex)];
    setRange(r.first, r.last, r.first);
    seekToSelection();
    emit searchChanged();
}

// ---- Transcription ------------------------------------------------------------------------------------------------

QString SourceTranscriptBridge::transcribe(bool allowOnce) {
    if (!m_transcript || !m_media.isValid() || m_streams.isEmpty()) {
        setMessage(tr("Load a clip with sound in the Source Monitor first"));
        return m_message;
    }
    const QString result = m_transcript->transcribeMedia(m_media, m_stream, allowOnce);
    setMessage(result);
    return result;
}

QVariantMap SourceTranscriptBridge::consentFor() const { return m_transcript ? m_transcript->consentForMedia() : QVariantMap{}; }

} // namespace wabcut::app
