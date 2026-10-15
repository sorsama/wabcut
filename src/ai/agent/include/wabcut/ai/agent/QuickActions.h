#pragma once

// One-click requests of the assistant panel. Each is a fixed prompt plus the constraints its result is checked
// against, so "Make a 30-second highlight" is verified to be 30 s long and "Add markers at topic changes" to have
// changed no clip.

#include <wabcut/ai/agent/Verification.h>
#include <wabcut/model/ProjectState.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::ai::agent {

struct QuickAction {
    std::string id;               ///< "removeSilences", "removeFillers", "highlight", "topicMarkers", "beatCut", "captions"
    std::string title;            ///< "Remove silences longer than {value} s"
    std::string description;
    std::string valueLabel;       ///< what {value} is ("Seconds"); empty when the action takes none
    double defaultValue = 0;
    double minValue = 0;
    double maxValue = 0;
    bool needsTranscript = false; ///< works from the transcript of the sequence's media
    bool needsBeats = false;      ///< needs beat detection (the host's analysis services)
    bool takesTrack = false;      ///< {track}: an audio track label
};

/// What the host can offer, which decides the actions it shows.
struct QuickActionContext {
    bool transcripts = false;   ///< transcript services are set up
    bool beatDetection = false; ///< analysis services are set up (C.5 beat tracking); otherwise "Cut to the beat" is hidden
};

/// Every quick action, in the panel's order.
const std::vector<QuickAction>& quickActions();
const QuickAction* findQuickAction(std::string_view id);
/// The actions to show: needsBeats ones only with beat detection. Actions that need a transcript are shown either
/// way (without one the agent says so).
std::vector<QuickAction> visibleQuickActions(const QuickActionContext& context);

struct QuickActionRequest {
    std::string prompt;
    Constraints constraints;
};

/// The request for an action with its value (seconds; defaultValue when nullopt) and track label ("A3": the music
/// track of "Cut to the beat", which its check keeps unchanged). nullopt for an unknown id.
std::optional<QuickActionRequest> quickActionRequest(std::string_view id, std::optional<double> value,
                                                     const ProjectState& state, SequenceId sequence,
                                                     std::string_view track = {});

/// The constraints of a free-form prompt: parseConstraints(), then what an earlier request already asked for
/// (Refine keeps the original request's targets unless the new prompt sets its own).
Constraints mergeConstraints(const Constraints& earlier, const Constraints& parsed);

} // namespace wabcut::ai::agent
