#include "TranscriptBridge.h"

#include "AiController.h"
#include "ProjectController.h"

#include <wabcut/ai/AnalysisCache.h>
#include <wabcut/ai/Capability.h>
#include <wabcut/ai/TranscribeService.h>
#include <wabcut/ai/TranscriptRedaction.h>
#include <wabcut/ai/TranscriptJson.h>
#include <wabcut/ai/TranscriptionService.h>
#include <wabcut/ai/edit/CaptionBuilder.h>
#include <wabcut/ai/edit/MediaServices.h>
#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/ai/edit/TranscriptSources.h>
#include <wabcut/command/Engine.h>
#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/MediaLocator.h>

#include <QClipboard>
#include <QCoreApplication>
#include <QDir>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlEngine>
#include <QSettings>
#include <QStandardPaths>
#include <QTextBoundaryFinder>

#include <algorithm>
#include <optional>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace wabcut::app {

namespace {

using command::Json;

/// A new paragraph after a pause this long.
constexpr Ticks kParagraphPause = Ticks::fromSeconds(2);
constexpr std::size_t kParagraphWords = 80;

QString fromStd(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

Json rangesJson(const std::vector<ai::edit::WordRange>& ranges) {
    Json out = Json::array();
    for (const ai::edit::WordRange& r : ranges)
        out.push_back(Json::array({r.first, r.last}));
    return out;
}

// ---- 0.4: voice tracks (transcribe-voice-tracks) ----

/// QSettings key of the remembered track choices: a JSON object of lower-case track name -> bool (C++ only).
constexpr const char* kTrackChoicesKey = "transcript/trackChoices";

/// A track name for matching: lower case, letters and digits only ("Mic/Aux 2" -> "micaux2").
QString matchName(const std::string& name) {
    QString out;
    for (const QChar ch : QString::fromStdString(name).toLower()) {
        if (ch.isLetterOrNumber())
            out += ch;
    }
    return out;
}

/// Voice chat (Discord, party chat, TeamSpeak: the names sourceTrackName gives them): friends' voices, left out.
bool isVoiceChatName(const QString& n) {
    return n.startsWith(QLatin1String("discord")) || n.startsWith(QLatin1String("voicechat"))
        || n.startsWith(QLatin1String("partychat")) || n.startsWith(QLatin1String("teamspeak"))
        || n.startsWith(QLatin1String("ts3")) || n.startsWith(QLatin1String("chat"));
}

/// A creator's own voice by name: Mic, Microphone, Voice, Commentary, Narration, VO.
bool isVoiceName(const QString& n) {
    return n.startsWith(QLatin1String("mic")) || (n.startsWith(QLatin1String("voice")) && !isVoiceChatName(n))
        || n.startsWith(QLatin1String("commentary")) || n.startsWith(QLatin1String("narration"))
        || n == QLatin1String("vo") || n.startsWith(QLatin1String("voiceover"));
}

/// The key a track's choice is remembered under: its name in lower case (OBS recordings repeat their names), else its
/// position ("a2") for an unnamed track.
QString choiceKey(const Track& track, int index) {
    const QString name = QString::fromStdString(track.name).trimmed().toLower();
    return name.isEmpty() ? QStringLiteral("a%1").arg(index + 1) : name;
}

QJsonObject storedTrackChoices() {
    const QByteArray text = QSettings().value(QLatin1String(kTrackChoicesKey)).toString().toUtf8();
    const QJsonDocument doc = QJsonDocument::fromJson(text);
    return doc.isObject() ? doc.object() : QJsonObject();
}

/// The default choice: the voice tracks (Dialogue role or a voice name), unmuted and not voice chat.
bool defaultVoice(const Track& track) {
    const QString n = matchName(track.name);
    return !track.muted && !isVoiceChatName(n) && (track.role == TrackRole::Dialogue || isVoiceName(n));
}
} // namespace

Ticks seekPointIn(const TimeRange& range, Rate rate) {
    const Ticks snapped = snapToFrame(range.start, rate);
    const Ticks next = snapped + framesToTicks(1, rate);
    return snapped < range.start && next < range.end() ? next : range.start;
}

// ---- Paragraph model --------------------------------------------------------------------------------

int TranscriptParagraphModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
}

QHash<int, QByteArray> TranscriptParagraphModel::roleNames() const {
    return {{SpeakerRole, "speaker"}, {TimeRole, "timeText"}, {FirstRole, "first"},
            {LastRole, "last"},       {WordsRole, "words"},   {SpeakerIndexRole, "speakerIndex"}};
}

QVariant TranscriptParagraphModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount())
        return {};
    const Paragraph& p = m_rows[static_cast<std::size_t>(index.row())];
    switch (role) {
    case SpeakerRole: return p.speaker;
    case TimeRole: return p.timeText;
    case FirstRole: return p.first;
    case LastRole: return p.last;
    case WordsRole: return p.words;
    case SpeakerIndexRole: return p.speakerIndex;
    default: return {};
    }
}

QVariantMap TranscriptParagraphModel::get(int row) const {
    if (row < 0 || row >= rowCount())
        return {};
    const Paragraph& p = m_rows[static_cast<std::size_t>(row)];
    return {{QStringLiteral("speaker"), p.speaker}, {QStringLiteral("timeText"), p.timeText},
            {QStringLiteral("first"), p.first},     {QStringLiteral("last"), p.last},
            {QStringLiteral("words"), p.words}};
}

void TranscriptParagraphModel::reset(std::vector<Paragraph> paragraphs) {
    const int before = rowCount();
    beginResetModel();
    m_rows = std::move(paragraphs);
    endResetModel();
    if (rowCount() != before)
        emit countChanged();
}

// ---- Bridge -----------------------------------------------------------------------------------------

TranscriptBridge::TranscriptBridge(QObject* parent)
    : QObject(parent), m_paragraphs(new TranscriptParagraphModel(this)),
      m_session(std::make_shared<ai::edit::MapTranscriptProvider>()) {
    setUseAnalysisCache(true);
}

TranscriptBridge::~TranscriptBridge() {
    cancelReview();
    m_reviewWorkers.clear();
    if (m_job.joinable()) {
        m_job.request_stop();
        m_job.join();
    }
}

TranscriptBridge* TranscriptBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* bridge = new TranscriptBridge;
    if (qmlEngine) {
        bridge->setController(qmlEngine->singletonInstance<ProjectController*>("Wabcut", "ProjectController"));
        bridge->setAiController(qmlEngine->singletonInstance<AiController*>("Wabcut", "AiController"));
    }
    return bridge;
}

void TranscriptBridge::setUseAnalysisCache(bool use) {
    m_useCache = use;
    m_chain = std::make_shared<ai::edit::TranscriptProviderChain>();
    m_chain->add(m_session);
    if (use) {
        const std::filesystem::path project = m_controller ? m_controller->projectFile() : std::filesystem::path();
        m_chain->add(std::make_shared<ai::edit::CachedTranscriptProvider>(ai::AnalysisCache::defaultRoot(), project));
    }
    m_services = std::make_shared<ai::edit::TranscriptServices>(m_chain);
    if (m_controller)
        rebuild();
}

