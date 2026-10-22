#include <wabcut/model/SubmixOps.h>

#include "SequenceEditor.h"

#include <wabcut/model/Parameters.h>

#include <algorithm>

namespace wabcut {

namespace {

/// Index of the submix `id`, or nullopt.
std::optional<std::size_t> submixAt(const Sequence& seq, TrackId id) {
    for (std::size_t i = 0; i < seq.submixTracks.size(); ++i) {
        if (seq.submixTracks[i] && seq.submixTracks[i]->id == id) {
            return i;
        }
    }
    return std::nullopt;
}

} // namespace

std::optional<std::vector<std::size_t>> submixProcessingOrder(const Sequence& seq) {
    // Kahn's algorithm over the submix -> submix outputs, picking the lowest index first so the order is stable.
    const std::size_t n = seq.submixTracks.size();
    std::vector<int> inputs(n, 0);
    std::vector<std::optional<std::size_t>> next(n);
    for (std::size_t i = 0; i < n; ++i) {
        const TrackPtr& t = seq.submixTracks[i];
        if (t && t->output.isValid()) {
            next[i] = submixAt(seq, t->output);
            if (next[i]) {
                ++inputs[*next[i]];
            }
        }
    }
    std::vector<std::size_t> order;
    order.reserve(n);
    std::vector<bool> done(n, false);
    while (order.size() < n) {
        std::optional<std::size_t> pick;
        for (std::size_t i = 0; i < n && !pick; ++i) {
            if (!done[i] && inputs[i] == 0) {
                pick = i;
            }
        }
        if (!pick) {
            return std::nullopt; // the rest feed each other: a cycle
        }
        done[*pick] = true;
        order.push_back(*pick);
        if (next[*pick]) {
            --inputs[*next[*pick]];
        }
    }
    return order;
}

std::optional<std::string> audioRoutingProblem(const Sequence& seq) {
    const ParameterInfo& level = parameterInfo(ParamId::VolumeDb);
    const ParameterInfo& balance = parameterInfo(ParamId::Pan);
    const auto checkBus = [&](const Track& t, const std::string& label) -> std::optional<std::string> {
        if (t.output.isValid()) {
            if (!submixAt(seq, t.output)) {
                return label + " outputs to track " + std::to_string(t.output.value) + ", which is not a submix track";
            }
            if (t.output == t.id) {
                return label + " outputs to itself";
            }
        }
        if (t.sends.size() > kMaxTrackSends) {
            return label + " has more than " + std::to_string(kMaxTrackSends) + " sends";
        }
        for (std::size_t k = 0; k < t.sends.size(); ++k) {
            const TrackSend& s = t.sends[k];
            const std::string send = label + " send " + std::to_string(k + 1);
            if (!submixAt(seq, s.submix)) {
                return send + " goes to track " + std::to_string(s.submix.value) + ", which is not a submix track";
            }
            if (!s.level.isValid(level.minValue, level.maxValue) || !s.pan.isValid(balance.minValue, balance.maxValue)) {
                return send + " has a level or pan outside its range, or a malformed curve";
            }
        }
        return std::nullopt;
    };
    for (std::size_t i = 0; i < seq.audioTracks.size(); ++i) {
        if (seq.audioTracks[i]) {
            if (auto p = checkBus(*seq.audioTracks[i], "A" + std::to_string(i + 1))) {
                return p;
            }
        }
    }
    for (std::size_t i = 0; i < seq.submixTracks.size(); ++i) {
        if (seq.submixTracks[i]) {
            if (auto p = checkBus(*seq.submixTracks[i], "S" + std::to_string(i + 1))) {
                return p;
            }
        }
    }
    if (!submixProcessingOrder(seq)) {
        return std::string("submix tracks output into each other in a loop");
    }
    return std::nullopt;
}

bool usesSubmixRouting(const Sequence& seq) {
    if (seq.submixTracks.empty()) {
        return false;
    }
    return std::any_of(seq.audioTracks.begin(), seq.audioTracks.end(),
                       [](const TrackPtr& t) { return t && (t->output.isValid() || !t->sends.empty()); });
}

} // namespace wabcut

