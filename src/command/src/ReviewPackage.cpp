// Review packages (ReviewPackage.h, C.9): notes.json, a self-contained HTML page and thumbnails; import back.

#include <wabcut/command/ReviewPackage.h>

#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ReviewOps.h>
#include <wabcut/model/Timecode.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <system_error>

namespace wabcut::command {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

constexpr std::string_view kFormat = "wabcut-review";

std::string base64(std::string_view data) {
    static constexpr char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < data.size(); i += 3) {
        const auto n = (std::uint32_t{static_cast<unsigned char>(data[i])} << 16) |
                       (std::uint32_t{static_cast<unsigned char>(data[i + 1])} << 8) |
                       std::uint32_t{static_cast<unsigned char>(data[i + 2])};
        out += kTable[(n >> 18) & 63];
        out += kTable[(n >> 12) & 63];
        out += kTable[(n >> 6) & 63];
        out += kTable[n & 63];
    }
    if (i < data.size()) {
        std::uint32_t n = std::uint32_t{static_cast<unsigned char>(data[i])} << 16;
        if (i + 1 < data.size()) {
            n |= std::uint32_t{static_cast<unsigned char>(data[i + 1])} << 8;
        }
        out += kTable[(n >> 18) & 63];
        out += kTable[(n >> 12) & 63];
        out += i + 1 < data.size() ? kTable[(n >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

std::string html(std::string_view text) {
    std::string out;
    for (const char c : text) {
        switch (c) {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        case '\'':
            out += "&#39;";
            break;
        case '\n':
            out += "<br>";
            break;
        default:
            out += c;
        }
    }
    return out;
}

std::string timecodeOf(const ProjectState& state, SequenceId sequence, Ticks t) {
    const Sequence* s = state.findSequence(sequence);
    return s ? ticksToTimecode(t, s->frameRate, s->dropFrame) : std::to_string(t.value);
}

std::string extensionOf(const std::string& mime) {
    if (mime == "image/jpeg") {
        return "jpg";
    }
    if (mime == "image/png") {
        return "png";
    }
    if (mime == "image/svg+xml") {
        return "svg";
    }
    return "bin";
}

json noteJson(const ProjectState& state, const ReviewNote& n, const std::string& frame) {
    json replies = json::array();
    for (const ReviewReply& r : n.replies) {
        replies.push_back(json{{"author", r.author}, {"text", r.text}, {"createdAt", r.createdAt}});
    }
    json out{{"id", n.id.value},
             {"sequence", n.sequence.value},
             {"start", n.start.value},
             {"duration", n.duration.value},
             {"timecode", timecodeOf(state, n.sequence, n.start)},
             {"end", timecodeOf(state, n.sequence, n.end())},
             {"mark", nullptr},
             {"author", n.author},
             {"text", n.text},
             {"createdAt", n.createdAt},
             {"status", std::string(toString(n.status))},
             {"replies", std::move(replies)},
             {"frame", frame.empty() ? json(nullptr) : json(frame)}};
    if (n.mark) {
        out["mark"] = json{{"x", n.mark->x}, {"y", n.mark->y}, {"width", n.mark->width}, {"height", n.mark->height}};
    }
    return out;
}

std::expected<void, Error> writeText(const fs::path& file, std::string_view text) {
    return io::writeFileAtomically(file, text);
}

} // namespace

std::expected<ReviewFrame, Error> PlaceholderFrames::frame(const ProjectState& state, SequenceId sequence, Ticks time,
                                                          int width) const {
    const Sequence* s = state.findSequence(sequence);
    if (!s) {
        return std::unexpected(Error{ErrorCode::NotFound, "no such sequence"});
    }
    const int w = std::clamp(width, 16, 4096);
    const int h = std::max(1, static_cast<int>(static_cast<std::int64_t>(w) * s->height / std::max(1, s->width)));
    const std::string tc = ticksToTimecode(time, s->frameRate, s->dropFrame);
    std::string svg = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" + std::to_string(w) + "\" height=\"" +
                      std::to_string(h) + "\" viewBox=\"0 0 " + std::to_string(w) + " " + std::to_string(h) +
                      "\"><rect width=\"100%\" height=\"100%\" fill=\"#24242b\"/><text x=\"50%\" y=\"45%\" fill=\"#d8d8e0\" "
                      "font-family=\"sans-serif\" font-size=\"" +
                      std::to_string(std::max(10, h / 9)) + "\" text-anchor=\"middle\">" + html(s->name) +
                      "</text><text x=\"50%\" y=\"65%\" fill=\"#a8a8b8\" font-family=\"monospace\" font-size=\"" +
                      std::to_string(std::max(10, h / 10)) + "\" text-anchor=\"middle\">" + tc + "</text></svg>";
    return ReviewFrame{"image/svg+xml", std::move(svg)};
}

std::string reviewPackageHtml(const ProjectState& state, std::span<const ReviewNote> notes,
                              const std::vector<std::pair<ReviewNoteId, ReviewFrame>>& frames, const std::string& title) {
    std::string out =
        "<!doctype html>\n<html lang=\"en\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" "
        "content=\"width=device-width, initial-scale=1\"><title>" +
        html(title) +
        "</title><style>\n"
        ":root{--bg:#f6f6f8;--card:#fff;--text:#1d1d22;--muted:#5d5d6a;--open:#c4302b;--done:#2e8540;--line:#dcdce4}\n"
        "@media (prefers-color-scheme:dark){:root{--bg:#141418;--card:#1f1f25;--text:#ececf1;--muted:#a3a3b2;"
        "--line:#33333d}}\n"
        "body{margin:0;padding:24px 16px;background:var(--bg);color:var(--text);font:15px/1.5 system-ui,sans-serif}\n"
        "main{max-width:960px;margin:0 auto}h1{font-size:22px;margin:0 0 4px}.meta{color:var(--muted);margin:0 0 20px}\n"
        "article{display:flex;gap:16px;flex-wrap:wrap;background:var(--card);border:1px solid var(--line);"
        "border-radius:10px;padding:14px;margin:0 0 14px}\n"
        ".shot{position:relative;flex:0 0 320px;max-width:100%}.shot img{display:block;width:100%;border-radius:6px}\n"
        ".mark{position:absolute;border:2px solid #ffcc00;border-radius:3px;box-sizing:border-box}\n"
        ".body{flex:1 1 280px;min-width:0}.tc{font-family:ui-monospace,monospace;color:var(--muted)}\n"
        ".status{display:inline-block;font-size:12px;padding:1px 8px;border-radius:999px;color:#fff;margin-left:8px}\n"
        ".open{background:var(--open)}.resolved{background:var(--done)}\n"
        ".reply{border-left:3px solid var(--line);padding-left:10px;margin-top:8px;color:var(--muted)}\n"
        "</style></head><body><main><h1>" +
        html(title) + "</h1><p class=\"meta\">" + std::to_string(notes.size()) + " review note" +
        (notes.size() == 1 ? "" : "s") + "</p>\n";
    for (const ReviewNote& n : notes) {
        const Sequence* s = state.findSequence(n.sequence);
        out += "<article id=\"note-" + std::to_string(n.id.value) + "\">";
        const auto frame = std::find_if(frames.begin(), frames.end(), [&](const auto& f) { return f.first == n.id; });
        if (frame != frames.end()) {
            out += "<div class=\"shot\"><img alt=\"Frame at " + html(timecodeOf(state, n.sequence, n.start)) +
                   "\" src=\"data:" + frame->second.mime + ";base64," + base64(frame->second.bytes) + "\">";
            if (n.mark) {
                char style[160];
                std::snprintf(style, sizeof style, "left:%.3f%%;top:%.3f%%;width:%.3f%%;height:%.3f%%;%s",
                              n.mark->x * 100.0, n.mark->y * 100.0, n.mark->width * 100.0, n.mark->height * 100.0,
                              n.mark->width == 0.0 ? "min-width:12px;min-height:12px;margin:-6px 0 0 -6px;border-radius:50%" : "");
                out += "<div class=\"mark\" style=\"" + std::string(style) + "\"></div>";
            }
            out += "</div>";
        }
        out += "<div class=\"body\"><div><span class=\"tc\">" + html(s ? s->name : std::string("?")) + " &middot; " +
               html(timecodeOf(state, n.sequence, n.start)) +
               (n.duration.value > 0 ? " - " + html(timecodeOf(state, n.sequence, n.end())) : std::string()) +
               "</span><span class=\"status " + std::string(toString(n.status)) + "\">" +
               std::string(toString(n.status)) + "</span></div><p><strong>" + html(n.author) + "</strong><br>" +
               html(n.text) + "</p>";
        for (const ReviewReply& r : n.replies) {
            out += "<div class=\"reply\"><strong>" + html(r.author) + "</strong>: " + html(r.text) + "</div>";
        }
        out += "</div></article>\n";
    }
    out += "</main></body></html>\n";
    return out;
}

std::expected<ReviewPackageWritten, Error> writeReviewPackage(const ProjectState& state, const fs::path& folder,
                                                              std::span<const ReviewNote> notes,
                                                              const ReviewFrameSource* frames, const std::string& title,
                                                              const std::string& createdAt) {
    std::error_code ec;
    fs::create_directories(folder / "frames", ec);
    if (ec) {
        return std::unexpected(Error{ErrorCode::IoError, "cannot create the package folder " + pathToUtf8(folder)});
    }
    ReviewPackageWritten out;
    std::vector<std::pair<ReviewNoteId, ReviewFrame>> made;
    json list = json::array();
    std::vector<std::uint64_t> sequences;
    for (const ReviewNote& n : notes) {
        std::string framePath;
        if (frames) {
            auto f = frames->frame(state, n.sequence, n.start, 640);
            if (f) {
                framePath = "frames/note-" + std::to_string(n.id.value) + "." + extensionOf(f->mime);
                if (auto written = writeText(folder / pathFromUtf8(framePath), f->bytes); !written) {
                    return std::unexpected(written.error());
                }
                made.emplace_back(n.id, std::move(*f));
                ++out.frames;
            } else {
                out.warnings.push_back("note " + std::to_string(n.id.value) + ": no thumbnail (" + f.error().message + ")");
            }
        }
        list.push_back(noteJson(state, n, framePath));
        if (std::find(sequences.begin(), sequences.end(), n.sequence.value) == sequences.end()) {
            sequences.push_back(n.sequence.value);
        }
        ++out.notes;
    }
    json seqs = json::array();
    for (const std::uint64_t id : sequences) {
        if (const Sequence* s = state.findSequence(SequenceId{id})) {
            seqs.push_back(json{{"id", id},
                                {"name", s->name},
                                {"frameRate", json{{"num", s->frameRate.num}, {"den", s->frameRate.den}}},
                                {"dropFrame", s->dropFrame}});
        }
    }
    const json doc{{"format", std::string(kFormat)},
                   {"version", 1},
                   {"title", title},
                   {"createdAt", createdAt},
                   {"sequences", std::move(seqs)},
                   {"notes", std::move(list)}};
    if (auto written = writeText(folder / "notes.json", doc.dump(2)); !written) {
        return std::unexpected(written.error());
    }
    if (auto written = writeText(folder / "index.html", reviewPackageHtml(state, notes, made, title)); !written) {
        return std::unexpected(written.error());
    }
    return out;
}

std::expected<ReviewImported, Error> importReviewPackage(const ProjectState& state, const fs::path& folder) {
    const fs::path file = folder / "notes.json";
    std::error_code ec;
    const auto size = fs::file_size(file, ec);
    if (ec) {
        return std::unexpected(Error{ErrorCode::NotFound, "no notes.json in " + pathToUtf8(folder)});
    }
    if (size > kMaxReviewPackageBytes) {
        return std::unexpected(Error{ErrorCode::ParseError, "the package's notes.json is larger than allowed"});
    }
    std::ifstream in(file, std::ios::binary);
    std::string text(static_cast<std::size_t>(size), '\0');
    if (!in.read(text.data(), static_cast<std::streamsize>(size))) {
        return std::unexpected(Error{ErrorCode::IoError, "cannot read " + pathToUtf8(file)});
    }
    return importReviewPackageData(state, text);
}

std::expected<ReviewImported, Error> importReviewPackageData(const ProjectState& state, std::string_view text) {
    if (text.size() > kMaxReviewPackageBytes || !io::jsonNestingWithin(text)) {
        return std::unexpected(Error{ErrorCode::ParseError, "the package's notes.json is larger or deeper than allowed"});
    }
    const json doc = json::parse(text, nullptr, false);
    if (!doc.is_object() || doc.value("format", std::string()) != kFormat || !doc.contains("notes") ||
        !doc["notes"].is_array()) {
        return std::unexpected(Error{ErrorCode::ParseError, "this is not a Wabcut review package"});
    }
    if (doc.value("version", 0) != 1) {
        return std::unexpected(Error{ErrorCode::VersionMismatch, "the review package was written by a newer version of Wabcut"});
    }
    if (doc["notes"].size() > kMaxReviewNotes) {
        return std::unexpected(Error{ErrorCode::ParseError, "the package holds more notes than allowed"});
    }
    ReviewImported out;
    out.state = state;
    std::size_t index = 0;
    for (const json& j : doc["notes"]) {
        const std::string where = "note " + std::to_string(++index);
        ReviewNote n;
        try {
            n.id = ReviewNoteId{j.value("id", std::uint64_t{0})};
            n.sequence = SequenceId{j.at("sequence").get<std::uint64_t>()};
            n.start = Ticks{j.at("start").get<std::int64_t>()};
            n.duration = Ticks{j.value("duration", std::int64_t{0})};
            n.author = j.value("author", std::string());
            n.text = j.value("text", std::string());
            n.createdAt = j.value("createdAt", std::string());
            const auto status = reviewStatusFromString(j.value("status", std::string("open")));
            if (!status) {
                out.conflicts.push_back(where + ": unknown status");
                continue;
            }
            n.status = *status;
            if (j.contains("mark") && j["mark"].is_object()) {
                const json& m = j["mark"];
                n.mark = ReviewMark{m.at("x").get<double>(), m.at("y").get<double>(), m.value("width", 0.0), m.value("height", 0.0)};
            }
            if (j.contains("replies") && j["replies"].is_array()) {
                if (j["replies"].size() > kMaxReviewReplies) {
                    out.conflicts.push_back(where + ": too many replies");
                    continue;
                }
                for (const json& r : j["replies"]) {
                    n.replies.push_back(ReviewReply{r.value("author", std::string()), r.value("text", std::string()),
                                                    r.value("createdAt", std::string())});
                }
            }
        } catch (const json::exception&) {
            out.conflicts.push_back(where + ": malformed");
            continue;
        }
        if (const auto problem = reviewNoteProblem(n)) {
            out.conflicts.push_back(where + ": " + *problem);
            continue;
        }
        const Sequence* seq = out.state.findSequence(n.sequence);
        if (!seq) {
            out.conflicts.push_back(where + " (" + n.text.substr(0, 40) + "): its sequence " + std::to_string(n.sequence.value) +
                                    " is not in this project");
            continue;
        }
        // The project's note: by id (same sequence), else the same sequence, time and text (a note added twice).
        const ReviewNote* existing = nullptr;
        if (const ReviewNote* byId = n.id.isValid() ? out.state.findReviewNote(n.id) : nullptr;
            byId && byId->sequence == n.sequence) {
            existing = byId;
        } else {
            for (const ReviewNote& p : out.state.reviewNotes) {
                if (p.sequence == n.sequence && p.start == n.start && p.text == n.text) {
                    existing = &p;
                    break;
                }
            }
        }
        if (!existing) {
            auto added = edit::addReviewNote(out.state, n);
            if (!added) {
                out.conflicts.push_back(where + " at " + timecodeOf(out.state, n.sequence, n.start) + ": " + added.error().message);
                continue;
            }
            out.state = std::move(added->state);
            ++out.added;
            continue;
        }
        if (existing->start != n.start || existing->duration != n.duration || existing->text != n.text ||
            existing->mark != n.mark || existing->author != n.author) {
            out.conflicts.push_back(where + " at " + timecodeOf(out.state, n.sequence, n.start) +
                                    ": the note was changed in the project and in the package; the project's is kept");
            continue;
        }
        // Same note: the package's replies (when they extend the project's) and status.
        ReviewNote merged = *existing;
        const bool extends = n.replies.size() >= existing->replies.size() &&
                             std::equal(existing->replies.begin(), existing->replies.end(), n.replies.begin());
        const bool behind = n.replies.size() < existing->replies.size() &&
                            std::equal(n.replies.begin(), n.replies.end(), existing->replies.begin());
        if (extends) {
            merged.replies = n.replies;
        } else if (!behind) {
            for (const ReviewReply& r : n.replies) {
                if (std::find(merged.replies.begin(), merged.replies.end(), r) == merged.replies.end()) {
                    merged.replies.push_back(r);
                }
            }
        }
        merged.status = n.status;
        if (merged == *existing) {
            ++out.unchanged;
            continue;
        }
        auto updated = edit::updateReviewNote(out.state, merged);
        if (!updated) {
            out.conflicts.push_back(where + ": " + updated.error().message);
            continue;
        }
        out.state = std::move(*updated);
        ++out.updated;
    }
    return out;
}

} // namespace wabcut::command