void TranscriptBridge::setController(ProjectController* controller) {
    if (m_controller == controller)
        return;
    cancelReview();
    if (m_controller)
        disconnect(m_controller, nullptr, this, nullptr);
    m_controller = controller;
    if (m_controller) {
        connect(m_controller, &ProjectController::revisionChanged, this, &TranscriptBridge::onModelChanged);
        connect(m_controller, &ProjectController::playheadChanged, this, &TranscriptBridge::updateCurrentWord);
        connect(m_controller, &ProjectController::selectionChanged, this, &TranscriptBridge::cancelReview);
        connect(m_controller, &ProjectController::sequenceChanged, this, &TranscriptBridge::cancelReview);
        // Another project resolves media against another file: look the cache up again.
        connect(m_controller, &ProjectController::mediaChanged, this, [this] { setUseAnalysisCache(m_useCache); });
    }
    setUseAnalysisCache(m_useCache);
}

void TranscriptBridge::setAiController(AiController* ai) {
    if (m_ai == ai)
        return;
    if (m_ai)
        disconnect(m_ai, nullptr, this, nullptr);
    m_ai = ai;
    if (m_ai) {
        connect(m_ai, &AiController::modelsChanged, this, &TranscriptBridge::providersChanged);
        // A cloud transcription job holds the platform (TranscribeServiceConfig::platform): it stops and is joined
        // before AiController destroys it (review wave 4, P2-4).
        connect(m_ai, &AiController::shuttingDown, this, [this] {
            if (m_job.joinable()) {
                m_job.request_stop();
                m_job.join();
            }
        });
    }
    emit providersChanged();
}

void TranscriptBridge::addTranscript(MediaId media, int audioStream, std::shared_ptr<const ai::Transcript> transcript) {
    m_session->add(media, audioStream, std::move(transcript));
    rebuild();
}

void TranscriptBridge::setTranscriber(const QString& providerId, const QString& label, Transcriber transcriber) {
    m_fakeTranscribers[providerId] = {label, std::move(transcriber)};
    emit providersChanged();
}

command::CommandResult TranscriptBridge::run(const std::string& name, Json params, bool dryRun) {
    command::CommandResult result;
    if (!m_controller) {
        result.error = command::CommandError{};
        result.error->message = "no project";
        return result;
    }
    command::EnginePolicy policy;
    policy.services = m_services;
    command::CommandEngine engine(m_controller->session(), ai::edit::editingCommands(), policy);
    command::CommandRequest request;
    request.command = name;
    params["sequence"] = m_controller->currentSequenceId().value;
    request.params = std::move(params);
    request.dryRun = dryRun;
    return engine.execute(request);
}

// ---- Mapping ----------------------------------------------------------------------------------------

QString TranscriptBridge::language() const {
    return m_transcript.empty() ? QString() : fromStd(m_transcript.language(0));
}

void TranscriptBridge::rebuild() {
    cancelReview();
    if (!m_controller)
        return;
    m_mapped = m_controller->snapshot();
    m_transcript = ai::edit::SequenceTranscript::build(*m_mapped, m_controller->currentSequenceId(), *m_chain);
    m_selection.clear();
    m_anchor.reset();
    m_pending.reset();
    m_review.clear();
    m_reviewRemove.clear();
    rebuildParagraphs();
    search();
    updateHighlights();
    updateCurrentWord();
    bumpMarks();
    rebuildAudioTracks();
    emit transcriptChanged();
    emit reviewChanged();
}

void TranscriptBridge::onModelChanged() {
    cancelReview();
    if (!m_controller)
        return;
    const ProjectSnapshot next = m_controller->snapshot();
    if (!m_mapped || m_transcript.sequence() != m_controller->currentSequenceId()) {
        rebuild();
        return;
    }
    if (next.get() == m_mapped.get())
        return;
    // P.1: the controller's ChangeSet when it starts from the state the transcript describes; diffing the whole
    // project again only when it does not (a change this bridge did not see).
    const TimelineChange& last = m_controller->lastChange();
    std::optional<ChangeSet> computed;
    const ChangeSet* changes = last.changes.get();
    if (!changes || last.before.get() != m_mapped.get()) {
        computed = diff(*m_mapped, *next);
        changes = &*computed;
    }
    m_mapped = next;
    const SequenceChange* sequenceChange = changes->sequence(m_transcript.sequence());
    if (sequenceChange == nullptr && changes->addedMedia.empty() && changes->removedMedia.empty())
        return;
    // 0.4: only In/Out, markers or captions changed (Mark In and Out, Add Marker from the words, their undo): the words
    // are the same, so the selection stays.
    if (sequenceChange && changes->sequences.size() == 1 && sequenceChange->tracks.empty()
        && sequenceChange->addedClips.empty() && sequenceChange->removedClips.empty()
        && sequenceChange->modifiedClips.empty() && !sequenceChange->trackListChanged && !sequenceChange->settingsChanged
        && changes->addedMedia.empty() && changes->removedMedia.empty() && changes->modifiedMedia.empty()
        && !changes->redactionsChanged && !changes->speakersChanged && !changes->transcriptEditsChanged)
        return;
    rebuildAudioTracks(); // 0.4: a track renamed, muted, added or removed
    // Incremental: clips that only moved shift their words; changed clips are mapped again.
    m_transcript.update(*next, *changes, *m_chain);
    m_selection.clear();
    m_anchor.reset();
    m_pending.reset();
    m_review.clear();
    m_reviewRemove.clear();
    rebuildParagraphs();
    search();
    updateHighlights();
    updateCurrentWord();
    bumpMarks();
    emit transcriptChanged();
    emit reviewChanged();
}

void TranscriptBridge::rebuildParagraphs() {
    std::vector<TranscriptParagraphModel::Paragraph> rows;
    const std::size_t n = m_transcript.size();
    for (std::size_t i = 0; i < n; ++i) {
        const ai::edit::TimelineWord& w = m_transcript[i];
        const bool start = rows.empty() || w.speaker != m_transcript[static_cast<std::size_t>(rows.back().last)].speaker
                        || w.timeline.start - m_transcript[static_cast<std::size_t>(rows.back().last)].timeline.end() >= kParagraphPause
                        || static_cast<std::size_t>(rows.back().last - rows.back().first + 1) >= kParagraphWords;
        if (start) {
            TranscriptParagraphModel::Paragraph p;
            p.first = static_cast<int>(i);
            p.speakerIndex = w.speaker.value_or(-1);
            p.speaker = w.speaker ? tr("Speaker %1").arg(*w.speaker + 1) : QString();
            p.timeText = m_controller ? m_controller->formatTimecode(w.timeline.start) : QString();
            rows.push_back(std::move(p));
        }
        TranscriptParagraphModel::Paragraph& p = rows.back();
        p.last = static_cast<int>(i);
        // A space after the word when its language writes one before the next word (never in Thai).
        std::string text(m_transcript.text(i));
        if (i + 1 < n) {
            const std::string pair = m_transcript.textOf(i, i + 1);
            const std::string next(m_transcript.text(i + 1));
            if (pair.size() > text.size() + next.size())
                text += ' ';
        }
        p.words.append(QVariantMap{{QStringLiteral("i"), static_cast<int>(i)}, {QStringLiteral("text"), fromStd(text)}});
    }
    m_paragraphs->reset(std::move(rows));
}

