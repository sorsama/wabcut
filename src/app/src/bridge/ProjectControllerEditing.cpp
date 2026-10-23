// ProjectController: the editing API (M5b). Every edit is an EditOps call applied through the EditSession; the clipboard
// (C.11) runs the model's clipboard.* commands.

#include "ProjectController.h"

#include <wabcut/command/Engine.h>
#include <wabcut/command/ExecutionContext.h>
#include <wabcut/model/ClipGroups.h>
#include <wabcut/model/Clipboard.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/ProjectOps.h>

#include <QClipboard>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QGuiApplication>
#include <QMimeData>

#include <algorithm>
#include <cstdlib>
#include <string>
#include <string_view>

namespace wabcut::app {

namespace {

std::vector<TrackId> tracksWithClipsAt(const Sequence& seq, std::span<const ClipId> clips, Ticks at) {
    std::vector<TrackId> tracks;
    for (ClipId id : clips) {
        const auto loc = seq.findClip(id);
        if (loc && loc->clip().timelineRange.contains(at)
            && std::find(tracks.begin(), tracks.end(), loc->track->id) == tracks.end())
            tracks.push_back(loc->track->id);
    }
    return tracks;
}

} // namespace

// ---- Gesture plumbing ---------------------------------------------------------------------------------------------

edit::EditOutcome ProjectController::previewEdit(const EditFn& fn) const {
    const ProjectSnapshot snap = m_session->snapshot();
    if (!snap || !fn)
        return std::unexpected(Error{ErrorCode::InvalidArgument, "nothing to edit"});
    return fn(*snap);
}

bool ProjectController::commitGesture(const char* label, const EditFn& fn) {
    m_session->beginGesture(label);
    const bool ok = applyResult(label, fn);
    m_session->endGesture();
    return ok;
}

void ProjectController::beginGesture(const QString& label) {
    m_session->beginGesture(label.toStdString());
}

void ProjectController::endGesture() {
    m_session->endGesture();
}

void ProjectController::cancelGesture() {
    m_session->cancelGesture(); // listeners resync the view through the ChangeSet
}

std::vector<ClipId> ProjectController::selectedClipIds() const {
    std::vector<ClipId> ids;
    const SequencePtr seq = currentSequence();
    if (!seq)
        return ids;
    for (ClipId id : m_selection) {
        if (seq->findClip(id))
            ids.push_back(id);
    }
    return ids;
}

// ---- Selection-based edits ----------------------------------------------------------------------------------------

void ProjectController::deleteSelection() {
    const std::vector<ClipId> clips = selectedClipIds();
    if (clips.empty() && m_gap) {
        rippleDeleteGap(); // 0.2 UX: Delete on a selected gap closes it (Premiere)
        return;
    }
    if (clips.empty())
        return;
    const SequenceId seq = m_sequence;
    applyResult(QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Delete"),
                [=](const ProjectState& s) { return edit::deleteClips(s, seq, clips); });
}

void ProjectController::rippleDeleteSelection() {
    const std::vector<ClipId> clips = selectedClipIds();
    if (clips.empty() && m_gap) {
        rippleDeleteGap(); // 0.2 UX: the selected gap
        return;
    }
    if (clips.empty())
        return;
    const SequenceId seq = m_sequence;
    applyResult(QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Ripple Delete"),
                [=](const ProjectState& s) { return edit::rippleDeleteClips(s, seq, clips); });
}

void ProjectController::razorAtPlayhead(bool allTracks) {
    const SequencePtr current = currentSequence();
    if (!current)
        return;
    const SequenceId seq = m_sequence;
    const Ticks at = m_playhead;
    if (allTracks) {
        applyResult(QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Add Edit to All Tracks"),
                    [=](const ProjectState& s) { return edit::addEditToAllTracks(s, seq, at); });
        return;
    }
    // Premiere: the selected clips under the playhead, else the targeted tracks.
    std::vector<TrackId> tracks = tracksWithClipsAt(*current, selectedClipIds(), at);
    if (tracks.empty())
        tracks = edit::targetedTracks(*current);
    if (tracks.empty())
        return;
    applyResult(QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Add Edit"),
                [=](const ProjectState& s) { return edit::addEdit(s, seq, at, tracks); });
}

void ProjectController::addEditAtPlayhead() {
    razorAtPlayhead(false);
}

bool ProjectController::razorAt(TrackId track, Ticks at, bool allTracks, bool ignoreLinks) {
    const SequenceId seq = m_sequence;
    if (allTracks) {
        return commitGesture(QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Razor"),
                             [=](const ProjectState& s) { return edit::addEditToAllTracks(s, seq, at); });
    }
    const std::vector<TrackId> tracks{track};
    const edit::LinkMode links = ignoreLinks ? edit::LinkMode::IgnoreLinks : edit::LinkMode::FollowLinks;
    return commitGesture(QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Razor"),
                         [=](const ProjectState& s) { return edit::addEdit(s, seq, at, tracks, links); });
}

void ProjectController::rippleTrimPreviousToPlayhead() {
    const SequenceId seq = m_sequence;
    const Ticks at = m_playhead;
    applyResult(QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Ripple Trim Previous Edit"),
                [=](const ProjectState& s) { return edit::rippleTrimPreviousEditToPlayhead(s, seq, at); }, true);
}

void ProjectController::rippleTrimNextToPlayhead() {
    const SequenceId seq = m_sequence;
    const Ticks at = m_playhead;
    applyResult(QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Ripple Trim Next Edit"),
                [=](const ProjectState& s) { return edit::rippleTrimNextEditToPlayhead(s, seq, at); }, true);
}

void ProjectController::extendEditToPlayhead() {
    const SequencePtr current = currentSequence();
    if (!current || !m_primary)
        return;
    const auto loc = current->findClip(m_primary);
    if (!loc)
        return;
    const Clip& clip = loc->clip();
    const Ticks at = m_playhead;
    // The edge nearest the playhead moves to it.
    const bool head = (at - clip.start()).value < 0
                   || (at < clip.end() && (at - clip.start()).value <= (clip.end() - at).value);
    const edit::Edge edge = head ? edit::Edge::Head : edit::Edge::Tail;
    const Ticks delta = at - (head ? clip.start() : clip.end());
    if (delta.value == 0)
        return;
    const SequenceId seq = m_sequence;
    const ClipId id = m_primary;
    applyResult(QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Extend Edit"),
                [=](const ProjectState& s) { return edit::trimEdge(s, seq, id, edge, delta); });
}

void ProjectController::selectClipAtPlayhead() {
    const SequencePtr current = currentSequence();
    if (!current)
        return;
    std::vector<TrackId> tracks = edit::targetedTracks(*current);
    if (tracks.empty())
        tracks = edit::allTracks(*current);
    std::vector<ClipId> clips;
    for (TrackId id : tracks) {
        const Track* track = current->findTrack(id);
        if (const auto index = track ? track->clipIndexAt(m_playhead) : std::nullopt)
            clips.push_back(track->clips[*index].id);
    }
    setSelection(edit::withLinkedClips(*current, clips));
}

bool ProjectController::selectionLinked() const {
    const SequencePtr seq = currentSequence();
    if (!seq)
        return false;
    return std::ranges::any_of(m_selection, [&](ClipId id) {
        const auto loc = seq->findClip(id);
        return loc && loc->clip().linkGroup.isValid();
    });
}

bool ProjectController::selectionEnabled() const {
    const SequencePtr seq = currentSequence();
    if (!seq)
        return true;
    return std::ranges::all_of(m_selection, [&](ClipId id) {
        const auto loc = seq->findClip(id);
        return !loc || loc->clip().enabled;
    });
}

void ProjectController::linkSelection() {
    const std::vector<ClipId> clips = selectedClipIds();
    if (clips.size() < 2)
        return;
    const SequenceId seq = m_sequence;
    applyResult(QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Link"),
                [=](const ProjectState& s) { return edit::linkClips(s, seq, clips); });
}

void ProjectController::unlinkSelection() {
    const std::vector<ClipId> clips = selectedClipIds();
    if (clips.empty())
        return;
    const SequenceId seq = m_sequence;
    applyResult(QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Unlink"),
                [=](const ProjectState& s) { return edit::unlinkClips(s, seq, clips); });
}

void ProjectController::toggleLinkSelection() {
    if (selectionLinked())
        unlinkSelection();
    else
        linkSelection();
}

void ProjectController::toggleEnabledSelection() {
    const std::vector<ClipId> clips = selectedClipIds();
    if (clips.empty())
        return;
    edit::ClipAttributeChange change;
    change.enabled = !selectionEnabled();
    const SequenceId seq = m_sequence;
    applyResult(*change.enabled ? QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Enable")
                                : QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Disable"),
                [=](const ProjectState& s) { return edit::setClipAttributes(s, seq, clips, change); });
}

void ProjectController::nudgeSelection(int frames) {
    const SequencePtr current = currentSequence();
    const std::vector<ClipId> clips = selectedClipIds();
    if (!current || clips.empty() || frames == 0)
        return;
    const SequenceId seq = m_sequence;
    const Ticks delta = framesToTicks(frames, current->frameRate);
    applyResult(QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Nudge"),
                [=](const ProjectState& s) { return edit::moveClips(s, seq, clips, delta); });
}

void ProjectController::slideSelection(int frames) {
    const SequencePtr current = currentSequence();
    if (!current || !m_primary || frames == 0)
        return;
    const SequenceId seq = m_sequence;
    const ClipId clip = m_primary;
    const Ticks delta = framesToTicks(frames, current->frameRate);
    applyResult(QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Slide"),
                [=](const ProjectState& s) { return edit::slide(s, seq, clip, delta); });
}

void ProjectController::slipSelection(int frames) {
    const SequencePtr current = currentSequence();
    const std::vector<ClipId> clips = selectedClipIds();
    if (!current || clips.empty() || frames == 0)
        return;
    const SequenceId seq = m_sequence;
    const Ticks delta = framesToTicks(frames, current->frameRate);
    applyResult(QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Slip"),
                [=](const ProjectState& s) { return edit::slip(s, seq, clips, delta); });
}

void ProjectController::renameClip(const QString& clipId, const QString& name) {
    const auto id = parseClipKey(clipId);
    const QString trimmed = name.trimmed();
    if (!id || trimmed.isEmpty())
        return;
    edit::ClipAttributeChange change;
    change.name = trimmed.toStdString();
    const SequenceId seq = m_sequence;
    const ClipId clip = *id;
    applyResult(QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Rename"), [=](const ProjectState& s) {
        const ClipId ids[] = {clip};
        return edit::setClipAttributes(s, seq, ids, change);
    });
}