namespace wabcut::edit {

using detail::invalidArgument;
using detail::notFound;
using detail::SequenceEditor;

namespace {

/// The audio track a send belongs to (sends go from audio tracks only), unlocked.
std::expected<const Track*, Error> sendingTrack(const Sequence& seq, TrackId track) {
    const Track* t = seq.findTrack(track);
    if (t == nullptr) {
        if (seq.findSubmix(track) != nullptr) {
            return std::unexpected(invalidArgument(detail::trackLabel(seq, track) +
                                                   " is a submix track: sends go from audio tracks"));
        }
        return std::unexpected(notFound("track " + std::to_string(track.value) + " does not exist"));
    }
    if (t->kind != TrackKind::Audio) {
        return std::unexpected(invalidArgument(detail::trackLabel(seq, track) + " is a video track: sends go from audio tracks"));
    }
    if (t->locked) {
        return std::unexpected(invalidArgument("track " + detail::trackLabel(seq, track) + " is locked"));
    }
    return t;
}

std::optional<Error> sendProblem(const Sequence& seq, const TrackSend& send) {
    if (seq.findSubmix(send.submix) == nullptr) {
        return notFound("a send goes to a submix track; track " + std::to_string(send.submix.value) + " is not one");
    }
    const ParameterInfo& level = parameterInfo(ParamId::VolumeDb);
    const ParameterInfo& balance = parameterInfo(ParamId::Pan);
    if (!send.level.isValid(level.minValue, level.maxValue)) {
        return invalidArgument("a send level is -144..+24 dB");
    }
    if (!send.pan.isValid(balance.minValue, balance.maxValue)) {
        return invalidArgument("a send pan is -1 (left) .. 1 (right)");
    }
    return std::nullopt;
}

} // namespace

EditOutcome addSubmixTrack(const ProjectState& state, SequenceId sequence, ChannelFormat format, std::string name) {
    const auto seq = detail::findSequence(state, sequence);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    if ((*seq)->submixTracks.size() >= kMaxSubmixTracks) {
        return std::unexpected(invalidArgument("a sequence has at most " + std::to_string(kMaxSubmixTracks) + " submix tracks"));
    }
    SequenceEditor ed(state, **seq);
    ed.addSubmixTrack(format, std::move(name));
    return ed.finish();
}

EditOutcome removeSubmixTrack(const ProjectState& state, SequenceId sequence, TrackId submix) {
    const auto seq = detail::findSequence(state, sequence);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const Track* bus = (*seq)->findSubmix(submix);
    if (bus == nullptr) {
        return std::unexpected(notFound("track " + std::to_string(submix.value) + " is not a submix track"));
    }
    if (bus->locked) {
        return std::unexpected(invalidArgument(detail::trackLabel(**seq, submix) + " is locked"));
    }
    SequenceEditor ed(state, **seq);
    // What fed it goes to the Mix (its tracks keep their place in the mix, as Premiere reassigns them), its sends go.
    for (const auto* list : {&(*seq)->audioTracks, &(*seq)->submixTracks}) {
        for (const TrackPtr& t : *list) {
            const bool feeds = t->output == submix;
            const bool sends = std::any_of(t->sends.begin(), t->sends.end(), [&](const TrackSend& s) { return s.submix == submix; });
            if (t->id == submix || (!feeds && !sends)) {
                continue;
            }
            Track& m = ed.mutableTrack(t->id);
            if (feeds) {
                m.output = TrackId{};
            }
            std::erase_if(m.sends, [&](const TrackSend& s) { return s.submix == submix; });
        }
    }
    ed.eraseTrack(submix);
    return ed.finish();
}

EditOutcome setTrackOutput(const ProjectState& state, SequenceId sequence, TrackId track, TrackId output) {
    const auto seq = detail::findSequence(state, sequence);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const Track* t = (*seq)->findAudioBus(track);
    if (t == nullptr) {
        if ((*seq)->findTrack(track) != nullptr) {
            return std::unexpected(invalidArgument(detail::trackLabel(**seq, track) +
                                                   " is a video track: only audio tracks and submixes have an output"));
        }
        return std::unexpected(notFound("track " + std::to_string(track.value) + " does not exist"));
    }
    if (t->locked) {
        return std::unexpected(invalidArgument("track " + detail::trackLabel(**seq, track) + " is locked"));
    }
    if (output.isValid() && (*seq)->findSubmix(output) == nullptr) {
        return std::unexpected(notFound("a track outputs to the Mix or a submix track; track " +
                                        std::to_string(output.value) + " is not a submix track"));
    }
    if (t->output == output) {
        return detail::unchanged(state);
    }
    SequenceEditor ed(state, **seq);
    ed.mutableTrack(track).output = output;
    if (!submixProcessingOrder(ed.sequence())) {
        return std::unexpected(invalidArgument(detail::trackLabel(**seq, track) + " cannot output to " +
                                               detail::trackLabel(**seq, output) + ": that submix feeds it (a loop)"));
    }
    return ed.finish();
}

EditOutcome addTrackSend(const ProjectState& state, SequenceId sequence, TrackId track, TrackSend send) {
    const auto seq = detail::findSequence(state, sequence);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const auto t = sendingTrack(**seq, track);
    if (!t) {
        return std::unexpected(t.error());
    }
    if ((*t)->sends.size() >= kMaxTrackSends) {
        return std::unexpected(invalidArgument("track " + detail::trackLabel(**seq, track) + " already has " +
                                               std::to_string(kMaxTrackSends) + " sends"));
    }
    if (auto problem = sendProblem(**seq, send)) {
        return std::unexpected(std::move(*problem));
    }
    SequenceEditor ed(state, **seq);
    ed.mutableTrack(track).sends.push_back(std::move(send));
    return ed.finish();
}

EditOutcome setTrackSend(const ProjectState& state, SequenceId sequence, TrackId track, std::size_t index, TrackSend send) {
    const auto seq = detail::findSequence(state, sequence);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const auto t = sendingTrack(**seq, track);
    if (!t) {
        return std::unexpected(t.error());
    }
    if (index >= (*t)->sends.size()) {
        return std::unexpected(notFound("track " + detail::trackLabel(**seq, track) + " has no send " + std::to_string(index + 1)));
    }
    if (auto problem = sendProblem(**seq, send)) {
        return std::unexpected(std::move(*problem));
    }
    if ((*t)->sends[index] == send) {
        return detail::unchanged(state);
    }
    SequenceEditor ed(state, **seq);
    ed.mutableTrack(track).sends[index] = std::move(send);
    return ed.finish();
}

EditOutcome removeTrackSend(const ProjectState& state, SequenceId sequence, TrackId track, std::size_t index) {
    const auto seq = detail::findSequence(state, sequence);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    const auto t = sendingTrack(**seq, track);
    if (!t) {
        return std::unexpected(t.error());
    }
    if (index >= (*t)->sends.size()) {
        return std::unexpected(notFound("track " + detail::trackLabel(**seq, track) + " has no send " + std::to_string(index + 1)));
    }
    SequenceEditor ed(state, **seq);
    auto& sends = ed.mutableTrack(track).sends;
    sends.erase(sends.begin() + static_cast<std::ptrdiff_t>(index));
    return ed.finish();
}

} // namespace wabcut::edit