void TranscriptBridge::updateCurrentWord() {
    int current = -1;
    if (m_controller && !m_transcript.empty()) {
        const Ticks t = m_controller->playhead();
        if (const auto i = m_transcript.wordAt(t); i && m_transcript[*i].timeline.contains(t))
            current = static_cast<int>(*i);
    }
    if (current != m_currentWord) {
        m_currentWord = current;
        emit currentWordChanged();
    }
}

// ---- Selection and marks ----------------------------------------------------------------------------

void TranscriptBridge::bumpMarks() {
    ++m_markRevision;
    emit marksChanged();
}

void TranscriptBridge::setMessage(const QString& message) {
    if (message == m_message)
        return;
    m_message = message;
    emit messageChanged();
}

bool TranscriptBridge::isSelected(int word) const {
    return word >= 0 && m_selection.contains(static_cast<std::size_t>(word));
}

bool TranscriptBridge::isRemoving(int word) const {
    return word >= 0 && m_pending && m_pending->words.contains(static_cast<std::size_t>(word));
}

bool TranscriptBridge::isMatch(int word) const {
    return word >= 0 && m_matchWords.contains(static_cast<std::size_t>(word));
}

std::vector<ai::edit::WordRange> TranscriptBridge::selectionRanges() const {
    std::vector<ai::edit::WordRange> out;
    for (std::size_t i : m_selection) {
        if (!out.empty() && out.back().last + 1 == i)
            out.back().last = i;
        else
            out.push_back({i, i});
    }
    return out;
}

QString TranscriptBridge::selectionInfo() const {
    if (m_selection.empty() || !m_controller)
        return {};
    Ticks length;
    for (const ai::edit::WordRange& r : selectionRanges())
        length += m_transcript[r.last].timeline.end() - m_transcript[r.first].timeline.start;
    return tr("%n word(s)", nullptr, static_cast<int>(m_selection.size())) + QStringLiteral(" · ")
         + m_controller->formatTimecode(length);
}

void TranscriptBridge::updateHighlights() {
    if (!m_controller)
        return;
    std::vector<TimelineHighlight> highlights;
    const SequencePtr seq = m_controller->currentSequence();
    if (seq) {
        for (const ai::edit::WordRange& r : selectionRanges()) {
            TimelineHighlight h;
            h.kind = TimelineHighlight::Kind::Selection;
            h.range = TimeRange{m_transcript[r.first].timeline.start,
                                m_transcript[r.last].timeline.end() - m_transcript[r.first].timeline.start};
            for (ClipId clip : m_transcript.clipsFor(r.first, r.last, *seq)) {
                if (const auto loc = seq->findClip(clip); loc && std::find(h.tracks.begin(), h.tracks.end(), loc->track->id) == h.tracks.end())
                    h.tracks.push_back(loc->track->id);
            }
            highlights.push_back(std::move(h));
        }
    }
    if (m_pending) {
        // The ghost of the dry run (the command plans the same cuts when it runs).
        for (const TimelineHighlight& cut : m_pendingCuts)
            highlights.push_back(cut);
    }
    m_controller->setTimelineHighlights(std::move(highlights));
}

void TranscriptBridge::seekToWord(std::size_t word) {
    if (!m_controller || word >= m_transcript.size())
        return;
    const TimeRange range = m_transcript[word].timeline;
    const SequencePtr seq = m_controller->currentSequence();
    m_controller->setPlayhead(seq ? seekPointIn(range, seq->frameRate) : range.start);
}

void TranscriptBridge::clickWord(int word, int modifiers) {
    if (word < 0 || static_cast<std::size_t>(word) >= m_transcript.size())
        return;
    const auto w = static_cast<std::size_t>(word);
    const auto mods = Qt::KeyboardModifiers(modifiers);
    cancelPreview();
    if (mods.testFlag(Qt::ShiftModifier) && m_anchor) {
        m_selection.clear();
        for (std::size_t i = std::min(*m_anchor, w); i <= std::max(*m_anchor, w); ++i)
            m_selection.insert(i);
    } else if (mods.testAnyFlags(Qt::ControlModifier | Qt::MetaModifier)) {
        if (!m_selection.erase(w))
            m_selection.insert(w);
        m_anchor = w;
    } else {
        m_selection = {w};
        m_anchor = w;
    }
    seekToWord(w);
    updateHighlights();
    bumpMarks();
}

void TranscriptBridge::selectWords(int first, int last) {
    const int n = static_cast<int>(m_transcript.size());
    if (n == 0)
        return;
    const int a = std::clamp(std::min(first, last), 0, n - 1);
    const int b = std::clamp(std::max(first, last), 0, n - 1);
    cancelPreview();
    m_selection.clear();
    for (int i = a; i <= b; ++i)
        m_selection.insert(static_cast<std::size_t>(i));
    m_anchor = static_cast<std::size_t>(first);
    updateHighlights();
    bumpMarks();
}

void TranscriptBridge::clearSelection() {
    if (m_selection.empty() && !m_pending)
        return;
    m_selection.clear();
    m_anchor.reset();
    m_pending.reset();
    m_pendingCuts.clear();
    updateHighlights();
    bumpMarks();
}

// ---- Text edits -------------------------------------------------------------------------------------

ai::edit::CutOptions TranscriptBridge::cutOptions() const {
    ai::edit::CutOptions options;
    options.padBefore = options.padAfter = Ticks{kTicksPerSecond * m_paddingMs / 1000};
    return options;
}

void TranscriptBridge::setPending(std::optional<Pending> pending, const ai::edit::EditPlan* plan) {
    m_pending = std::move(pending);
    m_pendingCuts.clear();
    if (m_pending && plan) {
        for (const ai::edit::PlannedCut& cut : plan->cuts) {
            TimelineHighlight h;
            h.kind = TimelineHighlight::Kind::Removal;
            h.range = cut.range;
            h.tracks = cut.tracks;
            m_pendingCuts.push_back(std::move(h));
        }
        m_pending->info = tr("Removes %1 in %n cut(s)", nullptr, static_cast<int>(plan->cuts.size()))
                              .arg(m_controller->formatTimecode(plan->removed()));
    }
    updateHighlights();
    bumpMarks();
}

QString TranscriptBridge::selectedText() const {
    const auto ranges = selectionRanges();
    if (ranges.size() != 1)
        return {};
    QStringList words;
    for (std::size_t i = ranges.front().first; i <= ranges.front().last; ++i)
        words << fromStd(std::string(m_transcript.text(i)));
    return words.join(QLatin1Char(' '));
}

QString TranscriptBridge::selectionSearchText() const {
    // As shown (and as search() matches): spaces only where the language writes them. Several runs: the first one.
    const auto ranges = selectionRanges();
    if (ranges.empty())
        return {};
    return fromStd(m_transcript.textOf(ranges.front().first, ranges.front().last));
}

