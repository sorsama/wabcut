#pragma once
#include <wabcut/command/ExecutionContext.h>
#include <wabcut/command/Registry.h>
namespace wabcut::ai::generation {
// Host-owned approval is deliberately outside model-controlled params. A host must bind consent and a hard cap
// to the exact immutable request before it returns a staged edit; dry runs must never submit a paid request.
class GenerationServices : public command::CommandServices {
public:
    virtual command::CommandOutput generate(std::string_view operation, command::ExecutionContext&) = 0;
    virtual command::Json local(std::string_view operation, const command::Json&) = 0;
};
void registerGenerationCommands(command::CommandRegistry&);
// Pure import/place operation. Rejects occupied extension ranges, locked tracks and mismatched conformed video. A sound
// effect (audio-only media, 0.2) is placed on a new audio track and never extends a clip.
std::expected<command::CommandOutput, Error> stageAsset(const ProjectState&, SequenceId, MediaItem,
    Ticks duration, std::optional<Ticks> at = {}, std::optional<ClipId> extend = {}, bool head = false);
}
