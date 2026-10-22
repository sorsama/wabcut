#include <wabcut/model/ColorManagementOps.h>

#include "SequenceEditor.h"

#include <string>

namespace wabcut::edit {

using detail::ClipRef;
using detail::invalidArgument;
using detail::SequenceEditor;

std::expected<ProjectState, Error> setColorManagement(const ProjectState& state, const ColorManagement& management) {
    if (const auto problem = colorManagementProblem(management)) {
        return std::unexpected(invalidArgument(std::string(*problem)));
    }
    ProjectState next = state;
    next.colorManagement = management;
    return next;
}

EditOutcome setClipInputColorSpace(const ProjectState& state, SequenceId sequence, std::span<const ClipId> clips,
                                   std::optional<InputColorSpace> space) {
    const auto seq = detail::findSequence(state, sequence);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const auto selection = detail::resolveClips(**seq, clips, LinkMode::IgnoreLinks);
    if (!selection) {
        return std::unexpected(selection.error());
    }
    SequenceEditor ed(state, **seq);
    for (const ClipRef& ref : *selection) {
        const Clip& current = ed.track(ref.track).clips[ref.index];
        if (current.stream.kind != StreamKind::Video || current.isNested() || current.hasUnknownSource()) {
            return std::unexpected(invalidArgument("clip " + std::to_string(ref.clip.value) +
                                                   " is not a video clip of a media item: only those have an input colour space"));
        }
        if (current.content().inputColorSpace == space) {
            continue;
        }
        Clip& c = ed.mutableClip(ref.track, ref.index);
        ClipContent content = c.content();
        content.inputColorSpace = space;
        c.setContent(std::move(content));
    }
    return ed.finish();
}

} // namespace wabcut::edit