QString TranscriptBridge::correctSelection(const QString& text) {
    const auto ranges = selectionRanges();
    if (ranges.size() != 1) {
        setMessage(ranges.empty() ? tr("Select the words to fix first") : tr("Fix one run of words at a time"));
        return m_message;
    }
    const ai::edit::WordRange r = ranges.front();
    const std::string fixed = text.simplified().toStdString();
    const command::CommandResult result =
        r.first == r.last ? run("transcript.correctWord", Json{{"word", r.first}, {"text", fixed}})
                          : run("transcript.correctRange", Json{{"words", Json::array({r.first, r.last})}, {"text", fixed}});
    if (!result.ok()) {
        setMessage(fromStd(result.error->message));
        return m_message;
    }
    setMessage(tr("Fixed. Captions made earlier update from the transcript in the Captions tab."));
    emit corrected();
    return {};
}

QString TranscriptBridge::previewDelete() {
    if (m_selection.empty()) {
        setMessage(tr("Select words first"));
        return m_message;
    }
    ai::edit::DeleteRequest request;
    request.words = selectionRanges();
    request.options = cutOptions();
    const auto plan = ai::edit::planDelete(m_transcript, *m_mapped, request);
    if (!plan) {
        setMessage(fromStd(plan.error().message));
        return m_message;
    }
    Pending p;
    p.command = "transcript.deleteRanges";
    p.params = Json{{"words", rangesJson(request.words)}, {"padding", m_paddingMs / 1000.0}};
    p.words = m_selection;
    setMessage({});
    setPending(std::move(p), &*plan);
    return {};
}

QString TranscriptBridge::previewKeep() {
    if (m_selection.empty()) {
        setMessage(tr("Select words first"));
        return m_message;
    }
    const std::vector<ai::edit::WordRange> keep = selectionRanges();
    const auto plan = ai::edit::planKeep(m_transcript, *m_mapped, keep, cutOptions());
    if (!plan) {
        setMessage(fromStd(plan.error().message));
        return m_message;
    }
    Pending p;
    p.command = "transcript.keepRanges";
    p.params = Json{{"words", rangesJson(keep)}, {"padding", m_paddingMs / 1000.0}};
    for (std::size_t i = 0; i < m_transcript.size(); ++i) {
        if (!m_selection.contains(i))
            p.words.insert(i);
    }
    setMessage({});
    setPending(std::move(p), &*plan);
    return {};
}

QString TranscriptBridge::previewReview() {
    std::vector<ai::edit::ReviewItem> items;
    for (std::size_t i = 0; i < m_review.size(); ++i) {
        if (m_reviewRemove[i])
            items.push_back(m_review[i]);
    }
    if (items.empty()) {
        setMessage(tr("Nothing is marked for removal"));
        return m_message;
    }
    const ai::edit::DeleteRequest request = ai::edit::deleteRequestFor(items, cutOptions());
    const auto plan = ai::edit::planDelete(m_transcript, *m_mapped, request);
    if (!plan) {
        setMessage(fromStd(plan.error().message));
        return m_message;
    }
    Pending p;
    p.command = "transcript.deleteRanges";
    p.params = Json{{"padding", m_paddingMs / 1000.0}};
    if (!request.words.empty())
        p.params["words"] = rangesJson(request.words);
    if (!request.gapsAfter.empty())
        p.params["gaps"] = request.gapsAfter;
    for (const ai::edit::WordRange& r : request.words) {
        for (std::size_t i = r.first; i <= r.last; ++i)
            p.words.insert(i);
    }
    setMessage({});
    setPending(std::move(p), &*plan);
    return {};
}

QString TranscriptBridge::applyPreview() {
    if (!m_pending)
        return {};
    const Pending pending = *m_pending;
    m_pending.reset();
    m_pendingCuts.clear();
    const command::CommandResult result = run(pending.command, pending.params);
    if (!result.ok()) {
        updateHighlights();
        bumpMarks();
        setMessage(fromStd(result.error->message));
        return m_message;
    }
    setMessage({});
    // The edit's change rebuilds the words (onModelChanged), which clears the marks.
    updateHighlights();
    bumpMarks();
    return {};
}

void TranscriptBridge::cancelPreview() {
    if (!m_pending)
        return;
    setPending(std::nullopt, nullptr);
}

QString TranscriptBridge::moveParagraph(int paragraph, int before) {
    const auto& rows = m_paragraphs->paragraphs();
    if (paragraph < 0 || paragraph >= static_cast<int>(rows.size()) || before < 0 || before > static_cast<int>(rows.size())
        || before == paragraph || before == paragraph + 1)
        return {};
    const auto& p = rows[static_cast<std::size_t>(paragraph)];
    const int beforeWord = before == static_cast<int>(rows.size()) ? static_cast<int>(m_transcript.size())
                                                                   : rows[static_cast<std::size_t>(before)].first;
    const command::CommandResult result =
        run("transcript.moveRange", Json{{"words", Json::array({p.first, p.last})}, {"before", beforeWord},
                                         {"padding", m_paddingMs / 1000.0}});
    if (!result.ok()) {
        setMessage(fromStd(result.error->message));
        return m_message;
    }
    setMessage({});
    return {};
}

// ---- Search -----------------------------------------------------------------------------------------

void TranscriptBridge::setSearchQuery(const QString& query) {
    if (query == m_query)
        return;
    m_query = query;
    search();
    bumpMarks();
}

