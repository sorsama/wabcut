// Which media a source-anchored redaction covers (RedactionMedia.h).

#include <wabcut/model/RedactionMedia.h>

namespace wabcut {

namespace {

/// Chains longer than this are not followed further (the model never makes one: a subclip of a subclip points at the
/// master, a stem at its source); the item reached so far is the root.
constexpr int kMaxRootHops = 16;

} // namespace

bool derivationKeepsTime(std::string_view operation) {
    // Stems are separated from the whole file and keep its length (AudioCommands, audio.separateStems). A remix
    // ("remix") rearranges the music in time: its source's media times mean nothing in it.
    return operation == "separateStems";
}

MediaId redactionRoot(const ProjectState& state, MediaId media) {
    MediaId current = media;
    for (int hop = 0; hop < kMaxRootHops; ++hop) {
        const MediaItem* item = state.findMedia(current);
        if (!item) {
            return current; // removed: still the anchor of what was derived from it
        }
        if (item->subclip && item->subclip->master.isValid() && item->subclip->master != current) {
            current = item->subclip->master;
            continue;
        }
        if (item->derived && item->derived->source.isValid() && item->derived->source != current &&
            derivationKeepsTime(item->derived->operation)) {
            current = item->derived->source;
            continue;
        }
        return current;
    }
    return current;
}

bool redactionCovers(const ProjectState& state, const Redaction& redaction, MediaId media) {
    return redaction.media == media || redactionRoot(state, redaction.media) == redactionRoot(state, media);
}

std::vector<const Redaction*> redactionsCovering(const ProjectState& state, MediaId media) {
    std::vector<const Redaction*> out;
    if (state.redactions.empty()) {
        return out;
    }
    const MediaId root = redactionRoot(state, media);
    for (const Redaction& r : state.redactions) {
        if (r.media == media || redactionRoot(state, r.media) == root) {
            out.push_back(&r);
        }
    }
    return out;
}

} // namespace wabcut
