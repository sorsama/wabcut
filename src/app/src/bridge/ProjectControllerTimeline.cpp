// ProjectController, 0.2 timeline parity: Clip > Group / Ungroup, Selection Follows Playhead and Go to Gap (Next /
// Previous in Sequence). The model side is ClipGroups.h and TimelineSelection.h.

#include "ProjectController.h"

#include <wabcut/model/ClipGroups.h>
#include <wabcut/model/TimelineSelection.h>

#include <algorithm>

namespace wabcut::app {

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

} // namespace wabcut::app