void ProjectController::setSelectionColor(int colorIndex) {
    const std::vector<ClipId> clips = selectedClipIds();
    if (clips.empty() || colorIndex < 0)
        return;
    edit::ClipAttributeChange change;
    change.colorIndex = colorIndex;
    const SequenceId seq = m_sequence;
    applyResult(QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Label Color"),
                [=](const ProjectState& s) { return edit::setClipAttributes(s, seq, clips, change); });
}

// ---- Clipboard (C.11) -----------------------------------------------------------------------------------------------

namespace {

QClipboard* systemClipboard() {
    return qobject_cast<QGuiApplication*>(QCoreApplication::instance()) ? QGuiApplication::clipboard() : nullptr;
}

/// The clipboard document on the system clipboard, or empty. Windows can hand a clipboard block back rounded up
/// with zero bytes.
QByteArray systemDocument(const QClipboard* clipboard) {
    const QMimeData* mime = clipboard ? clipboard->mimeData() : nullptr;
    if (!mime || !mime->hasFormat(ProjectController::clipboardMimeType()))
        return {};
    QByteArray bytes = mime->data(ProjectController::clipboardMimeType());
    while (!bytes.isEmpty() && bytes.back() == '\0')
        bytes.chop(1);
    return bytes;
}

QByteArray digest(const QByteArray& bytes) {
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
}

/// The data of a batch's first step (or of a single command).
const command::Json& firstData(const command::CommandResult& result) {
    return result.steps.empty() ? result.data : result.steps.front().data;
}

QString resultError(const command::CommandResult& result) {
    return result.error ? QString::fromStdString(result.error->message) : QString();
}

command::CommandBatch singleCommand(const char* name, command::Json params, const char* label) {
    command::CommandRequest request;
    request.command = name;
    request.params = std::move(params);
    command::CommandBatch batch;
    batch.commands.push_back(std::move(request));
    batch.label = label;
    return batch;
}

} // namespace

