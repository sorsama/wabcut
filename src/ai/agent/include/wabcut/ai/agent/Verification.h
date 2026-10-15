#pragma once

// Deterministic verification of an agent's staged edit (docs/ai.md, "Editing assistant"). Agents fail often and
// claim success anyway (Timeline-Bench: <= 27% of real tasks solved, 93% false success claims), so the assistant
// never reports the model's own verdict: after the model says it is done, verify() checks the staged result
// against the request's explicit constraints and the edit invariants, and the UI shows this report.
//
// Times in constraints are BASE timeline times (the sequence as it was when the user asked). Checks:
//   invariants   validate(): the staged project is structurally sound
//   changes      something was staged when the request asks for an edit
//   duration     the sequence's content length is within the target (a "30-second highlight")
//   protected    the source content of protected ranges is still on the timeline (moved is fine)
//   scope        no content edit outside the referenced range or tracks (ripple shifts are not edits)
//   onlyMarkers  quick actions that only add markers or captions changed no clip
//   tracks       no track added or removed unless the request allows it
//   flashFrames  no new clip shorter than minClipFrames frames
//   gaps         no new gap inside a track's content unless the request allows it (gaps next to titles and graphics
//                are fine: overlays come and go, C.13)
//   locked       locked tracks are untouched
//   others       no other sequence or media item was changed or removed (new sequences are fine: reframes, shorts)
//   newSequences at most maxNewSequences sequences were added (AI-9)

#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/Ids.h>
#include <wabcut/model/ProjectState.h>
#include <wabcut/model/Time.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::ai::agent {

/// What the request explicitly asks for, in base timeline time. Quick actions set these directly; free-form
/// prompts get them from parseConstraints().
struct Constraints {
    std::optional<Ticks> targetDuration;   ///< content length afterwards
    std::optional<Ticks> maxDuration;      ///< "under 60 seconds"
    Ticks durationTolerance{};             ///< 0 = max(1 s, 5% of the target)
    std::vector<TimeRange> protectedRanges;
    std::optional<TimeRange> scope;        ///< content edits only inside this range
    std::vector<TrackId> onlyTracks;       ///< content edits only on these tracks (empty = any)
    bool keepTrackCount = true;
    bool allowClipEdits = true;            ///< false: only markers and captions may change
    bool allowGaps = false;
    int minClipFrames = 2;
    bool expectChanges = true;
    std::size_t maxNewSequences = 10; ///< sequences one request may add (reframe.createSequence, shorts.create)

    bool operator==(const Constraints&) const = default;
};

/// Reads explicit constraints from a free-form request: target or maximum durations ("a 30-second highlight",
/// "under 2 minutes"), protected ranges ("keep 00:01:00:00-00:01:30:00", "don't touch 1:00 to 1:30"), a scope
/// ("between 00:02:00:00 and 00:03:00:00"), tracks ("only on A1"), gaps ("lift", "leave gaps") and track changes
/// ("add a track"). Only unambiguous phrases are read; anything else is left to the model. Timecodes use the
/// sequence's rate.
Constraints parseConstraints(std::string_view prompt, const ProjectState& state, SequenceId sequence);

struct VerificationItem {
    std::string id;     ///< "duration", "protected", ... (stable)
    std::string label;  ///< what was checked, for the UI ("Length is 0:30 ± 0:01.5")
    bool passed = true;
    std::string detail; ///< what was found ("0:42: 12 s too long"), empty when passed
    bool blocking = true; ///< a failure makes the whole report fail (informational items do not)

    bool operator==(const VerificationItem&) const = default;
};

struct VerificationReport {
    std::vector<VerificationItem> items;

    bool passed() const;
    /// The failing blocking items as one message for the model ("- duration: 0:42 ...").
    std::string failuresForModel() const;
    const VerificationItem* item(std::string_view id) const;
};

/// Checks the staged `after` against `before` (the base) for `sequence` and the constraints.
/// `commandsStaged` is the number of staged mutating commands.
VerificationReport verify(const ProjectState& before, const ProjectState& after, SequenceId sequence,
                          const Constraints& constraints, std::size_t commandsStaged);

/// "1:02.5" (minutes:seconds) for durations in reports; "0:30" for whole seconds.
std::string formatDuration(Ticks t);

} // namespace wabcut::ai::agent