void TranscriptBridge::search() {
    m_matches.clear();
    m_matchWords.clear();
    m_matchIndex = -1;
    const QString needle = m_query.trimmed().toCaseFolded();
    if (!needle.isEmpty() && !m_transcript.empty()) {
        // The text as shown (spaces only where the language writes them), with each word's character offset, so
        // a query can span words and Thai (no spaces) matches inside runs of word units.
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

void TranscriptBridge::nextMatch(int direction) {
    if (m_matches.empty())
        return;
    const int n = static_cast<int>(m_matches.size());
    m_matchIndex = m_matchIndex < 0 ? (direction < 0 ? n - 1 : 0) : ((m_matchIndex + (direction < 0 ? -1 : 1)) % n + n) % n;
    const ai::edit::WordRange r = m_matches[static_cast<std::size_t>(m_matchIndex)];
    selectWords(static_cast<int>(r.first), static_cast<int>(r.last));
    seekToWord(r.first);
    emit searchChanged();
}

void TranscriptBridge::selectAllMatches() {
    if (m_matchWords.empty())
        return;
    cancelPreview();
    m_selection = m_matchWords;
    m_anchor = *m_selection.begin();
    updateHighlights();
    bumpMarks();
}

// ---- Review -----------------------------------------------------------------------------------------

void TranscriptBridge::setReviewFillers(bool on) {
    if (on == m_reviewFillers)
        return;
    m_reviewFillers = on;
    cancelReview();
    emit reviewOptionsChanged();
}

void TranscriptBridge::setSilenceSeconds(double seconds) {
    seconds = std::clamp(seconds, 0.0, 30.0);
    if (seconds == m_silenceSeconds)
        return;
    m_silenceSeconds = seconds;
    cancelReview();
    emit reviewOptionsChanged();
}

void TranscriptBridge::setPaddingMs(int ms) {
    ms = std::clamp(ms, 0, 500);
    if (ms == m_paddingMs)
        return;
    m_paddingMs = ms;
    emit reviewOptionsChanged();
}

QVariantList TranscriptBridge::review() const {
    QVariantList out;
    for (std::size_t i = 0; i < m_review.size(); ++i) {
        const ai::edit::ReviewItem& item = m_review[i];
        out.append(QVariantMap{
            {QStringLiteral("index"), static_cast<int>(i)},
            {QStringLiteral("kind"), fromStd(ai::edit::toString(item.kind))},
            {QStringLiteral("label"), fromStd(item.label)},
            {QStringLiteral("context"), fromStd(item.context)},
            {QStringLiteral("durationText"), tr("%1 s").arg(item.range.duration.toSeconds(), 0, 'f', 1)},
            {QStringLiteral("confidence"), item.confidence},
            {QStringLiteral("remove"), static_cast<bool>(m_reviewRemove[i])},
        });
    }
    return out;
}

int TranscriptBridge::reviewRemoveCount() const {
    return static_cast<int>(std::count(m_reviewRemove.begin(), m_reviewRemove.end(), true));
}

void TranscriptBridge::setReviewRemove(int item, bool remove) {
    if (item < 0 || item >= static_cast<int>(m_reviewRemove.size()) || m_reviewRemove[static_cast<std::size_t>(item)] == remove)
        return;
    m_reviewRemove[static_cast<std::size_t>(item)] = remove;
    cancelPreview();
    emit reviewChanged();
}

void TranscriptBridge::setAllReviewRemove(bool remove) {
    std::fill(m_reviewRemove.begin(), m_reviewRemove.end(), remove);
    cancelPreview();
    emit reviewChanged();
}

// ---- Transcription ----------------------------------------------------------------------------------

QVariantList TranscriptBridge::providers() const {
    QVariantList out;
    const bool local = std::filesystem::exists(ai::defaultInferExecutable());
    out.append(QVariantMap{
        {QStringLiteral("id"), QStringLiteral("local")},
        {QStringLiteral("label"), tr("On this computer")},
        {QStringLiteral("detail"), tr("whisper.cpp, private, free")},
        {QStringLiteral("available"), local},
        {QStringLiteral("reason"), local ? QString() : tr("Local transcription is not part of this build")},
    });
    if (m_ai) {
        for (const QVariant& v : m_ai->transcriptionModels()) {
            const QVariantMap m = v.toMap();
            out.append(QVariantMap{
                {QStringLiteral("id"), QStringLiteral("cloud:") + m.value(QStringLiteral("id")).toString()},
                {QStringLiteral("label"), m.value(QStringLiteral("name")).toString()},
                {QStringLiteral("detail"), m.value(QStringLiteral("providerName")).toString() + QStringLiteral(" · ")
                                               + m.value(QStringLiteral("price")).toString()},
                {QStringLiteral("available"), m.value(QStringLiteral("usable")).toBool()},
                {QStringLiteral("reason"), m.value(QStringLiteral("reason")).toString()},
            });
        }
    }
    for (const auto& [id, entry] : m_fakeTranscribers) {
        out.append(QVariantMap{{QStringLiteral("id"), id}, {QStringLiteral("label"), entry.first},
                               {QStringLiteral("detail"), QString()}, {QStringLiteral("available"), true},
                               {QStringLiteral("reason"), QString()}});
    }
    return out;
}

bool TranscriptBridge::setupNeeded() const {
    for (const QVariant& p : providers()) {
        if (p.toMap().value(QStringLiteral("available")).toBool())
            return false;
    }
    return true;
}

void TranscriptBridge::setProvider(const QString& id) {
    if (id == m_provider)
        return;
    m_provider = id;
    emit providersChanged();
}

std::vector<TranscriptBridge::JobItem> TranscriptBridge::jobItems(const QString& scope) const {
    std::vector<JobItem> items;
    if (!m_controller)
        return items;
    const SequencePtr seq = m_controller->currentSequence();
    const ProjectSnapshot snap = m_controller->snapshot();
    if (!seq)
        return items;
    std::set<std::uint64_t> wanted;
    if (scope == QLatin1String("selection")) {
        const std::vector<ClipId> clips = m_controller->selection();
        for (ClipId c : edit::withLinkedClips(*seq, clips))
            wanted.insert(c.value);
    }
    std::set<std::pair<std::uint64_t, int>> seen;
    for (std::size_t t = 0; t < seq->audioTracks.size(); ++t) {
        const TrackPtr& track = seq->audioTracks[t];
        // 0.4: only the chosen (voice) tracks, for both scopes: a selected clip's linked partners on other tracks
        // (game audio, voice chat) are left out unless their track is chosen.
        if (!trackChosen(*track, static_cast<int>(t)))
            continue;
        for (const Clip& clip : track->clips) {
            if (!wanted.empty() && !wanted.contains(clip.id.value))
                continue;
            if (!seen.insert({clip.media.value, clip.stream.index}).second)
                continue;
            const MediaItem* media = snap->findMedia(clip.media);
            if (!media || media->isGenerator() || clip.stream.index < 0
                || clip.stream.index >= static_cast<int>(media->info.audio.size()))
                continue;
            if (m_chain->transcript(*media, clip.stream.index))
                continue; // transcribed already
            const auto path = resolveMediaPath(media->locator, m_controller->projectFile());
            if (!path || !std::filesystem::is_regular_file(*path))
                continue;
            items.push_back({media->id, clip.stream.index, media->info.audio[static_cast<std::size_t>(clip.stream.index)].index,
                             pathToUtf8(*path)});
        }
    }
    return items;
}

QVariantMap TranscriptBridge::consentFor(const QString& scope) const {
    if (!m_ai || !m_provider.startsWith(QLatin1String("cloud:")))
        return {};
    const std::vector<JobItem> items = jobItems(scope);
    return m_ai->consentPrompt(m_provider.mid(6), QStringLiteral("transcribe"),
                               tr("the audio of %n media file(s)", nullptr, static_cast<int>(items.size())));
}

QString TranscriptBridge::transcribe(const QString& scope, bool allowOnce) {
    if (m_jobStatus == QLatin1String("running"))
        return tr("A transcription is running");
    // Consent is enforced here and by the guarded platform call, not only by the panel that asks for it (AI-6).
    if (m_provider.startsWith(QLatin1String("cloud:")) && m_ai && !allowOnce &&
        m_ai->consentPrompt(m_provider.mid(6), QStringLiteral("transcribe"), QString()).value(QStringLiteral("needed")).toBool())
        return tr("Cloud transcription needs your consent first: nothing was uploaded");
    // 0.4: only chosen tracks are transcribed; with none chosen there is nothing to send.
    if (const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr; seq && !seq->audioTracks.empty()) {
        bool any = false;
        for (std::size_t t = 0; t < seq->audioTracks.size() && !any; ++t)
            any = trackChosen(*seq->audioTracks[t], static_cast<int>(t));
        if (!any) {
            setMessage(tr("Choose at least one audio track"));
            return m_message;
        }
    }
    std::vector<JobItem> items = jobItems(scope);
    if (items.empty()) {
        // Selected clips heard only on tracks switched off under Tracks: say so, and which tracks (fix round 1: the
        // message below blamed transcripts or missing files).
        const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
        if (seq && scope == QLatin1String("selection")) {
            std::set<std::uint64_t> wanted;
            const std::vector<ClipId> clips = m_controller->selection();
            for (ClipId c : edit::withLinkedClips(*seq, clips))
                wanted.insert(c.value);
            QStringList off;
            bool onChosen = false;
            for (std::size_t t = 0; t < seq->audioTracks.size() && !wanted.empty(); ++t) {
                const Track& track = *seq->audioTracks[t];
                if (std::none_of(track.clips.begin(), track.clips.end(), [&](const Clip& c) { return wanted.contains(c.id.value); }))
                    continue;
                if (trackChosen(track, static_cast<int>(t)))
                    onChosen = true;
                else
                    off << (track.name.empty() ? QStringLiteral("A%1").arg(static_cast<int>(t) + 1)
                                               : QStringLiteral("A%1 · %2").arg(static_cast<int>(t) + 1).arg(QString::fromStdString(track.name)));
            }
            if (!onChosen && !off.isEmpty()) {
                setMessage(tr("Nothing to transcribe: the selected clips are on tracks switched off under Tracks (%1)")
                               .arg(off.join(QStringLiteral(", "))));
                return m_message;
            }
        }
        setMessage(tr("Nothing to transcribe: every clip here has a transcript, or its media file is missing"));
        return m_message;
    }
    return startJob(std::move(items), allowOnce);
}

QVariantMap TranscriptBridge::consentForMedia() const {
    if (!m_ai || !m_provider.startsWith(QLatin1String("cloud:")))
        return {};
    return m_ai->consentPrompt(m_provider.mid(6), QStringLiteral("transcribe"), tr("the audio of %n media file(s)", nullptr, 1));
}

QString TranscriptBridge::transcribeMedia(MediaId media, int audioStream, bool allowOnce) {
    if (m_jobStatus == QLatin1String("running"))
        return tr("A transcription is running");
    if (m_provider.startsWith(QLatin1String("cloud:")) && m_ai && !allowOnce &&
        m_ai->consentPrompt(m_provider.mid(6), QStringLiteral("transcribe"), QString()).value(QStringLiteral("needed")).toBool())
        return tr("Cloud transcription needs your consent first: nothing was uploaded");
    const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : nullptr;
    const MediaItem* item = snap ? snap->findMedia(media) : nullptr;
    if (!item || item->isGenerator() || audioStream < 0 || audioStream >= static_cast<int>(item->info.audio.size())) {
        setMessage(tr("This clip has no sound to transcribe"));
        return m_message;
    }
    if (m_chain->transcript(*item, audioStream)) {
        setMessage(tr("This clip has a transcript already"));
        return m_message;
    }
    const auto path = resolveMediaPath(item->locator, m_controller->projectFile());
    if (!path || !std::filesystem::is_regular_file(*path)) {
        setMessage(tr("The clip's media file is missing"));
        return m_message;
    }
    std::vector<JobItem> items{{item->id, audioStream, item->info.audio[static_cast<std::size_t>(audioStream)].index,
                                pathToUtf8(*path)}};
    return startJob(std::move(items), allowOnce);
}

QString TranscriptBridge::startJob(std::vector<JobItem> items, bool allowOnce) {
    // Review P1-3: what the project bleeps or mutes is silenced before any transcriber hears it (TranscribeSource::
    // redacted) and never comes back as words (every result is redacted below, whatever made it). Per file: every media
    // item of the project that plays it adds its redactions - two items of one file share its transcript (review
    // wave 5, A5-P2-28: keyed by the items transcribed, the other item's redaction was uploaded).
    std::map<std::string, std::vector<TimeRange>> redacted;
    if (m_controller) {
        const ProjectSnapshot snap = m_controller->snapshot();
        std::set<std::string> files;
        for (const JobItem& item : items) {
            files.insert(item.path);
            redacted[item.path];
        }
        for (const MediaPtr& media : snap->media) {
            if (media->isGenerator()) {
                continue;
            }
            const auto path = resolveMediaPath(media->locator, m_controller->projectFile());
            if (!path || !files.contains(pathToUtf8(*path))) {
                continue;
            }
            const std::vector<TimeRange> more = ai::audioRedactionRanges(*snap, media->id);
            auto& ranges = redacted[pathToUtf8(*path)];
            ranges.insert(ranges.end(), more.begin(), more.end());
        }
        for (auto& entry : redacted) {
            std::vector<TimeRange>& ranges = entry.second;
            std::sort(ranges.begin(), ranges.end(), [](const TimeRange& a, const TimeRange& b) { return a.start < b.start; });
            std::vector<TimeRange> merged;
            for (const TimeRange& r : ranges) {
                if (!merged.empty() && r.start <= merged.back().end()) {
                    merged.back() = TimeRange::fromStartEnd(merged.back().start, std::max(merged.back().end(), r.end()));
                } else {
                    merged.push_back(r);
                }
            }
            ranges = std::move(merged);
        }
    }
    Transcriber transcriber;
    if (const auto fake = m_fakeTranscribers.find(m_provider); fake != m_fakeTranscribers.end()) {
        transcriber = fake->second.second;
    } else if (m_provider == QLatin1String("local") || (m_provider.startsWith(QLatin1String("cloud:")) && m_ai)) {
        // ai::TranscribeService (shared with transcript.transcribe and wabcut-cli): 16 kHz extraction, local whisper
        // or the cloud model through the platform's guards (chunked at silences), the analysis cache.
        const std::string provider = m_provider.toStdString();
        if (m_provider == QLatin1String("local") && !std::filesystem::exists(ai::defaultInferExecutable()))
            return tr("Local transcription is not part of this build");
        if (m_provider.startsWith(QLatin1String("cloud:"))) {
            ai::CallOptions options;
            options.allowOnce = allowOnce; // only the user's answer; otherwise the platform requires recorded consent
            if (auto created = m_ai->platform().transcriber(provider.substr(6), options); !created)
                return fromStd(created.error().message); // no key, unknown model, ...: say so now
        }
        ai::TranscribeServiceConfig config;
        config.platform = m_ai ? &m_ai->platform() : nullptr;
        auto service = std::make_shared<ai::TranscribeService>(std::move(config));
        transcriber = [service, provider, allowOnce, redacted](const std::string& path, int stream,
                                                               const std::function<void(float)>& progress,
                                                               std::stop_token stop) -> std::expected<ai::Transcript, Error> {
            ai::TranscribeJobOptions options;
            options.provider = provider;
            options.allowOnce = allowOnce; // only the user's answer; otherwise the platform requires recorded consent
            const auto ranges = redacted.find(path);
            auto outcome = service->run(
                ai::TranscribeSource{path, stream, ranges != redacted.end() ? ranges->second : std::vector<TimeRange>{}},
                options, stop, progress);
            if (!outcome)
                return std::unexpected(outcome.error());
            return std::move(outcome->transcript);
        };
    } else {
        return tr("Choose how to transcribe first");
    }

    transcriber = [inner = std::move(transcriber), redacted](const std::string& path, int stream,
                                                            const std::function<void(float)>& progress,
                                                            std::stop_token stop) -> std::expected<ai::Transcript, Error> {
        auto t = inner(path, stream, progress, stop);
        if (const auto ranges = redacted.find(path); t && ranges != redacted.end()) {
            ai::redactTranscript(*t, ranges->second);
        }
        return t;
    };
    m_jobStatus = QStringLiteral("running");
    m_jobProgress = 0.0;
    m_jobText = tr("Transcribing %n file(s)…", nullptr, static_cast<int>(items.size()));
    emit jobChanged();
    const int generation = ++m_jobGeneration;
    QPointer<TranscriptBridge> self(this);
    m_job = std::jthread([self, items, transcriber, generation, this](std::stop_token stop) {
        std::vector<std::pair<JobItem, std::expected<ai::Transcript, Error>>> results;
        for (std::size_t k = 0; k < items.size() && !stop.stop_requested(); ++k) {
            const auto progress = [&, k](float p) {
                const double overall = (static_cast<double>(k) + std::clamp(static_cast<double>(p), 0.0, 1.0))
                                     / static_cast<double>(items.size());
                QMetaObject::invokeMethod(this, [self, overall, generation] {
                    if (self && self->m_jobGeneration == generation && self->m_jobStatus == QLatin1String("running")) {
                        self->m_jobProgress = overall;
                        emit self->jobChanged();
                    }
                }, Qt::QueuedConnection);
            };
            // Review P2-13: an exception in a transcriber is a failed item, not std::terminate.
            std::expected<ai::Transcript, Error> result = std::unexpected(Error{ErrorCode::Unknown, "transcription failed"});
            try {
                result = transcriber(items[k].path, items[k].containerStream, progress, stop);
            } catch (const std::exception& e) {
                result = std::unexpected(Error{ErrorCode::Unknown, std::string("transcription failed: ") + e.what()});
            } catch (...) {
            }
            results.emplace_back(items[k], std::move(result));
        }
        const bool cancelled = stop.stop_requested();
        QMetaObject::invokeMethod(this, [self, results = std::move(results), cancelled, generation]() mutable {
            if (self && self->m_jobGeneration == generation)
                self->finishJob(std::move(results), cancelled);
        }, Qt::QueuedConnection);
    });
    return {};
}

void TranscriptBridge::finishJob(std::vector<std::pair<JobItem, std::expected<ai::Transcript, Error>>> results,
                                 bool cancelled) {
    if (m_job.joinable())
        m_job.join();
    int done = 0;
    QString error;
    for (auto& [item, result] : results) {
        if (result) {
            m_session->add(item.media, item.audioStream, std::make_shared<const ai::Transcript>(std::move(*result)));
            ++done;
        } else if (result.error().code != ErrorCode::Cancelled) {
            error = fromStd(result.error().message);
        }
    }
    if (done > 0)
        rebuild();
    m_jobStatus = cancelled ? QStringLiteral("idle") : !error.isEmpty() && done == 0 ? QStringLiteral("error") : QStringLiteral("done");
    m_jobProgress = cancelled ? 0.0 : 1.0;
    m_jobText = cancelled ? tr("Transcription cancelled")
              : !error.isEmpty() ? tr("Transcription failed: %1").arg(error)
                                 : tr("Transcribed %n file(s)", nullptr, done);
    emit jobChanged();
}

void TranscriptBridge::cancelTranscription() {
    if (m_jobStatus != QLatin1String("running"))
        return;
    m_job.request_stop();
    m_jobText = tr("Cancelling…");
    emit jobChanged();
}

// ---- 0.4: follow the playhead and the word menu ---------------------------------------------------------

int TranscriptBridge::paragraphOfWord(int word) const {
    const auto& rows = m_paragraphs->paragraphs();
    if (word < 0 || rows.empty())
        return -1;
    // The first paragraph whose last word is at or after `word` (paragraphs are in word order, without gaps).
    const auto it = std::lower_bound(rows.begin(), rows.end(), word,
                                     [](const TranscriptParagraphModel::Paragraph& p, int w) { return p.last < w; });
    if (it == rows.end() || it->first > word)
        return -1;
    return static_cast<int>(it - rows.begin());
}

int TranscriptBridge::selectionFirst() const {
    return m_selection.empty() ? -1 : static_cast<int>(*m_selection.begin());
}

std::optional<TimeRange> TranscriptBridge::selectionSpan() const {
    if (m_selection.empty() || !m_controller)
        return std::nullopt;
    const SequencePtr seq = m_controller->currentSequence();
    const std::size_t first = *m_selection.begin();
    const std::size_t last = *m_selection.rbegin();
    if (!seq || last >= m_transcript.size())
        return std::nullopt;
    const Rate rate = seq->frameRate;
    // From the start of the first word's frame to the end of the last word's last frame.
    const Ticks start = framesToTicks(ticksToFrames(m_transcript[first].timeline.start, rate), rate);
    const Ticks lastTick = std::max(m_transcript[last].timeline.end() - Ticks{1}, m_transcript[last].timeline.start);
    const Ticks end = framesToTicks(ticksToFrames(lastTick, rate) + 1, rate);
    if (end <= start)
        return std::nullopt;
    return TimeRange::fromStartEnd(start, end);
}

QString TranscriptBridge::copySelection(bool withTimecodes) {
    if (m_selection.empty()) {
        setMessage(tr("Select words first"));
        return m_message;
    }
    QStringList lines;
    const auto& paragraphs = m_paragraphs->paragraphs();
    for (const ai::edit::WordRange& r : selectionRanges()) {
        if (!withTimecodes) {
            lines << fromStd(m_transcript.textOf(r.first, r.last));
            continue;
        }
        // One line per run and paragraph: "[00:01:02:03] Speaker 1: text" (no speaker when it is not known).
        for (const TranscriptParagraphModel::Paragraph& p : paragraphs) {
            const std::size_t a = std::max(r.first, static_cast<std::size_t>(std::max(p.first, 0)));
            const std::size_t b = p.last < 0 ? 0 : std::min(r.last, static_cast<std::size_t>(p.last));
            if (p.last < 0 || a > b)
                continue;
            QString line = QLatin1Char('[') + (m_controller ? m_controller->formatTimecode(m_transcript[a].timeline.start) : QString())
                         + QStringLiteral("] ");
            if (!p.speaker.isEmpty())
                line += p.speaker + QStringLiteral(": ");
            lines << line + fromStd(m_transcript.textOf(a, b));
        }
    }
    const QString text = lines.join(QLatin1Char('\n'));
    QClipboard* clipboard = qobject_cast<QGuiApplication*>(QCoreApplication::instance()) ? QGuiApplication::clipboard() : nullptr;
    if (!clipboard) {
        setMessage(tr("The clipboard is not available"));
        return m_message;
    }
    clipboard->setText(text);
    setMessage({});
    return {};
}

QString TranscriptBridge::markSelection() {
    const auto span = selectionSpan();
    if (!span) {
        setMessage(tr("Select words first"));
        return m_message;
    }
    const SequenceId seq = m_controller->currentSequenceId();
    const Ticks in = span->start;
    const Ticks lastFrame = span->end() - m_controller->frameTicks();
    const bool ok = m_controller->applyEditFn(
        QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Mark In and Out"), [&](const ProjectState& state) -> edit::EditOutcome {
            auto marked = edit::markIn(state, seq, in);
            if (!marked)
                return marked;
            return edit::markOut(marked->state, seq, lastFrame);
        });
    if (!ok) {
        setMessage(m_controller->lastError());
        return m_message;
    }
    setMessage({});
    return {};
}

QString TranscriptBridge::addMarkerFromSelection(bool chapter) {
    const auto span = selectionSpan();
    if (!span) {
        setMessage(tr("Select words first"));
        return m_message;
    }
    const std::size_t first = *m_selection.begin();
    const std::size_t last = *m_selection.rbegin();
    const QString text = fromStd(m_transcript.textOf(first, last)).simplified();
    // The name: the first ~60 characters, cut at a word, with "…"; the comment: the whole text.
    QString name = text;
    constexpr qsizetype kNameLength = 60;
    if (name.size() > kNameLength) {
        qsizetype cut = name.lastIndexOf(QLatin1Char(' '), kNameLength);
        if (cut < kNameLength / 2) {
            // One long word, or Thai (written without spaces): cut between grapheme clusters, never inside one.
            QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, name);
            finder.setPosition(kNameLength);
            if (!finder.isAtBoundary())
                finder.toPreviousBoundary();
            cut = finder.position() > 0 ? finder.position() : kNameLength;
        }
        name = name.left(cut).trimmed() + QChar(0x2026);
    }
    Json params{{"time", span->start.value}, {"name", name.toStdString()}, {"comment", text.toStdString()}};
    if (chapter)
        params["type"] = "chapter";
    else
        params["duration"] = span->duration.value;
    const command::CommandResult result = run("marker.add", std::move(params));
    if (!result.ok()) {
        setMessage(fromStd(result.error->message));
        return m_message;
    }
    setMessage({});
    return {};
}

