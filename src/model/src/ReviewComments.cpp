// Comments from a review link (ReviewComments.h).

#include <wabcut/model/ReviewComments.h>

#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ReviewOps.h>
#include <wabcut/model/TitleOps.h>

#include <nlohmann/json.hpp>

#include <algorithm>

namespace wabcut {

namespace {

using json = nlohmann::json;

/// Control characters (but newline and tab) and the bidirectional overrides are removed, so a comment cannot reorder the text
/// around it, and "<" and ">" become the single quotation marks; the rest is kept as written.
std::string cleaned(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        const auto b = static_cast<unsigned char>(text[i]);
        if (b < 0x20 && b != '\n' && b != '\t') {
            ++i;
            continue;
        }
        if (b == 0x7f) {
            ++i;
            continue;
        }
        // U+202A..U+202E (E2 80 AA..AE), U+2066..U+2069 (E2 81 A6..A9), U+200E/F, U+061C
        if (b == 0xE2 && i + 2 < text.size()) {
            const auto b1 = static_cast<unsigned char>(text[i + 1]);
            const auto b2 = static_cast<unsigned char>(text[i + 2]);
            if ((b1 == 0x80 && ((b2 >= 0xAA && b2 <= 0xAE) || b2 == 0x8E || b2 == 0x8F)) || (b1 == 0x81 && b2 >= 0xA6 && b2 <= 0xA9)) {
                i += 3;
                continue;
            }
        }
        if (b == 0xD8 && i + 1 < text.size() && static_cast<unsigned char>(text[i + 1]) == 0x9C) {
            i += 2;
            continue;
        }
        // Angle brackets become the single quotation marks that look like them: whatever shows a note (the panels, the markers made
        // from it) can never read a stranger's text as markup.
        if (text[i] == '<') {
            out += "\xE2\x80\xB9";
            ++i;
            continue;
        }
        if (text[i] == '>') {
            out += "\xE2\x80\xBA";
            ++i;
            continue;
        }
        out += text[i++];
    }
    return out;
}

std::string trimmed(std::string text) {
    const auto space = [](char c) { return c == ' ' || c == '\n' || c == '\t'; };
    while (!text.empty() && space(text.back())) {
        text.pop_back();
    }
    std::size_t start = 0;
    while (start < text.size() && space(text[start])) {
        ++start;
    }
    return text.substr(start);
}

/// Cuts at a character boundary.
std::string cutUtf8(std::string text, std::size_t maxBytes) {
    if (text.size() <= maxBytes) {
        return text;
    }
    std::size_t cut = maxBytes;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0u) == 0x80u) {
        --cut;
    }
    text.resize(cut);
    return text;
}

double number(const json& j, const char* key, double fallback) {
    const auto it = j.find(key);
    return it != j.end() && it->is_number() ? it->get<double>() : fallback;
}

} // namespace