QString ProjectController::clipboardMimeType() {
    return QStringLiteral("application/x-wabcut-clipboard");
}

bool ProjectController::canPaste() const {
    return m_systemForeign || m_clipboardServices->get().has_value();
}

void ProjectController::notifyClipboardChanged() {
    emit clipboardChanged();
}

command::CommandResult ProjectController::runClipboardBatch(const command::CommandBatch& batch) {
    command::CommandBatch request = batch;
    for (command::CommandRequest& r : request.commands) {
        if (!r.params.contains("sequence"))
            r.params["sequence"] = m_sequence.value;
    }
    command::EnginePolicy policy;
    policy.services = m_clipboardServices;
    command::CommandEngine engine(*m_session, command::builtinCommands(), policy);
    return engine.execute(request);
}

void ProjectController::setSystemClipboardEnabled(bool on) {
    if (on == m_systemClipboard)
        return;
    m_systemClipboard = on;
    disconnect(m_systemConnection);
    m_systemConnection = {};
    if (QClipboard* clipboard = on ? systemClipboard() : nullptr)
        m_systemConnection = connect(clipboard, &QClipboard::dataChanged, this, &ProjectController::onSystemClipboardChanged);
    onSystemClipboardChanged();
}

void ProjectController::onSystemClipboardChanged() {
    const QByteArray bytes = m_systemClipboard ? systemDocument(systemClipboard()) : QByteArray();
    const bool foreign = !bytes.isEmpty() && digest(bytes) != m_systemSeen;
    if (!foreign && !m_systemForeign)
        return;
    m_systemForeign = foreign;
    emit clipboardChanged(); // also for one foreign document replacing another
}