QString TranscriptBridge::liftSelection() {
    const auto span = selectionSpan();
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    if (!span || !seq) {
        setMessage(tr("Select words first"));
        return m_message;
    }
    const std::size_t first = *m_selection.begin();
    const std::size_t last = *m_selection.rbegin();
    // The tracks the words are heard on, plus their linked partners' tracks (the video too).
    std::vector<TrackId> tracks;
    const auto addTrack = [&](TrackId id) {
        if (id.isValid() && std::find(tracks.begin(), tracks.end(), id) == tracks.end())
            tracks.push_back(id);
    };
    for (std::size_t i = first; i <= last; ++i)
        addTrack(m_transcript[i].track);
    const std::vector<ClipId> clips = m_transcript.clipsFor(first, last, *seq);
    for (const ClipId clip : edit::withLinkedClips(*seq, clips)) {
        if (const auto loc = seq->findClip(clip))
            addTrack(loc->track->id);
    }
    // A locked track would keep its part of the words: refuse the whole lift (nothing changes).
    for (const TrackId id : tracks) {
        if (const Track* track = seq->findTrack(id); track && track->locked) {
            setMessage(tr("A locked track would change"));
            return m_message;
        }
    }
    Json ids = Json::array();
    for (const TrackId id : tracks)
        ids.push_back(id.value);
    cancelPreview();
    // sequence.lift: one undo step "Lift"; it clears In/Out (Premiere's Lift); the playhead parks at the gap's start.
    const command::CommandResult result =
        run("sequence.lift", Json{{"in", span->start.value}, {"out", span->end().value}, {"tracks", std::move(ids)}});
    if (!result.ok()) {
        setMessage(result.error->code == command::ErrorCode::LockedTrack ? tr("A locked track would change")
                                                                         : fromStd(result.error->message));
        return m_message;
    }
    m_controller->setPlayhead(span->start);
    setMessage({});
    return {};
}

