#pragma once

// analysis.speakers and multicam.autoSwitch (AI-13, docs/ai.md "Podcast and interview editing"):
//   analysis.speakers   who speaks when in a multicam source, and which angle shows whom (Speakers.h), from the
//                       transcripts, each angle's mic level and each camera's faces; nothing changes.
//   multicam.autoSwitch the angle cuts of a multicam clip (AutoSwitch.h), as ONE edit: ordinary angle cuts
//                       (multicam.switchAngle's), editable afterwards; each cut with its rule and explanation. The
//                       picture only by default: a podcast's sound stays the mix of its microphones.
// Both run on this computer (no cloud, no cost). The host provides the media signals (SpeakerServices: levels and
// faces); without them the transcripts alone decide, and the user's angle assignments fill in what is unknown.

#include <wabcut/ai/multicam/AutoSwitch.h>
#include <wabcut/ai/multicam/Evidence.h>
#include <wabcut/command/ExecutionContext.h>
#include <wabcut/command/Registry.h>
#include <wabcut/model/Nesting.h>

#include <memory>

namespace wabcut::ai::multicam {

class SpeakerServices : public command::CommandServices {
public:
    explicit SpeakerServices(std::shared_ptr<AngleSignals> angleSignals) : angleSignals(std::move(angleSignals)) {}
    std::shared_ptr<AngleSignals> angleSignals;
};

/// Evidence and timeline of a multicam source, with the services `services` holds (transcripts, signals).
struct SpeakerRun {
    SpeakerEvidence evidence;
    SpeakerTimeline timeline;
};
/// `evidence.faces` false skips the cameras' faces (the slow part of a first analysis of long recordings).
std::expected<SpeakerRun, Error> speakersOf(const ProjectState& state, SequenceId source, command::CommandServices* services,
                                            const SpeakerOptions& options, const EvidenceProgress& progress = {}, std::stop_token stop = {},
                                            const EvidenceOptions& evidence = {});

/// One cut as made on the timeline.
struct AppliedCut {
    PlannedCut cut;
    Ticks timeline{}; ///< where it is on the sequence
    ClipId clip;      ///< the multicam clip piece that starts there
};

struct AutoSwitchResult {
    ProjectState state;
    SequenceId sequence;
    SequenceId source;
    std::vector<AppliedCut> cuts;
    SwitchPlan plan;
    std::vector<std::string> warnings;
};

/// Plans and makes the cuts on every piece of the multicam clip's source on its track (the clip after a clean-up is
/// several pieces). `clip` may be the picture or the sound of the multicam clip; the cuts go on the picture
/// (`target` Video), or on both. Errors: NotFound, InvalidArgument (not a multicam clip, a locked track, retimed
/// pieces), the model's edit errors.
std::expected<AutoSwitchResult, Error> autoSwitch(const ProjectState& state, SequenceId sequence, ClipId clip, const SpeakerRun& speakers,
                                                  const SwitchOptions& options, wabcut::edit::AngleTarget target = wabcut::edit::AngleTarget::Video);

command::Json speakersToJson(const SpeakerRun& run, std::size_t segmentLimit = 400);
command::Json switchesToJson(const AutoSwitchResult& result, const std::function<std::string(Ticks)>& timecode);

void registerMulticamAiCommands(command::CommandRegistry& registry);

} // namespace wabcut::ai::multicam