std::expected<ReviewCommentsFile, Error> parseReviewComments(std::string_view text) {
    if (text.size() > kMaxReviewCommentsBytes || !io::jsonNestingWithin(text, 16)) {
        return std::unexpected(Error{ErrorCode::ParseError, "The comments file is larger or deeper than allowed."});
    }
    const json doc = json::parse(text, nullptr, false);
    if (!doc.is_object() || doc.value("format", std::string()) != kReviewCommentsFormat || !doc.contains("comments") ||
        !doc["comments"].is_array()) {
        return std::unexpected(Error{ErrorCode::ParseError, "This is not a Wabcut review comments file."});
    }
    if (doc.value("version", 0) > kReviewCommentsVersion || doc.value("version", 0) < 1) {
        return std::unexpected(Error{ErrorCode::VersionMismatch, "The comments file was written by a newer version of the review page."});
    }
    ReviewCommentsFile out;
    const auto linkId = doc.find("link");
    out.link = linkId != doc.end() && linkId->is_string() ? cutUtf8(linkId->get<std::string>(), 64) : std::string();
    const auto reviewer = doc.find("reviewer");
    std::string name = reviewer != doc.end() && reviewer->is_string() ? reviewer->get<std::string>() : std::string();
    if (!edit::isValidUtf8(name)) {
        name.clear();
    }
    name = trimmed(cleaned(name));
    std::replace(name.begin(), name.end(), '\n', ' ');
    std::replace(name.begin(), name.end(), '\t', ' ');
    out.reviewer = name.empty() ? std::string("Reviewer") : cutUtf8(std::move(name), 100);

    std::size_t index = 0;
    for (const json& c : doc["comments"]) {
        ++index;
        if (out.comments.size() >= kMaxReviewComments) {
            out.skipped.push_back("more than " + std::to_string(kMaxReviewComments) + " comments: the rest were left out");
            break;
        }
        const std::string where = "comment " + std::to_string(index);
        if (!c.is_object() || !c.contains("frame") || !c["frame"].is_number_integer() || !c.contains("text") || !c["text"].is_string()) {
            out.skipped.push_back(where + ": it has no frame or text");
            continue;
        }
        ReviewComment comment;
        comment.frame = c["frame"].get<std::int64_t>();
        std::string body = c["text"].get<std::string>();
        if (!edit::isValidUtf8(body)) {
            out.skipped.push_back(where + ": the text is not valid text");
            continue;
        }
        body = trimmed(cleaned(body));
        if (body.empty()) {
            out.skipped.push_back(where + ": the text is empty");
            continue;
        }
        if (body.size() > kMaxReviewTextBytes) {
            body = cutUtf8(std::move(body), kMaxReviewTextBytes);
        }
        comment.text = std::move(body);
        if (comment.frame < 0 || comment.frame > 100'000'000) {
            out.skipped.push_back(where + ": its frame is out of range");
            continue;
        }
        if (c.contains("mark") && c["mark"].is_object()) {
            const json& m = c["mark"];
            ReviewMark mark{number(m, "x", -1.0), number(m, "y", -1.0), number(m, "width", 0.0), number(m, "height", 0.0)};
            const auto unit = [](double v) { return v >= 0.0 && v <= 1.0; };
            if (unit(mark.x) && unit(mark.y)) {
                mark.width = std::clamp(mark.width, 0.0, 1.0 - mark.x);
                mark.height = std::clamp(mark.height, 0.0, 1.0 - mark.y);
                comment.mark = mark;
            }
        }
        const auto when = c.find("createdAt");
        if (when != c.end() && when->is_string() && when->get<std::string>().size() <= 32) {
            comment.createdAt = when->get<std::string>();
        }
        out.comments.push_back(std::move(comment));
    }
    return out;
}

namespace edit {

std::expected<ReviewCommentsImported, Error> importReviewComments(const ProjectState& state, const ReviewCommentsFile& file,
                                                                 const ReviewLinkTarget& target, bool alsoMarkers, std::string_view now) {
    if (file.link != target.link) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "These comments belong to another review link, not to this one."});
    }
    const Sequence* sequence = state.findSequence(target.sequence);
    if (!sequence) {
        return std::unexpected(Error{ErrorCode::NotFound, "The sequence this review link was made from is no longer in the project."});
    }
    ReviewCommentsImported out;
    out.state = state;
    const std::string author = cutUtf8(file.reviewer, kMaxReviewAuthorBytes - kReviewLinkAuthorSuffix.size()) + std::string(kReviewLinkAuthorSuffix);
    for (const ReviewComment& comment : file.comments) {
        const std::string where = "\"" + cutUtf8(comment.text, 40) + "\"";
        const auto offset = framesToTicks(comment.frame, sequence->frameRate, Rounding::Nearest);
        if (!offset) {
            out.conflicts.push_back(where + ": its frame cannot be placed");
            continue;
        }
        if (target.duration.value > 0 && *offset >= target.duration) {
            out.conflicts.push_back(where + ": it is after the end of the review copy");
            continue;
        }
        ReviewNote note;
        note.sequence = target.sequence;
        note.start = target.start + *offset;
        note.mark = comment.mark;
        note.author = author;
        note.text = comment.text;
        note.createdAt = comment.createdAt.empty() ? std::string(now) : comment.createdAt;
        const std::vector<ReviewNote>& current = out.state.reviewNotes.get();
        const bool exists = std::any_of(current.begin(), current.end(), [&](const ReviewNote& n) {
            return n.sequence == note.sequence && n.start == note.start && n.text == note.text && n.author == note.author;
        });
        if (exists) {
            ++out.unchanged;
            continue;
        }
        auto added = addReviewNote(out.state, std::move(note));
        if (!added) {
            out.conflicts.push_back(where + ": " + added.error().message);
            continue;
        }
        out.state = std::move(added->state);
        out.notes.push_back(added->id);
        ++out.added;
    }
    if (alsoMarkers && !out.notes.empty()) {
        const std::size_t before = out.state.findSequence(target.sequence)->markers.size();
        auto converted = reviewNotesToMarkers(out.state, target.sequence, out.notes, false);
        if (!converted) {
            return std::unexpected(converted.error());
        }
        out.state = std::move(converted->state);
        out.markers = out.state.findSequence(target.sequence)->markers.size() - before;
    }
    return out;
}

} // namespace edit

} // namespace wabcut
