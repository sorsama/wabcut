#pragma once

// The shorts commands of the catalog (docs/commands.md, docs/ai.md "Shorts finder"):
//
//   shorts.find    Read         the best 20-90 s moments of a sequence for shorts, with an explainable score
//                               breakdown, a title and a range snapped to sentences and cuts; optionally re-ranked
//                               and titled by the user's cloud chat model (transcript text only, through the consent,
//                               spend and no-cloud guards)
//   shorts.create  EditProject  one new 9:16 sequence per chosen short: the range cut out, optionally cleaned of
//                               fillers and silences, reframed (reframe.*), captioned, with a hook title and
//                               normalised loudness; all shorts of one call are one undo step
//
// They read transcripts from the host's edit::TranscriptServices (and its audio-energy probe), subjects from
// reframe::ReframeServices (cached analyses unless asked to analyse), and the cloud model from ShortsServices.

#include <wabcut/ai/Capability.h>
#include <wabcut/ai/reframe/Reframe.h>
#include <wabcut/ai/shorts/Shorts.h>
#include <wabcut/command/ExecutionContext.h>
#include <wabcut/command/Registry.h>

#include <expected>
#include <functional>
#include <memory>

namespace wabcut::ai::shorts {

inline constexpr std::string_view kGenerator = "shorts/1";
inline constexpr std::size_t kMaxShortsPerCall = 10;

class ShortsServices : public command::CommandServices {
public:
    using ChatOpener = std::function<std::expected<std::unique_ptr<IChatModel>, AiError>()>;
    /// `chat`: the user's chat model through AiPlatform::chat (guarded); null = no cloud re-rank here.
    explicit ShortsServices(ChatOpener chat) : chat(std::move(chat)) {}
    ChatOpener chat;
};

/// A new sequence holding [range) of `source` (clips trimmed, transitions, captions and markers inside kept, links
/// renewed), starting at 0, sized `width` x `height`, appended to the project.
std::expected<reframe::SequenceCopy, Error> cutSequence(const ProjectState& state, SequenceId source, TimeRange range,
                                                        std::string name, int width, int height);

/// Adds shorts.find and shorts.create.
void registerShortsCommands(command::CommandRegistry& registry);

/// A short as the commands report it.
command::Json shortToJson(const ShortCandidate& candidate, std::size_t rank);

} // namespace wabcut::ai::shorts
