// Timeline parity (0.2 round): Premiere's Clip > Group / Ungroup (ClipGroups.h) and the clips' fade handles
// (ClipFades.h).

#include "Catalog.h"

#include <wabcut/model/ClipFades.h>
#include <wabcut/model/ClipGroups.h>

namespace wabcut::command::detail {

void registerTimelineCommands(CommandRegistry& registry) {
    addBuiltin(registry,
               {"clip.group", 1, "Group",
                "Groups clips (Premiere's Clip > Group, Ctrl+G): clicking any of them selects the whole group, so it "
                "moves, copies and deletes together; Alt-click selects one clip of it. Clips leave the groups they were "
                "in. Unlike a link, clips on any tracks can be grouped.",
                Capability::EditSequence,
                withSequence({{"clips", clipsParam("Clips to group (at least two).", 2)}}, {"clips"}),
                Json{{"clips", Json::array({42, 43, 57})}}, {"clips.group"},
                [](ExecutionContext& c) {
                    return edited(c.take(edit::groupClips(c.state(), c.sequenceId(), c.clips("clips"))));
                }});
    addBuiltin(registry,
               {"clip.ungroup", 1, "Ungroup",
                "Ungroups clips (Premiere's Clip > Ungroup, Ctrl+Shift+G): every group one of the clips belongs to is "
                "dissolved, all its members included. Clips that are not grouped are left alone.",
                Capability::EditSequence, withSequence({{"clips", clipsParam("Clips whose groups to dissolve.")}}, {"clips"}),
                Json{{"clips", Json::array({42})}}, {"clips.ungroup"},
                [](ExecutionContext& c) {
                    return edited(c.take(edit::ungroupClips(c.state(), c.sequenceId(), c.clips("clips"))));
                }});
    addBuiltin(registry,
               {"clip.setFade", 1, "Set Fade",
                "Sets a clip's fade in (edge \"head\") or fade out (\"tail\"), what dragging the fade handle at a clip's top "
                "corner does: a one-sided transition inside the clip, Cross Dissolve on video (from or to black) and "
                "Constant Power on audio (from or to silence). The duration is clamped to the clip; 0 removes the fade. "
                "An edge that already has a transition with the neighbouring clip is refused. (clip.fade keyframes "
                "opacity or volume instead.)",
                Capability::EditSequence,
                withSequence({{"clip", clipParam("The clip.")},
                              {"edge", schema::enumeration({"head", "tail"},
                                                           "\"head\": the fade in at the clip's In point; \"tail\": the fade "
                                                           "out at its Out point.")},
                              {"duration", timeParam("The fade's length (0 removes it).")}},
                             {"clip", "edge", "duration"}),
                Json{{"clip", 42}, {"edge", "tail"}, {"duration", "00:00:01:00"}}, {},
                [](ExecutionContext& c) {
                    const edit::Edge edge = c.string("edge") == "head" ? edit::Edge::Head : edit::Edge::Tail;
                    edit::EditResult r =
                        c.take(edit::setClipFade(c.state(), c.sequenceId(), c.clip("clip"), edge, c.time("duration")));
                    const auto fade = edit::clipFade(*r.state.findSequence(c.sequenceId()), c.clip("clip"), edge);
                    Json data{{"fade", fade ? timeJson(c, *fade) : Json(nullptr)}};
                    return edited(std::move(r), std::move(data));
                }});
}

} // namespace wabcut::command::detail