bool ProjectController::adoptSystemClipboard() {
    if (!m_systemClipboard || !m_systemForeign)
        return false;
    const QByteArray bytes = systemDocument(systemClipboard());
    m_systemForeign = false;
    if (bytes.isEmpty()) {
        emit clipboardChanged();
        return false;
    }
    m_systemSeen = digest(bytes);
    auto document = clipboardFromJson(std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size())));
    if (!document) {
        setError(tr("The clipboard holds clips this Wabcut cannot read: %1")
                     .arg(QString::fromStdString(document.error().message)));
        emit clipboardChanged();
        return false;
    }
    m_clipboardServices->set(std::move(*document));
    emit clipboardChanged();
    return true;
}

void ProjectController::publishToSystemClipboard(const std::string& document) {
    QClipboard* clipboard = systemClipboard();
    if (!clipboard)
        return;
    const QByteArray bytes = QByteArray::fromStdString(document);
    m_systemSeen = digest(bytes);
    m_systemForeign = false;
    auto* mime = new QMimeData;
    mime->setData(clipboardMimeType(), bytes);
    clipboard->setMimeData(mime); // the clipboard owns it
}

bool ProjectController::copyToClipboard(bool cut) {
    const std::vector<ClipId> clips = selectedClipIds();
    if (!currentSequence() || clips.empty())
        return false;
    command::Json ids = command::Json::array();
    for (const ClipId id : clips)
        ids.push_back(id.value);
    const bool exported = m_systemClipboard && systemClipboard();
    // Exactly the selection, as Premiere copies it: linked selection already put a clip's partners in it, or not.
    const command::CommandResult result = runClipboardBatch(
        singleCommand(cut ? "clipboard.cut" : "clipboard.copy",
                      command::Json{{"clips", std::move(ids)}, {"linked", false}, {"export", exported}},
                      cut ? QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Cut") : ""));
    if (!result.ok()) {
        setError(resultError(result));
        return false;
    }
    setError({});
    const command::Json& data = firstData(result);
    if (exported && data.contains("document") && data["document"].is_string())
        publishToSystemClipboard(data["document"].get<std::string>());
    emit clipboardChanged();
    return true;
}

void ProjectController::copySelection() {
    copyToClipboard(false);
}

void ProjectController::cutSelection() {
    copyToClipboard(true);
}

void ProjectController::paste() {
    pasteClips(false);
}

void ProjectController::pasteInsert() {
    pasteClips(true);
}

void ProjectController::pasteClips(bool insert) {
    if (!currentSequence())
        return;
    if (m_systemForeign) {
        // Another Wabcut's copy on the system clipboard is the newer one; one that cannot be read is refused rather
        // than replaced by this session's older copy.
        setError({});
        if (!adoptSystemClipboard() && !m_lastError.isEmpty())
            return;
    }
    if (!m_clipboardServices->get())
        return;
    command::Json params{{"at", m_playhead.value}};
    if (insert)
        params["mode"] = "insert";
    const command::CommandResult result =
        runClipboardBatch(singleCommand("clipboard.paste", std::move(params),
                                        insert ? QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Paste Insert")
                                               : QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Paste")));
    if (!result.ok()) {
        setError(resultError(result));
        return;
    }
    setError({});
    const command::Json& data = firstData(result);
    // Premiere selects the pasted clips and parks the playhead after them.
    std::vector<ClipId> pasted;
    if (const auto it = data.find("pasted"); it != data.end() && it->is_array()) {
        for (const command::Json& id : *it)
            pasted.push_back(ClipId{id.get<std::uint64_t>()});
    }
    if (!pasted.empty())
        setSelection(std::move(pasted));
    if (const auto it = data.find("playhead"); it != data.end() && it->contains("ticks"))
        setPlayhead(Ticks{(*it)["ticks"].get<std::int64_t>()});
    QStringList notes;
    if (const auto it = data.find("notes"); it != data.end() && it->is_array()) {
        for (const command::Json& note : *it)
            notes << QString::fromStdString(note.get<std::string>());
    }
    if (!notes.isEmpty())
        emit clipboardNotice(tr("Paste: %1").arg(notes.join(QStringLiteral("; "))));
}

// ---- Keyboard-only editing ------------------------------------------------------------------------------------------

namespace {

/// Tracks in display order (Vn..V1, A1..An).
std::vector<const Track*> displayTracks(const Sequence& seq) {
    std::vector<const Track*> rows;
    for (auto it = seq.videoTracks.rbegin(); it != seq.videoTracks.rend(); ++it)
        rows.push_back(it->get());
    for (const TrackPtr& t : seq.audioTracks)
        rows.push_back(t.get());
    return rows;
}

/// The clip on `track` nearest to `t` (the one under it, else the closest edge).
std::optional<ClipId> nearestClip(const Track& track, Ticks t) {
    if (track.clips.empty())
        return std::nullopt;
    if (const auto index = track.clipIndexAt(t))
        return track.clips[*index].id;
    const std::size_t next = track.clips.lowerBoundStart(t);
    const auto distance = [&](std::size_t i) {
        const Clip& c = track.clips[i];
        return std::min(std::abs((c.start() - t).value), std::abs((c.end() - t).value));
    };
    std::size_t best = next < track.clips.size() ? next : track.clips.size() - 1;
    if (next > 0 && (next >= track.clips.size() || distance(next - 1) <= distance(next)))
        best = next - 1;
    return track.clips[best].id;
}

} // namespace

void ProjectController::setSelectedEdge(const QString& edge) {
    const QString next = edge == QLatin1String("head") || edge == QLatin1String("tail") ? edge : QString();
    if (next == m_selectedEdge)
        return;
    m_selectedEdge = next;
    emit selectedEdgeChanged();
}

void ProjectController::selectAdjacentClip(int direction) {
    const SequencePtr seq = currentSequence();
    if (!seq || direction == 0)
        return;
    std::optional<ClipId> target;
    if (const auto loc = m_primary ? seq->findClip(m_primary) : std::nullopt) {
        const auto index = static_cast<std::int64_t>(loc->index) + (direction > 0 ? 1 : -1);
        if (index >= 0 && index < static_cast<std::int64_t>(loc->track->clips.size()))
            target = loc->track->clips[static_cast<std::size_t>(index)].id;
    } else {
        for (const Track* track : displayTracks(*seq)) {
            const std::size_t i = track->clips.lowerBoundStart(m_playhead);
            if (const auto under = track->clipIndexAt(m_playhead)) {
                target = track->clips[*under].id;
                break;
            }
            if (i < track->clips.size()) {
                target = track->clips[i].id;
                break;
            }
        }
    }
    if (target)
        selectClipById(*target, Replace, true);
}

void ProjectController::selectNextClip(int direction) {
    const SequencePtr seq = currentSequence();
    if (!seq || direction == 0)
        return;
    // Premiere's Select Next / Previous Clip (0.2): the next (previous) clip on the targeted tracks - every track when
    // none is targeted - after (before) the selected clip: every targeted track's clip that starts there, with its links
    // and its group. With nothing selected: the clips under the playhead, else the first ones after it.
    std::vector<const Track*> tracks;
    for (const Track* track : displayTracks(*seq)) {
        if (track->targeted)
            tracks.push_back(track);
    }
    if (tracks.empty())
        tracks = displayTracks(*seq);
    std::vector<ClipId> picked;
    std::optional<Ticks> target;
    const auto loc = m_primary ? seq->findClip(m_primary) : std::nullopt;
    if (loc) {
        const Ticks from = loc->clip().start();
        for (const Track* track : tracks) {
            if (direction > 0) {
                const std::size_t i = track->clips.upperBoundStart(from);
                if (i < track->clips.size() && (!target || track->clips[i].start() < *target))
                    target = track->clips[i].start();
            } else {
                const std::size_t i = track->clips.lowerBoundStart(from);
                if (i > 0 && (!target || track->clips[i - 1].start() > *target))
                    target = track->clips[i - 1].start();
            }
        }
    } else {
        for (const Track* track : tracks) {
            if (const auto under = track->clipIndexAt(m_playhead))
                picked.push_back(track->clips[*under].id);
        }
        if (picked.empty()) {
            for (const Track* track : tracks) {
                const std::size_t i = track->clips.lowerBoundStart(m_playhead);
                if (i < track->clips.size() && (!target || track->clips[i].start() < *target))
                    target = track->clips[i].start();
            }
        }
    }
    if (target) {
        for (const Track* track : tracks) {
            const std::size_t i = track->clips.lowerBoundStart(*target);
            if (i < track->clips.size() && track->clips[i].start() == *target)
                picked.push_back(track->clips[i].id);
        }
    }
    if (picked.empty())
        return;
    const ClipId primary = picked.front(); // the topmost targeted track's (display order)
    setSelection(edit::withGroupedClips(*seq, picked), primary);
}

void ProjectController::selectClipOnAdjacentTrack(int direction) {
    const SequencePtr seq = currentSequence();
    if (!seq || direction == 0)
        return;
    const auto loc = m_primary ? seq->findClip(m_primary) : std::nullopt;
    if (!loc) {
        selectAdjacentClip(1);
        return;
    }
    const std::vector<const Track*> rows = displayTracks(*seq);
    auto row = static_cast<std::int64_t>(std::ranges::find(rows, loc->track) - rows.begin());
    const Ticks anchor = loc->clip().start();
    for (row += direction > 0 ? 1 : -1; row >= 0 && row < static_cast<std::int64_t>(rows.size()); row += direction > 0 ? 1 : -1) {
        if (const auto clip = nearestClip(*rows[static_cast<std::size_t>(row)], anchor)) {
            selectClipById(*clip, Replace, false); // move to that track's clip itself, not its linked partner
            return;
        }
    }
}

void ProjectController::trimSelectedEdge(int frames) {
    const SequencePtr seq = currentSequence();
    if (!seq || !m_primary || frames == 0)
        return;
    const SequenceId seqId = m_sequence;
    const ClipId clip = m_primary;
    const edit::Edge edge = m_selectedEdge == QLatin1String("head") ? edit::Edge::Head : edit::Edge::Tail;
    const Ticks delta = framesToTicks(frames, seq->frameRate);
    applyResult(QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Trim"),
                [=](const ProjectState& s) { return edit::trimEdge(s, seqId, clip, edge, delta); });
}

QVariantMap ProjectController::commandAvailability(const QString& id) const {
    const auto state = [](bool handled, bool enabled, const QString& reason = {}) {
        return QVariantMap{{QStringLiteral("handled"), handled}, {QStringLiteral("enabled"), enabled},
                           {QStringLiteral("reason"), reason}};
    };
    if (!handlesCommand(id))
        return state(false, false);
    const bool hasSelection = !selectedClipIds().empty();
    const bool transitionSelected = static_cast<bool>(m_selectedTransition);
    static const QStringList kDeletes{QStringLiteral("edit.clear"), QStringLiteral("timeline.clearSelection"),
                                      QStringLiteral("edit.rippleDelete"), QStringLiteral("timeline.rippleDelete")};
    if (transitionSelected && kDeletes.contains(id))
        return state(true, true);
    // 0.2 UX (stream T): a selected gap - the deletes close it, Deselect All deselects it.
    if (m_gap && !hasSelection && (kDeletes.contains(id) || id == QLatin1String("edit.deselectAll")))
        return state(true, true);
    static const QStringList kNeedsSelection{
        QStringLiteral("edit.clear"), QStringLiteral("timeline.clearSelection"), QStringLiteral("edit.rippleDelete"),
        QStringLiteral("timeline.rippleDelete"), QStringLiteral("clip.link"), QStringLiteral("clip.enable"),
        QStringLiteral("edit.copy"), QStringLiteral("edit.cut"), QStringLiteral("edit.deselectAll"),
        QStringLiteral("sequence.extendEditToPlayhead"), QStringLiteral("marker.markSelection"),
        QStringLiteral("transport.goToSelectedClipStart"), QStringLiteral("transport.goToSelectedClipEnd"),
        QStringLiteral("sequence.applyDefaultTransitions"),
    };
    if ((kNeedsSelection.contains(id) || id.startsWith(QLatin1String("timeline.nudgeClip"))
         || id.startsWith(QLatin1String("timeline.slideClip")) || id.startsWith(QLatin1String("timeline.slipClip")))
        && !hasSelection)
        return state(true, false, tr("Select a clip first"));
    if (id == QLatin1String("edit.undo") && !canUndo())
        return state(true, false, tr("Nothing to undo"));
    if (id == QLatin1String("edit.redo") && !canRedo())
        return state(true, false, tr("Nothing to redo"));
    if ((id == QLatin1String("edit.paste") || id == QLatin1String("edit.pasteInsert")) && !canPaste())
        return state(true, false, tr("Nothing has been copied"));
    if ((id == QLatin1String("marker.clearIn") || id == QLatin1String("marker.goToIn")) && !markIn())
        return state(true, false, tr("No In mark is set"));
    if ((id == QLatin1String("marker.clearOut") || id == QLatin1String("marker.goToOut")) && !markOut())
        return state(true, false, tr("No Out mark is set"));
    if (id == QLatin1String("marker.clearInAndOut") && !markIn() && !markOut())
        return state(true, false, tr("No In or Out mark is set"));
    if (id == QLatin1String("sequence.rippleTrimPreviousToPlayhead") || id == QLatin1String("sequence.rippleTrimNextToPlayhead")
        || id == QLatin1String("sequence.applyVideoTransition") || id == QLatin1String("sequence.applyAudioTransition")) {
        const SequencePtr seq = currentSequence();
        if (!seq || edit::targetedTracks(*seq).empty())
            return state(true, false, tr("No track is targeted"));
    }
    return state(true, true);
}

} // namespace wabcut::app