// ---- 0.4: voice tracks --------------------------------------------------------------------------------

bool TranscriptBridge::trackChosen(const Track& track, int index) const {
    const QJsonObject stored = storedTrackChoices();
    const QString key = choiceKey(track, index);
    if (const auto it = stored.find(key); it != stored.end() && it->isBool())
        return it->toBool();
    if (defaultVoice(track))
        return true;
    // No voice track in the sequence: every unmuted track is transcribed (as before 0.4).
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    if (!seq)
        return !track.muted;
    const bool anyVoice = std::any_of(seq->audioTracks.begin(), seq->audioTracks.end(),
                                      [](const TrackPtr& t) { return defaultVoice(*t); });
    return !anyVoice && !track.muted;
}

void TranscriptBridge::rebuildAudioTracks() {
    QVariantList list;
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    if (seq) {
        for (std::size_t t = 0; t < seq->audioTracks.size(); ++t) {
            const Track& track = *seq->audioTracks[t];
            const int index = static_cast<int>(t);
            const QString name = QString::fromStdString(track.name);
            QString role;
            QString roleLabel;
            switch (track.role) {
            case TrackRole::Dialogue: role = QStringLiteral("dialogue"); roleLabel = tr("Dialogue"); break;
            case TrackRole::Music: role = QStringLiteral("music"); roleLabel = tr("Music"); break;
            case TrackRole::Effects: role = QStringLiteral("effects"); roleLabel = tr("Effects"); break;
            case TrackRole::None: break;
            }
            // "A2 · Mic", then the role and "(muted)" when set.
            QString label = QStringLiteral("A%1").arg(index + 1);
            if (!name.isEmpty())
                label += QStringLiteral(" · ") + name;
            if (!roleLabel.isEmpty())
                label += QStringLiteral(" · ") + roleLabel;
            if (track.muted)
                label += QLatin1Char(' ') + tr("(muted)");
            list.append(QVariantMap{{QStringLiteral("index"), index},
                                    {QStringLiteral("label"), label},
                                    {QStringLiteral("name"), name},
                                    {QStringLiteral("role"), role},
                                    {QStringLiteral("muted"), track.muted},
                                    {QStringLiteral("chosen"), trackChosen(track, index)}});
        }
    }
    if (list == m_audioTracks)
        return;
    m_audioTracks = std::move(list);
    emit audioTracksChanged();
}

void TranscriptBridge::setTrackChosen(int index, bool chosen) {
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    if (!seq || index < 0 || index >= static_cast<int>(seq->audioTracks.size()))
        return;
    const Track& track = *seq->audioTracks[static_cast<std::size_t>(index)];
    QJsonObject stored = storedTrackChoices();
    stored.insert(choiceKey(track, index), chosen);
    QSettings().setValue(QLatin1String(kTrackChoicesKey),
                         QString::fromUtf8(QJsonDocument(stored).toJson(QJsonDocument::Compact)));
    rebuildAudioTracks();
}

} // namespace wabcut::app
