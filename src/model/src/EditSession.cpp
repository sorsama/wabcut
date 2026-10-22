#include <wabcut/model/EditSession.h>

#include <algorithm>
#include <unordered_set>
#include <variant>

namespace wabcut {

namespace {

constexpr std::size_t kNodeOverhead = 64; // allocation + control block, roughly

std::size_t sequenceBytes(const Sequence& s) {
    std::size_t bytes = sizeof(Sequence) + kNodeOverhead + s.name.size() + s.markers.size() * sizeof(Marker);
    for (const Marker& m : s.markers) {
        bytes += m.name.size() + m.comment.size() + m.url.size() + m.target.size();
    }
    return bytes;
}

std::size_t mediaBytes(const MediaItem& m) {
    std::size_t bytes = sizeof(MediaItem) + kNodeOverhead + m.name.size() + m.locator.path.size() +
                        m.locator.relativePath.size() + m.info.video.size() * sizeof(VideoStreamInfo) +
                        m.info.audio.size() * sizeof(AudioStreamInfo) +
                        (m.generator ? m.generator->text.size() + m.generator->fontFamily.size() +
                                           m.generator->fontStyle.size()
                                     : 0) +
                        (m.generated ? m.generated->provider.size() + m.generated->model.size() + 96 : 0) +
                        m.markers.size() * sizeof(Marker);
    for (const Marker& marker : m.markers) {
        bytes += marker.name.size() + marker.comment.size() + marker.url.size() + marker.target.size();
    }
    return bytes;
}

// Project-level collections (C.10). Plain vectors are copied by every snapshot; SharedLists only when an edit changed
// them. Without them the budget missed what a large project spends per step (1,000 review notes: ~300 KB a step).

std::size_t valueBytes(const ParamValue& v) {
    const Curve* curve = std::get_if<Curve>(&v.data);
    return curve ? curve->keys.size() * sizeof(Keyframe) : 0;
}

std::size_t redactionBytes(const Redaction& r) {
    std::size_t bytes = sizeof(Redaction) + r.name.size() + r.region.name.size() + r.audioRanges.size() * sizeof(TimeRange) +
                        r.exceptions.size() * sizeof(ClipId) + r.region.params.size() * sizeof(MaskParam);
    for (const MaskParam& p : r.region.params) {
        bytes += p.key.size() + valueBytes(p.value);
    }
    for (const MaskPathKey& k : r.region.path) {
        bytes += sizeof(MaskPathKey) + k.vertices.size() * sizeof(MaskVertex);
    }
    return bytes;
}

std::size_t noteBytes(const ReviewNote& n) {
    std::size_t bytes = sizeof(ReviewNote) + n.author.size() + n.text.size() + n.createdAt.size();
    for (const ReviewReply& r : n.replies) {
        bytes += sizeof(ReviewReply) + r.author.size() + r.text.size() + r.createdAt.size();
    }
    return bytes;
}

std::size_t editsBytes(const TranscriptEdits& e) {
    std::size_t bytes = sizeof(TranscriptEdits) + e.labels.size() * sizeof(SpeakerLabel) +
                        e.speakerSpans.size() * sizeof(SpeakerSpan) + e.corrections.size() * sizeof(WordCorrection);
    for (const WordCorrection& c : e.corrections) {
        bytes += c.original.size() + c.text.size();
    }
    return bytes;
}

template <class T, class Fn>
std::size_t listBytes(const std::vector<T>& items, Fn&& itemBytes) {
    std::size_t bytes = 0;
    for (const T& item : items) {
        bytes += itemBytes(item);
    }
    return bytes;
}

std::size_t projectListBytes(const ProjectState& older, const ProjectState& newer) {
    std::size_t bytes = older.bins.size() * sizeof(Bin) + older.assets.size() * sizeof(ProjectAsset) +
                        older.speakers.size() * sizeof(Speaker);
    if (!older.redactions.sharesWith(newer.redactions)) { // shared between snapshots until changed (C.10, I.8)
        bytes += listBytes(older.redactions.get(), redactionBytes);
    }
    if (!older.reviewNotes.sharesWith(newer.reviewNotes)) {
        bytes += listBytes(older.reviewNotes.get(), noteBytes);
    }
    if (!older.transcriptEdits.sharesWith(newer.transcriptEdits)) {
        bytes += listBytes(older.transcriptEdits.get(), editsBytes);
    }
    if (!older.captionPresets.sharesWith(newer.captionPresets)) {
        bytes += older.captionPresets.size() * sizeof(CaptionPreset);
    }
    return bytes;
}

std::size_t trackExclusiveBytes(const Track& older, const Track* newer) {
    std::size_t bytes = sizeof(Track) + kNodeOverhead + older.name.size() + older.transitions.size() * sizeof(Transition);
    std::unordered_set<const void*> shared;
    if (newer) {
        for (std::size_t c = 0; c < newer->clips.chunkCount(); ++c) {
            shared.insert(newer->clips.chunkIdentity(c));
        }
    }
    for (std::size_t c = 0; c < older.clips.chunkCount(); ++c) {
        if (!shared.contains(older.clips.chunkIdentity(c))) {
            bytes += kNodeOverhead + older.clips.chunkSize(c) * sizeof(Clip);
        }
    }
    return bytes;
}

const Track* findTrack(const Sequence* s, TrackId id) { return s ? s->findTrack(id) : nullptr; }

} // namespace

std::size_t estimateExclusiveBytes(const ProjectState& older, const ProjectState& newer) {
    std::size_t bytes = projectListBytes(older, newer);
    for (const MediaPtr& m : older.media) {
        const MediaItem* other = newer.findMedia(m->id);
        if (other != m.get()) {
            bytes += mediaBytes(*m);
        }
    }
    for (const SequencePtr& s : older.sequences) {
        const SequencePtr other = newer.sequencePtr(s->id);
        if (other == s) {
            continue;
        }
        bytes += sequenceBytes(*s);
        s->forEachTrack([&](const Track& t) {
            const Track* match = findTrack(other.get(), t.id);
            if (match != &t) {
                bytes += trackExclusiveBytes(t, match);
            }
        });
    }
    return bytes;
}

EditSession::EditSession(ProjectState initial, EditSessionOptions options)
    : m_current(std::make_shared<const ProjectState>(std::move(initial))), m_options(options) {
    raiseIdHighWater(*snapshot());
}

EditSession::~EditSession() { releasePending(); }

ProjectSnapshot EditSession::snapshot() const noexcept { return m_current.load(std::memory_order_acquire); }

void EditSession::raiseIdHighWater(const ProjectState& state) noexcept {
    m_idHighWater = std::max(m_idHighWater, state.ids.next);
}

ProjectState EditSession::stateForSave() const {
    ProjectState state = *snapshot();
    state.ids.next = std::max(state.ids.next, m_idHighWater);
    return state;
}

void EditSession::retire(ProjectSnapshot snapshot) {
    if (!snapshot) {
        return;
    }
    if (!m_options.deferRelease) {
        return; // the last reference (if this is it) goes away here
    }
    const std::lock_guard lock(m_releaseMutex);
    m_releaseQueue.push_back(std::move(snapshot));
}

std::size_t EditSession::pendingReleases() const {
    const std::lock_guard lock(m_releaseMutex);
    return m_releaseQueue.size();
}

std::size_t EditSession::releasePending() {
    std::vector<ProjectSnapshot> doomed;
    {
        const std::lock_guard lock(m_releaseMutex);
        doomed.swap(m_releaseQueue);
    }
    return doomed.size(); // freed here, outside the lock
}

void EditSession::pushUndo(Step step, const ProjectState& successor) {
    step.bytes = estimateExclusiveBytes(*step.state, successor);
    m_undoBytes += step.bytes;
    m_undo.push_back(std::move(step));
    enforceBudget();
}

void EditSession::enforceBudget() {
    while (m_undo.size() > m_options.undoLimit ||
           (m_undoBytes > m_options.undoMemoryBudget && m_undo.size() > 1)) {
        m_undoBytes -= m_undo.front().bytes;
        retire(std::move(m_undo.front().state));
        m_undo.pop_front();
    }
}

std::expected<ChangeSet, Error> EditSession::commit(std::string_view label, const ProjectState& next,
                                                    std::string_view mergeKey) {
    const ProjectSnapshot current = snapshot();
    return commitFrom(*current, label, next, mergeKey);
}

std::expected<ChangeSet, Error> EditSession::commitFrom(const ProjectState& base, std::string_view label,
                                                        const ProjectState& next, std::string_view mergeKey) {
    if (isSameRevision(base, next)) {
        return ChangeSet{}; // no-op edits do not create undo steps
    }
    if (m_options.validateEdits) {
        if (auto valid = validate(next, Validation::Structure); !valid) {
            return std::unexpected(Error{valid.error().code, "edit '" + std::string(label) +
                                                                 "' rejected: " + valid.error().message});
        }
    }
    const ProjectSnapshot current = snapshot();
    auto published = std::make_shared<const ProjectState>(next);
    raiseIdHighWater(*published);
    ChangeSet changes = diff(*current, *published);

    for (Step& s : m_redo) {
        retire(std::move(s.state));
    }
    m_redo.clear();

    std::string_view stepLabel = label;
    if (inGesture()) {
        stepLabel = m_gestureLabel;
        if (!m_gestureStarted || m_undo.empty()) {
            pushUndo({m_gestureLabel, current, {}, 0}, *published);
            m_gestureStarted = true;
        } else {
            // Replace the gesture's intermediate state; the step still leads back to the gesture start.
            m_undoBytes -= m_undo.back().bytes;
            m_undo.back().bytes = estimateExclusiveBytes(*m_undo.back().state, *published);
            m_undoBytes += m_undo.back().bytes;
            retire(current);
        }
        m_mergeOpen = false;
    } else {
        const bool merge = m_mergeOpen && !mergeKey.empty() && !m_undo.empty() && m_undo.back().mergeKey == mergeKey;
        if (merge) {
            m_undoBytes -= m_undo.back().bytes;
            m_undo.back().bytes = estimateExclusiveBytes(*m_undo.back().state, *published);
            m_undoBytes += m_undo.back().bytes;
            retire(current);
        } else {
            pushUndo({std::string(label), current, std::string(mergeKey), 0}, *published);
        }
        m_mergeOpen = !mergeKey.empty();
    }
    publish(std::move(published), ChangeKind::Edit, stepLabel, changes);
    return changes;
}

std::optional<ChangeSet> EditSession::undo() {
    m_gestureDepth = 0;
    m_gestureStarted = false;
    if (m_undo.empty()) {
        return std::nullopt;
    }
    Step step = std::move(m_undo.back());
    m_undo.pop_back();
    m_undoBytes -= step.bytes;
    const ProjectSnapshot current = snapshot();
    ProjectSnapshot target = std::move(step.state);
    ChangeSet changes = diff(*current, *target);
    step.state = current;
    step.bytes = 0;
    m_redo.push_back(std::move(step));
    m_mergeOpen = false;
    publish(std::move(target), ChangeKind::Undo, m_redo.back().label, changes);
    return changes;
}

std::optional<ChangeSet> EditSession::redo() {
    m_gestureDepth = 0;
    m_gestureStarted = false;
    if (m_redo.empty()) {
        return std::nullopt;
    }
    Step step = std::move(m_redo.back());
    m_redo.pop_back();
    const ProjectSnapshot current = snapshot();
    ProjectSnapshot target = std::move(step.state);
    ChangeSet changes = diff(*current, *target);
    step.state = current;
    const std::string label = step.label;
    pushUndo(std::move(step), *target);
    m_mergeOpen = false;
    publish(std::move(target), ChangeKind::Redo, label, changes);
    return changes;
}

std::optional<std::string> EditSession::undoLabel() const {
    return m_undo.empty() ? std::nullopt : std::optional<std::string>(m_undo.back().label);
}

std::optional<std::string> EditSession::redoLabel() const {
    return m_redo.empty() ? std::nullopt : std::optional<std::string>(m_redo.back().label);
}

std::vector<std::string> EditSession::undoLabels() const {
    std::vector<std::string> out;
    out.reserve(m_undo.size());
    for (const Step& s : m_undo) {
        out.push_back(s.label);
    }
    return out;
}

std::vector<std::string> EditSession::redoLabels() const {
    std::vector<std::string> out;
    out.reserve(m_redo.size());
    for (auto it = m_redo.rbegin(); it != m_redo.rend(); ++it) {
        out.push_back(it->label);
    }
    return out;
}

void EditSession::beginGesture(std::string label) {
    if (m_gestureDepth++ == 0) {
        m_gestureLabel = std::move(label);
        m_gestureStarted = false;
        m_mergeOpen = false;
    }
}

void EditSession::endGesture() {
    if (m_gestureDepth > 0 && --m_gestureDepth == 0) {
        m_gestureStarted = false;
    }
}

std::optional<ChangeSet> EditSession::cancelGesture() {
    const bool started = m_gestureDepth > 0 && m_gestureStarted && !m_undo.empty();
    m_gestureDepth = 0;
    m_gestureStarted = false;
    if (!started) {
        return std::nullopt;
    }
    Step step = std::move(m_undo.back());
    m_undo.pop_back();
    m_undoBytes -= step.bytes;
    const ProjectSnapshot current = snapshot();
    ChangeSet changes = diff(*current, *step.state);
    retire(current);
    publish(std::move(step.state), ChangeKind::GestureCancelled, m_gestureLabel, changes);
    return changes;
}

void EditSession::reset(ProjectState state) {
    for (Step& s : m_undo) {
        retire(std::move(s.state));
    }
    for (Step& s : m_redo) {
        retire(std::move(s.state));
    }
    m_undo.clear();
    m_redo.clear();
    m_undoBytes = 0;
    m_mergeOpen = false;
    m_gestureDepth = 0;
    m_gestureStarted = false;
    const ProjectSnapshot current = snapshot();
    auto next = std::make_shared<const ProjectState>(std::move(state));
    raiseIdHighWater(*next);
    const ChangeSet changes = diff(*current, *next);
    retire(current);
    publish(std::move(next), ChangeKind::Reset, {}, changes);
}

EditSession::Parked EditSession::park(ProjectState replacement) {
    m_gestureDepth = 0;
    m_gestureStarted = false;
    m_mergeOpen = false;
    Parked out;
    out.m_state = snapshot();
    out.m_undo = std::move(m_undo);
    out.m_redo = std::move(m_redo);
    out.m_undoBytes = m_undoBytes;
    out.m_idHighWater = m_idHighWater;
    m_undo.clear();
    m_redo.clear();
    m_undoBytes = 0;
    m_idHighWater = 1;
    auto next = std::make_shared<const ProjectState>(std::move(replacement));
    raiseIdHighWater(*next);
    const ChangeSet changes = diff(*out.m_state, *next);
    publish(std::move(next), ChangeKind::Reset, {}, changes);
    return out;
}

EditSession::Parked EditSession::exchange(Parked other) {
    m_gestureDepth = 0;
    m_gestureStarted = false;
    m_mergeOpen = false;
    Parked out;
    out.m_state = snapshot();
    out.m_undo = std::move(m_undo);
    out.m_redo = std::move(m_redo);
    out.m_undoBytes = m_undoBytes;
    out.m_idHighWater = m_idHighWater;
    m_undo = std::move(other.m_undo);
    m_redo = std::move(other.m_redo);
    m_undoBytes = other.m_undoBytes;
    m_idHighWater = std::max<std::uint64_t>(1, other.m_idHighWater);
    ProjectSnapshot next = other.m_state ? std::move(other.m_state) : std::make_shared<const ProjectState>();
    raiseIdHighWater(*next);
    const ChangeSet changes = diff(*out.m_state, *next);
    publish(std::move(next), ChangeKind::Reset, {}, changes);
    return out;
}

void EditSession::restore(Parked parked) {
    for (Step& s : m_undo) {
        retire(std::move(s.state));
    }
    for (Step& s : m_redo) {
        retire(std::move(s.state));
    }
    m_gestureDepth = 0;
    m_gestureStarted = false;
    m_mergeOpen = false;
    m_undo = std::move(parked.m_undo);
    m_redo = std::move(parked.m_redo);
    m_undoBytes = parked.m_undoBytes;
    m_idHighWater = std::max<std::uint64_t>(1, parked.m_idHighWater);
    ProjectSnapshot next = parked.m_state ? std::move(parked.m_state) : std::make_shared<const ProjectState>();
    raiseIdHighWater(*next);
    const ProjectSnapshot current = snapshot();
    const ChangeSet changes = diff(*current, *next);
    retire(current);
    publish(std::move(next), ChangeKind::Reset, {}, changes);
}

EditSession::ListenerId EditSession::addListener(Listener listener) {
    const ListenerId id = m_nextListenerId++;
    m_listeners.emplace_back(id, std::move(listener));
    return id;
}

void EditSession::removeListener(ListenerId id) {
    std::erase_if(m_listeners, [&](const auto& entry) { return entry.first == id; });
}

void EditSession::publish(ProjectSnapshot state, ChangeKind kind, std::string_view label, const ChangeSet& changes) {
    m_current.store(state, std::memory_order_release);
    m_revision.fetch_add(1, std::memory_order_acq_rel);
    // Copy first: a listener may add or remove listeners, or edit again.
    const std::string labelCopy(label);
    const auto listeners = m_listeners;
    const ChangeEvent event{kind, labelCopy, std::move(state), changes};
    for (const auto& entry : listeners) {
        entry.second(event);
    }
}

} // namespace wabcut
