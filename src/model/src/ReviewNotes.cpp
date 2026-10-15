// Review notes (ReviewNotes.h, ReviewOps.h, C.9).

#include <wabcut/model/ReviewOps.h>

#include <wabcut/model/TitleOps.h>

#include "SequenceEditor.h"

#include <algorithm>
#include <cmath>

namespace wabcut {

std::string_view toString(ReviewStatus status) { return status == ReviewStatus::Resolved ? "resolved" : "open"; }

std::optional<ReviewStatus> reviewStatusFromString(std::string_view name) {
    if (name == "open") {
        return ReviewStatus::Open;
    }
    if (name == "resolved") {
        return ReviewStatus::Resolved;
    }
    return std::nullopt;
}

namespace {

std::optional<std::string> textProblem(const std::string& author, const std::string& text, const std::string& createdAt,
                                       const char* what) {
    if (author.size() > kMaxReviewAuthorBytes || !edit::isValidUtf8(author)) {
        return std::string(what) + " has an author that is not UTF-8 of at most " + std::to_string(kMaxReviewAuthorBytes) +
               " bytes";
    }
    if (text.size() > kMaxReviewTextBytes || !edit::isValidUtf8(text)) {
        return std::string(what) + " has text that is not UTF-8 of at most " + std::to_string(kMaxReviewTextBytes) +
               " bytes";
    }
    if (!createdAt.empty() && !isIso8601Utc(createdAt)) {
        return std::string(what) + " has a creation time that is not ISO-8601 UTC";
    }
    return std::nullopt;
}

} // namespace

std::optional<std::string> reviewNoteProblem(const ReviewNote& note) {
    if (note.start.value < 0 || note.duration.value < 0 || !checkedAdd(note.start, note.duration)) {
        return std::string("a note needs a start and length that are not negative");
    }
    if (const auto problem = textProblem(note.author, note.text, note.createdAt, "the note")) {
        return problem;
    }
    if (note.mark) {
        const ReviewMark& m = *note.mark;
        const bool finite = std::isfinite(m.x) && std::isfinite(m.y) && std::isfinite(m.width) && std::isfinite(m.height);
        if (!finite || m.x < 0.0 || m.y < 0.0 || m.width < 0.0 || m.height < 0.0 || m.x + m.width > 1.0 ||
            m.y + m.height > 1.0) {
            return std::string("a note's mark lies outside the frame (fractions 0..1)");
        }
    }
    if (note.replies.size() > kMaxReviewReplies) {
        return "a note has more than " + std::to_string(kMaxReviewReplies) + " replies";
    }
    for (const ReviewReply& r : note.replies) {
        if (const auto problem = textProblem(r.author, r.text, r.createdAt, "a reply")) {
            return problem;
        }
    }
    return std::nullopt;
}

namespace edit {

using detail::invalidArgument;
using detail::notFound;

namespace {

std::expected<ReviewNote*, Error> noteIn(ProjectState& state, ReviewNoteId id) {
    const auto it = std::lower_bound(state.reviewNotes.begin(), state.reviewNotes.end(), id,
                                     [](const ReviewNote& n, ReviewNoteId v) { return n.id < v; });
    if (it == state.reviewNotes.end() || it->id != id) {
        return std::unexpected(notFound("review note " + std::to_string(id.value) + " does not exist"));
    }
    const auto index = static_cast<std::size_t>(it - state.reviewNotes.begin());
    return &state.reviewNotes.edit()[index];
}

std::expected<void, Error> checkNote(const ProjectState& state, const ReviewNote& note) {
    const Sequence* seq = state.findSequence(note.sequence);
    if (!seq) {
        return std::unexpected(notFound("sequence " + std::to_string(note.sequence.value) + " does not exist"));
    }
    if (const auto problem = reviewNoteProblem(note)) {
        return std::unexpected(invalidArgument(*problem));
    }
    if (note.start > seq->duration) {
        return std::unexpected(invalidArgument("the note starts after the end of the sequence"));
    }
    return {};
}

/// "Client: the logo is too small" -> its first line, at most 128 bytes (whole UTF-8 characters).
std::string firstLine(const std::string& text) {
    std::string line = text.substr(0, text.find('\n'));
    if (line.size() > 128) {
        std::size_t cut = 128;
        while (cut > 0 && (static_cast<unsigned char>(line[cut]) & 0xC0) == 0x80) {
            --cut;
        }
        line = line.substr(0, cut) + "...";
    }
    return line;
}

} // namespace

std::expected<Created<ReviewNoteId>, Error> addReviewNote(const ProjectState& state, ReviewNote note) {
    if (state.reviewNotes.size() >= kMaxReviewNotes) {
        return std::unexpected(invalidArgument("the project has the most review notes it can hold"));
    }
    if (auto ok = checkNote(state, note); !ok) {
        return std::unexpected(ok.error());
    }
    Created<ReviewNoteId> out{state, {}};
    note.id = out.state.ids.allocate<ReviewNoteId>();
    out.id = note.id;
    out.state.reviewNotes.push_back(std::move(note)); // the newest id is the highest: still sorted
    return out;
}

std::expected<ProjectState, Error> replyToReviewNote(const ProjectState& state, ReviewNoteId id, ReviewReply reply) {
    ProjectState out = state;
    const auto note = noteIn(out, id);
    if (!note) {
        return std::unexpected(note.error());
    }
    if ((*note)->replies.size() >= kMaxReviewReplies) {
        return std::unexpected(invalidArgument("the note has the most replies it can hold"));
    }
    if (const auto problem = textProblem(reply.author, reply.text, reply.createdAt, "the reply")) {
        return std::unexpected(invalidArgument(*problem));
    }
    (*note)->replies.push_back(std::move(reply));
    return out;
}

std::expected<ProjectState, Error> setReviewNoteStatus(const ProjectState& state, ReviewNoteId id, ReviewStatus status) {
    ProjectState out = state;
    const auto note = noteIn(out, id);
    if (!note) {
        return std::unexpected(note.error());
    }
    (*note)->status = status;
    return out;
}

std::expected<ProjectState, Error> updateReviewNote(const ProjectState& state, ReviewNote note) {
    if (auto ok = checkNote(state, note); !ok) {
        return std::unexpected(ok.error());
    }
    ProjectState out = state;
    const auto existing = noteIn(out, note.id);
    if (!existing) {
        return std::unexpected(existing.error());
    }
    **existing = std::move(note);
    return out;
}

std::expected<ProjectState, Error> removeReviewNote(const ProjectState& state, ReviewNoteId id) {
    ProjectState out = state;
    const auto note = noteIn(out, id);
    if (!note) {
        return std::unexpected(note.error());
    }
    out.reviewNotes.erase(out.reviewNotes.begin() + (*note - out.reviewNotes.data()));
    return out;
}

EditOutcome reviewNotesToMarkers(const ProjectState& state, SequenceId sequence, std::span<const ReviewNoteId> notes,
                                 bool removeNotes) {
    const auto seq = detail::findSequence(state, sequence);
    if (!seq) {
        return std::unexpected(seq.error());
    }
    std::vector<const ReviewNote*> chosen;
    if (notes.empty()) {
        for (const ReviewNote& n : state.reviewNotes) {
            if (n.sequence == sequence) {
                chosen.push_back(&n);
            }
        }
    } else {
        for (const ReviewNoteId id : notes) {
            const ReviewNote* n = state.findReviewNote(id);
            if (!n) {
                return std::unexpected(notFound("review note " + std::to_string(id.value) + " does not exist"));
            }
            if (n->sequence != sequence) {
                return std::unexpected(invalidArgument("review note " + std::to_string(id.value) + " is on another sequence"));
            }
            chosen.push_back(n);
        }
    }
    if (chosen.empty()) {
        return detail::unchanged(state);
    }
    std::vector<Marker> markers = (*seq)->markers;
    if (markers.size() + chosen.size() > kMaxMarkers) {
        return std::unexpected(invalidArgument("the sequence would have more than " + std::to_string(kMaxMarkers) + " markers"));
    }
    for (const ReviewNote* n : chosen) {
        std::string comment = n->text;
        if (!n->author.empty()) {
            comment += "\n- " + n->author;
        }
        for (const ReviewReply& r : n->replies) {
            comment += "\n" + (r.author.empty() ? std::string("Reply") : r.author) + ": " + r.text;
        }
        if (comment.size() > kMaxMarkerTextBytes) {
            std::size_t cut = kMaxMarkerTextBytes - 3;
            while (cut > 0 && (static_cast<unsigned char>(comment[cut]) & 0xC0) == 0x80) {
                --cut;
            }
            comment = comment.substr(0, cut) + "...";
        }
        Marker m;
        m.start = n->start;
        m.duration = n->duration;
        m.name = firstLine(n->text);
        m.comment = std::move(comment);
        m.colorIndex = n->status == ReviewStatus::Resolved ? 0 : 1; // green once resolved, red while open
        markers.push_back(std::move(m));
    }
    std::stable_sort(markers.begin(), markers.end(), [](const Marker& a, const Marker& b) { return a.start < b.start; });
    ProjectState base = state;
    if (removeNotes) {
        std::erase_if(base.reviewNotes.edit(), [&](const ReviewNote& n) {
            return std::any_of(chosen.begin(), chosen.end(), [&](const ReviewNote* c) { return c->id == n.id; });
        });
    }
    auto result = setSequenceMarkers(base, sequence, std::move(markers));
    if (result && removeNotes) {
        result->changes.reviewNotesChanged = true;
    }
    return result;
}

} // namespace edit

} // namespace wabcut
