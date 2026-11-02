#include <wabcut/ai/autopilot/AutopilotCommands.h>

#include <wabcut/ai/analysis/AnalysisCommands.h>
#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/ai/search/SearchCommands.h>
#include <wabcut/command/FrameServices.h>
#include <wabcut/command/Schema.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/MediaLocator.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>

namespace wabcut::ai::autopilot {

using command::CommandOutput;
using command::ErrorCode;
using command::ExecutionContext;
using command::fail;
using command::Json;
namespace schema = command::schema;

namespace {

constexpr std::size_t kNoLine = static_cast<std::size_t>(-1);

double rounded(double v, double scale = 1000.0) { return std::round(v * scale) / scale; }
Ticks fromSeconds(double s) { return Ticks{static_cast<std::int64_t>(std::llround(s * static_cast<double>(kTicksPerSecond)))}; }

/// "... (NN%)": the bridges show it as progress.
void progress(ExecutionContext& c, const std::string& what, std::size_t done, std::size_t total) {
    char text[32];
    std::snprintf(text, sizeof text, " (%d%%)", total == 0 ? 100 : static_cast<int>(done * 100 / total));
    c.checkpoint(what + text);
}

std::shared_ptr<edit::TranscriptProvider> transcriptsOf(ExecutionContext& c, bool required) {
    auto* services = command::findService<edit::TranscriptServices>(c.services());
    if ((!services || !services->transcripts) && required) {
        fail(ErrorCode::InvalidRequest, "transcripts are not available here: the host provides none");
    }
    return services ? services->transcripts : nullptr;
}

edit::SequenceTranscript wordsOf(ExecutionContext& c, const Sequence& seq, const std::shared_ptr<edit::TranscriptProvider>& provider) {
    return provider ? edit::SequenceTranscript::build(c.state(), seq.id, *provider) : edit::SequenceTranscript{};
}

Json mediaParam(std::string description) {
    return Json{{"type", Json::array({"integer", "string"})}, {"description", description + " A media id or exact name."}, {"minimum", 1}};
}

Json withSequence(Json properties) {
    properties["sequence"] = schema::integer("The sequence; only needed when the project has several.", 1);
    return Json{{"type", "object"}, {"properties", std::move(properties)}, {"additionalProperties", false}};
}

void add(command::CommandRegistry& registry, command::CommandSpec spec) {
    [[maybe_unused]] const auto added = registry.add(std::move(spec));
}

Json rhythmJson(const Rhythm& r) {
    Json shots = Json::array();
    for (std::size_t i = 0; i < r.shots.size() && i < 600; ++i) {
        shots.push_back(rounded(r.shots[i]));
    }
    Json curve = Json::array();
    for (const double v : r.curve) {
        curve.push_back(rounded(v));
    }
    return Json{{"length", rounded((r.end - r.start).toSeconds())},
                {"cuts", r.cuts.size()},
                {"shotCount", r.shots.size()},
                {"medianShot", rounded(r.median)},
                {"averageShot", rounded(r.mean)},
                {"cutsPerMinute", rounded(r.cutsPerMinute, 10.0)},
                {"curve", std::move(curve)},
                {"shots", std::move(shots)}};
}

// --- autopilot.broll ---------------------------------------------------------------------------------------------------

Json brollMatchJson(ExecutionContext& c, const BrollMatch& m, const std::vector<Line>& lines, std::size_t index) {
    Json j{{"index", index + 1},
           {"media", m.media.value},
           {"mediaName", m.mediaName},
           {"at", rounded(m.at.toSeconds())},
           {"atTicks", m.at.value},
           {"duration", rounded(m.source.duration.toSeconds())},
           {"durationTicks", m.source.duration.value},
           {"sourceIn", rounded(m.source.start.toSeconds())},
           {"sourceInTicks", m.source.start.value},
           {"timecode", c.timecode(m.at)},
           {"score", rounded(m.score, 100.0)},
           {"how", m.how},
           {"evidence", m.evidence},
           {"reason", m.reason}};
    if (m.line != kNoLine) {
        j["line"] = m.line + 1;
    }
    if (m.line < lines.size()) {
        j["text"] = lines[m.line].text;
        j["keywords"] = lines[m.line].keywords;
    }
    return j;
}

CommandOutput brollCommand(ExecutionContext& c) {
    const Sequence& seq = c.sequence();
    const std::shared_ptr<edit::TranscriptProvider> provider = transcriptsOf(c, !c.has("placements"));
    CommandOutput out;
    out.state = c.state();
    std::vector<BrollMatch> matches;
    BrollPlan plan;
    if (c.has("placements")) {
        // The preview's chosen suggestions, placed as they are.
        const Json& list = c.params()["placements"];
        for (std::size_t i = 0; i < list.size(); ++i) {
            const Json& p = list[i];
            const std::string path = "/placements/" + std::to_string(i);
            BrollMatch m;
            m.media = c.mediaValue(p.at("media"), path + "/media");
            const MediaItem* media = c.state().findMedia(m.media);
            m.mediaName = media ? media->name : std::string();
            const auto timeOf = [&](const char* ticks, const char* secs) -> Ticks {
                if (p.contains(ticks)) {
                    return Ticks{p[ticks].get<std::int64_t>()};
                }
                if (p.contains(secs)) {
                    return fromSeconds(p[secs].get<double>());
                }
                fail(ErrorCode::InvalidParams, std::string("a placement needs \"") + secs + "\"", Json{{"path", path}});
            };
            m.at = timeOf("atTicks", "at");
            m.source = TimeRange{timeOf("sourceInTicks", "sourceIn"), timeOf("durationTicks", "duration")};
            if (m.at < Ticks{} || m.source.start < Ticks{} || m.source.duration <= Ticks{}) {
                fail(ErrorCode::InvalidParams, "a placement needs a start of 0 or more and a length", Json{{"path", path}});
            }
            m.how = "chosen";
            m.evidence = "chosen";
            const std::int64_t line = p.value("line", std::int64_t{0});
            m.line = line > 0 ? static_cast<std::size_t>(line - 1) : kNoLine;
            m.score = 1.0;
            matches.push_back(std::move(m));
        }
        std::ranges::sort(matches, [](const BrollMatch& a, const BrollMatch& b) { return a.at < b.at; });
    } else {
        const edit::SequenceTranscript words = wordsOf(c, seq, provider);
        BrollOptions o;
        if (c.has("perMinute")) {
            o.perMinute = c.number("perMinute");
        }
        if (c.has("minSeconds")) {
            o.minSeconds = c.number("minSeconds");
        }
        if (c.has("maxSeconds")) {
            o.maxSeconds = std::max(o.minSeconds, c.number("maxSeconds"));
        }
        if (c.has("gapSeconds")) {
            o.gapSeconds = c.number("gapSeconds");
        }
        if (c.has("introSeconds")) {
            o.introSeconds = c.number("introSeconds");
        }
        if (c.has("minScore")) {
            o.minScore = c.number("minScore");
        }
        PictureSearch pictures;
        auto notIndexed = std::make_shared<std::size_t>(0);
        if (c.boolean("visual", true)) {
            if (auto* services = command::findService<search::SearchServices>(c.services())) {
                auto opened = services->search();
                if (opened && *opened && (*opened)->embedder()) {
                    std::shared_ptr<search::FootageSearch> footage = *opened;
                    const ProjectState& state = c.state();
                    pictures = [footage, &state, notIndexed](const std::string& query, std::span<const MediaId> media) {
                        search::SearchFilters filters;
                        filters.transcript = false;
                        filters.media.assign(media.begin(), media.end());
                        std::vector<PictureHit> hits;
                        auto results = footage->search(state, query, filters, 8);
                        if (!results) {
                            return hits;
                        }
                        *notIndexed = std::max(*notIndexed, results->notIndexed.size());
                        for (const search::SearchHit& h : results->hits) {
                            if (h.similarity) {
                                hits.push_back(PictureHit{h.media, h.range, h.thumbnail, *h.similarity});
                            }
                        }
                        return hits;
                    };
                } else {
                    out.warnings.emplace_back("visual search is off (its model is not installed): B-roll is matched by words only");
                }
            } else {
                out.warnings.emplace_back("no footage search here: B-roll is matched by words only");
            }
        }
        plan = planBroll(c.state(), seq.id, words, provider.get(), pictures, o,
                         [&c](std::size_t done, std::size_t total) { progress(c, "Matching what is said with the library", done, total); });
        if (*notIndexed > 0) {
            out.warnings.push_back(std::to_string(*notIndexed) +
                                   " library item(s) are not indexed for visual search yet: index them in the Library's smart search");
        }
        out.warnings.insert(out.warnings.end(), plan.warnings.begin(), plan.warnings.end());
        matches = plan.matches;
    }
    Json suggestions = Json::array();
    for (std::size_t i = 0; i < matches.size(); ++i) {
        suggestions.push_back(brollMatchJson(c, matches[i], plan.lines, i));
    }
    Json unmatched = Json::array();
    for (const std::size_t i : plan.unmatched) {
        const Line& l = plan.lines[i];
        unmatched.push_back(Json{{"line", i + 1},
                                 {"text", l.text},
                                 {"keywords", l.keywords},
                                 {"at", rounded(l.range.start.toSeconds())},
                                 {"duration", rounded(l.range.duration.toSeconds())},
                                 {"timecode", c.timecode(l.range.start)},
                                 {"prompt", generationPrompt(l)}});
    }
    Json data{{"sequence", seq.id.value},
              {"lines", plan.lines.size()},
              {"candidates", plan.candidates},
              {"visual", plan.visual},
              {"placed", 0},
              {"suggestions", std::move(suggestions)},
              {"unmatched", std::move(unmatched)}};
    if (!matches.empty()) {
        c.checkpoint("Placing the B-roll");
        auto placed = placeBroll(c.state(), seq.id, matches);
        if (!placed) {
            fail(ErrorCode::EditRejected, "the B-roll could not be placed: " + placed.error().message);
        }
        out.state = std::move(placed->state);
        data["placed"] = placed->clips.size();
        data["track"] = Json{{"id", placed->track.track.value},
                             {"name", placed->track.name},
                             {"label", "V" + std::to_string(placed->track.index + 1)},
                             {"created", placed->track.created}};
        Json clips = Json::array();
        for (const ClipId id : placed->clips) {
            clips.push_back(id.value);
            out.createdIds.push_back(id.value);
        }
        data["clips"] = std::move(clips);
    } else if (!c.has("placements")) {
        out.warnings.emplace_back("nothing in the library matched what is said well enough: no B-roll was placed");
    }
    out.data = std::move(data);
    return out;
}

// --- autopilot.publishKit ----------------------------------------------------------------------------------------------

bool writeText(const std::filesystem::path& file, const std::string& text) {
    std::ofstream f(file, std::ios::binary | std::ios::trunc);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    return f.good();
}

std::string kitMarkdown(const PublishingKit& kit, const std::string& title, const std::string& description, const std::string& name,
                        const std::vector<std::string>& thumbs) {
    std::string md = "# Publishing kit: " + name + "\n\n## Title\n\n" + title + "\n";
    if (kit.titles.size() > 1) {
        md += "\nOther ideas:\n\n";
        for (const std::string& t : kit.titles) {
            if (t != title) {
                md += "- " + t + "\n";
            }
        }
    }
    md += "\n## Description\n\n" + description + "\n";
    if (!kit.chapters.empty()) {
        md += "\n## Chapters\n\n```\n" + chaptersText(kit.chapters) + "```\n";
    }
    std::string tags;
    for (const std::string& t : kit.tags) {
        tags += (tags.empty() ? "" : ", ") + t;
    }
    md += "\n## Tags\n\n" + (tags.empty() ? std::string("(none)") : tags) + "\n";
    if (!thumbs.empty()) {
        md += "\n## Thumbnails\n\n";
        for (std::size_t i = 0; i < thumbs.size() && i < kit.thumbnails.size(); ++i) {
            std::string reasons;
            for (const std::string& r : kit.thumbnails[i].reasons) {
                reasons += (reasons.empty() ? "" : ", ") + r;
            }
            md += "- " + thumbs[i] + " at " + youtubeTime(kit.thumbnails[i].time) + " (" + reasons + ")\n";
        }
    }
    if (!kit.warnings.empty()) {
        md += "\n## Notes\n\n";
        for (const std::string& w : kit.warnings) {
            md += "- " + w + "\n";
        }
    }
    md += "\nMade by Wabcut's creator autopilot. Nothing was uploaded.\n";
    return md;
}

CommandOutput publishKitCommand(ExecutionContext& c) {
    const Sequence& seq = c.sequence();
    const std::shared_ptr<edit::TranscriptProvider> provider = transcriptsOf(c, false);
    const edit::SequenceTranscript words = wordsOf(c, seq, provider);
    CommandOutput out;
    out.state = c.state();
    if (!provider) {
        out.warnings.emplace_back("no transcripts here: the kit has chapters from markers and thumbnails only");
    }
    KitOptions o;
    o.thumbnails = static_cast<std::size_t>(c.integer("thumbnails", 4));
    o.chapters = c.string("chapters", "auto");
    auto* services = command::findService<AutopilotServices>(c.services());
    PublishingKit kit = makeKit(c.state(), seq.id, words, services ? services->frames : FramePicture{}, o,
                                [&c](std::size_t done, std::size_t total) { progress(c, "Looking for thumbnail frames", done, total); });
    bool cloud = false;
    if (c.boolean("cloud", false)) {
        if (!services || !services->chat) {
            fail(ErrorCode::InvalidRequest, "\"cloud\" needs the user's chat model: none is set up here");
        }
        if (words.empty()) {
            out.warnings.emplace_back("no transcript to write from: your AI was not asked");
        } else {
            c.checkpoint("Writing the copy with your AI");
            auto model = services->chat();
            if (!model) {
                out.warnings.push_back("your AI is not available (" + model.error().message + "): the kit keeps the copy made on this computer");
            } else {
                const ChatRequest request = kitChatRequest(kitTranscriptText(words), kit.chapters, seq.name);
                auto reply = (*model)->chat(request, {}, {});
                if (!reply) {
                    if (reply.error().code == AiErrorCode::Cancelled) {
                        fail(ErrorCode::Cancelled, "cancelled");
                    }
                    out.warnings.push_back("your AI could not write the copy (" + reply.error().message + "): the kit keeps the copy made on this computer");
                } else if (const std::optional<CloudCopy> copy = parseKitReply(reply->message.text())) {
                    cloud = true;
                    std::vector<std::string> titles = copy->titles;
                    for (const std::string& t : kit.titles) {
                        if (std::ranges::find(titles, t) == titles.end() && titles.size() < 6) {
                            titles.push_back(t);
                        }
                    }
                    kit.titles = std::move(titles);
                    if (!copy->description.empty()) {
                        kit.lead = copy->description;
                    }
                    if (!copy->tags.empty()) {
                        kit.tags = copy->tags;
                    }
                } else {
                    out.warnings.emplace_back("your AI's answer could not be read: the kit keeps the copy made on this computer");
                }
            }
        }
    }
    // The user's own edits from the preview win.
    if (c.has("tags")) {
        kit.tags.clear();
        for (const Json& t : c.params()["tags"]) {
            if (t.is_string() && !t.get<std::string>().empty()) {
                kit.tags.push_back(t.get<std::string>());
            }
        }
    }
    std::string title = kit.titles.empty() ? seq.name : kit.titles.front();
    if (const std::string t = c.string("title"); !t.empty()) {
        title = t;
    }
    const std::string description = c.has("description") ? c.string("description") : kit.description();

    Json chapters = Json::array();
    for (const KitChapter& ch : kit.chapters) {
        chapters.push_back(Json{{"seconds", rounded(ch.start.toSeconds())}, {"time", youtubeTime(ch.start)}, {"title", ch.title}});
    }
    Json thumbnails = Json::array();
    for (const ThumbnailPick& p : kit.thumbnails) {
        Json t{{"seconds", rounded(p.time.toSeconds())},
               {"ticks", p.time.value},
               {"time", youtubeTime(p.time)},
               {"timecode", c.timecode(p.time)},
               {"score", rounded(p.score, 100.0)},
               {"reasons", p.reasons}};
        if (p.media.isValid()) {
            t["media"] = p.media.value;
            t["sourceTicks"] = p.sourceTime.value;
        }
        thumbnails.push_back(std::move(t));
    }
    Json data{{"sequence", seq.id.value},
              {"name", seq.name},
              {"title", title},
              {"titles", kit.titles},
              {"description", description},
              {"lead", kit.lead},
              {"chapters", std::move(chapters)},
              {"chaptersText", chaptersText(kit.chapters)},
              {"chaptersFrom", kit.chaptersFrom},
              {"tags", kit.tags},
              {"thumbnails", std::move(thumbnails)},
              {"cloud", cloud},
              {"written", false}};
    out.warnings.insert(out.warnings.end(), kit.warnings.begin(), kit.warnings.end());

    if (c.has("folder")) {
        const std::filesystem::path parent = pathFromUtf8(c.string("folder"));
        std::error_code ec;
        if (parent.empty() || !std::filesystem::is_directory(parent, ec)) {
            fail(ErrorCode::NotFound, "no folder " + c.string("folder"), Json{{"path", "/folder"}});
        }
        const std::filesystem::path folder = parent / pathFromUtf8(safeFileName(seq.name + " - publishing kit"));
        data["folder"] = pathToUtf8(folder);
        if (!c.dryRun()) {
            std::filesystem::create_directories(folder, ec);
            if (ec) {
                fail(ErrorCode::EditRejected, "cannot make the folder " + pathToUtf8(folder) + ": " + ec.message());
            }
            Json files = Json::array();
            const auto text = [&](const char* name, const std::string& content) {
                if (!writeText(folder / name, content)) {
                    fail(ErrorCode::EditRejected, std::string("cannot write ") + name + " in " + pathToUtf8(folder));
                }
                files.push_back(name);
            };
            text("title.txt", title + "\n");
            text("description.txt", description + "\n");
            if (!kit.chapters.empty()) {
                text("chapters.txt", chaptersText(kit.chapters));
            }
            std::string tags;
            for (const std::string& t : kit.tags) {
                tags += (tags.empty() ? "" : ", ") + t;
            }
            text("tags.txt", tags + "\n");
            // Thumbnails: YouTube's 1280 x 720, cropped to fill, a JPEG under 2 MB.
            std::vector<std::string> thumbs;
            auto* stills = command::findService<command::FrameExportServices>(c.services());
            if (!kit.thumbnails.empty() && (!stills || !stills->writer())) {
                out.warnings.emplace_back("this host cannot render frames: the kit has no thumbnail files");
            } else {
                command::FrameExportServices::Options still;
                still.width = 1280;
                still.height = 720;
                still.fill = true;
                still.quality = 92;
                still.maxBytes = 2'000'000;
                for (std::size_t i = 0; i < kit.thumbnails.size(); ++i) {
                    progress(c, "Writing thumbnail " + std::to_string(i + 1) + " of " + std::to_string(kit.thumbnails.size()), i,
                             kit.thumbnails.size());
                    const std::string name = "thumbnail-" + std::to_string(i + 1) + ".jpg";
                    auto written = stills->writer()(c.state(), seq.id, kit.thumbnails[i].time, folder / name, "jpeg", still);
                    if (!written) {
                        out.warnings.push_back(name + " could not be written: " + written.error().message);
                        continue;
                    }
                    data["thumbnails"][i]["file"] = name;
                    thumbs.push_back(name);
                    files.push_back(name);
                }
            }
            Json kitJson = data;
            kitJson.erase("folder");
            kitJson.erase("written");
            text("publishing-kit.json", kitJson.dump(2) + "\n");
            text("publishing-kit.md", kitMarkdown(kit, title, description, seq.name, thumbs));
            data["files"] = std::move(files);
            data["written"] = true;
        }
    }
    out.data = std::move(data);
    return out;
}

// --- autopilot.pacing --------------------------------------------------------------------------------------------------

CommandOutput pacingCommand(ExecutionContext& c) {
    const Sequence& seq = c.sequence();
    const std::string apply = c.string("apply", "none");
    Rhythm reference;
    Json referenceInfo = Json::object();
    if (c.has("reference")) {
        const MediaId id = c.media("reference");
        const MediaItem* media = c.state().findMedia(id);
        if (!media || !media->info.hasVideo()) {
            fail(ErrorCode::InvalidParams, "the reference needs a picture", Json{{"path", "/reference"}});
        }
        auto* services = command::findService<analysis::AnalysisServices>(c.services());
        if (!services || !services->provider) {
            fail(ErrorCode::InvalidRequest, "shot detection is not available here: the host provides none");
        }
        auto shots = services->provider->shots(*media, 0, analysis::ShotOptions{}, [&c, name = media->name](double fraction) {
            char text[32];
            std::snprintf(text, sizeof text, " (%d%%)", static_cast<int>(std::lround(fraction * 100.0)));
            c.checkpoint("Finding the cuts in '" + name + "'" + text);
            return true;
        });
        if (!shots) {
            fail(shots.error().code == wabcut::ErrorCode::Cancelled ? ErrorCode::Cancelled : ErrorCode::EditRejected,
                 "shot detection of '" + media->name + "' failed: " + shots.error().message);
        }
        std::vector<Ticks> cuts;
        for (const analysis::ShotCut& cut : shots->cuts) {
            cuts.push_back(cut.time);
        }
        const Ticks end = shots->end > shots->start ? shots->end : media->info.duration;
        reference = rhythmOf(shots->start, end, cuts);
        referenceInfo = Json{{"media", id.value}, {"name", media->name}};
    } else if (c.has("referenceCuts")) {
        std::vector<Ticks> cuts;
        for (const Json& v : c.params()["referenceCuts"]) {
            cuts.push_back(fromSeconds(v.get<double>()));
        }
        if (!c.has("referenceLength")) {
            fail(ErrorCode::InvalidParams, "\"referenceCuts\" needs \"referenceLength\"", Json{{"path", "/referenceLength"}});
        }
        reference = rhythmOf(Ticks{}, fromSeconds(c.number("referenceLength")), cuts);
        referenceInfo = Json{{"name", "cuts given"}};
    } else {
        fail(ErrorCode::InvalidParams, "pass the reference video (\"reference\") or its cuts (\"referenceCuts\")", Json{{"path", "/reference"}});
    }
    c.checkpoint("Comparing the rhythms");
    const std::shared_ptr<edit::TranscriptProvider> provider = transcriptsOf(c, false);
    const edit::SequenceTranscript words = wordsOf(c, seq, provider);
    const Rhythm ours = editRhythm(seq);
    PacingOptions o;
    if (c.has("strength")) {
        o.strength = c.number("strength");
    }
    if (c.has("tolerance")) {
        o.tolerance = c.number("tolerance");
    }
    const std::vector<CutCandidate> candidates = cutCandidates(seq, words);
    PacingPlan plan = planPacing(reference, ours, candidates, seq.frameRate, o, words.empty() ? nullptr : &words);

    // The suggestions to apply: all, or the chosen ones (1-based).
    std::vector<bool> chosen(plan.cuts.size(), !c.has("only"));
    if (c.has("only")) {
        for (const Json& v : c.params()["only"]) {
            const std::int64_t i = v.get<std::int64_t>();
            if (i >= 1 && static_cast<std::size_t>(i) <= chosen.size()) {
                chosen[static_cast<std::size_t>(i - 1)] = true;
            }
        }
    }
    Json suggestions = Json::array();
    for (std::size_t i = 0; i < plan.cuts.size(); ++i) {
        const CutSuggestion& s = plan.cuts[i];
        suggestions.push_back(Json{{"index", i + 1},
                                   {"seconds", rounded(s.time.toSeconds())},
                                   {"ticks", s.time.value},
                                   {"timecode", c.timecode(s.time)},
                                   {"kind", s.kind},
                                   {"reason", s.reason},
                                   {"target", rounded(s.targetSeconds)},
                                   {"shot", rounded(s.shotSeconds)},
                                   {"chosen", static_cast<bool>(chosen[i])}});
    }
    referenceInfo.update(rhythmJson(plan.reference));
    // The cuts again (seconds from its start), so a later run can pass them as referenceCuts without analysing again.
    Json cutTimes = Json::array();
    for (const Ticks t : plan.reference.cuts) {
        cutTimes.push_back(rounded((t - plan.reference.start).toSeconds(), 1e6));
    }
    referenceInfo["cutTimes"] = std::move(cutTimes);
    CommandOutput out;
    out.state = c.state();
    out.warnings = plan.warnings;
    std::size_t applied = 0;
    if (apply == "markers") {
        auto next = std::make_shared<Sequence>(seq);
        for (std::size_t i = 0; i < plan.cuts.size(); ++i) {
            if (!chosen[i]) {
                continue;
            }
            Marker m;
            m.start = plan.cuts[i].time;
            m.name = "Cut (pace)";
            m.comment = plan.cuts[i].reason;
            m.colorIndex = 4;
            next->markers.push_back(std::move(m));
            ++applied;
        }
        std::ranges::stable_sort(next->markers, [](const Marker& a, const Marker& b) { return a.start < b.start; });
        out.state = c.state().withSequence(std::move(next));
    } else if (apply == "cuts") {
        ProjectState state = c.state();
        for (std::size_t i = 0; i < plan.cuts.size(); ++i) {
            if (!chosen[i]) {
                continue;
            }
            auto cut = wabcut::edit::addEditToAllTracks(state, seq.id, plan.cuts[i].time);
            if (!cut) {
                out.warnings.push_back("no cut at " + c.timecode(plan.cuts[i].time) + ": " + cut.error().message);
                continue;
            }
            state = std::move(cut->state);
            ++applied;
        }
        out.state = std::move(state);
    }
    out.data = Json{{"sequence", seq.id.value},
                    {"reference", std::move(referenceInfo)},
                    {"edit", rhythmJson(plan.edit)},
                    {"after", rhythmJson(plan.after)},
                    {"suggestions", std::move(suggestions)},
                    {"fasterShots", plan.fasterShots},
                    {"apply", apply},
                    {"applied", applied}};
    return out;
}

} // namespace

void registerAutopilotCommands(command::CommandRegistry& registry) {
    using command::Capability;
    Json placement{{"type", "object"},
                   {"properties",
                    Json{{"media", mediaParam("The B-roll's media item.")},
                         {"at", schema::number("Where it starts on the timeline, in seconds.", 0.0)},
                         {"atTicks", schema::integer("Or exactly, in ticks.", 0)},
                         {"sourceIn", schema::number("The first moment of the media to show, in seconds.", 0.0)},
                         {"sourceInTicks", schema::integer("Or exactly, in ticks.", 0)},
                         {"duration", schema::number("How long, in seconds.", 0.0)},
                         {"durationTicks", schema::integer("Or exactly, in ticks.", 1)},
                         {"line", schema::integer("The line of the transcript it is for (1-based, as the suggestions number them).", 1)}}},
                   {"required", Json::array({"media"})},
                   {"additionalProperties", false}};
    add(registry,
        command::runsModel(
            {"autopilot.broll", 1, "Auto B-roll",
             "Reads what is said in a sequence (its transcript, line by line) and puts matching B-roll from the library on a "
             "free video track: video items the sequence does not use yet, matched by picture (the smart search, when its "
             "model is installed) and by words (the item's name, bin, Description, Comment, clip markers and transcript). "
             "Each clip lasts as long as its line (2 to 6 s), at most 4 per minute, 4 s apart, never over the first 3 s; "
             "the same shot is never used twice. The track is the lowest free video track above the A-roll, else a new "
             "\"B-roll\" track. Returns the suggestions and the lines nothing matched (with a prompt for generating B-roll). "
             "With \"placements\" it places exactly those instead. Needs transcripts. One undo step; dryRun previews it.",
             Capability::EditSequence,
             withSequence(Json{{"perMinute", schema::number("At most this many B-roll clips per minute (default 4).", 0.5, 30.0)},
                               {"minSeconds", schema::number("The shortest B-roll clip, in seconds (default 2).", 0.5, 30.0)},
                               {"maxSeconds", schema::number("The longest B-roll clip, in seconds (default 6).", 0.5, 60.0)},
                               {"gapSeconds", schema::number("Seconds between two B-roll clips at least (default 4).", 0.0, 120.0)},
                               {"introSeconds", schema::number("No B-roll over the first seconds (default 3).", 0.0, 600.0)},
                               {"minScore", schema::number("How well a match must fit, 0..1 (default 0.35).", 0.0, 1.0)},
                               {"visual", schema::boolean("Match by picture too when the visual search model is installed (default true).", true)},
                               {"placements", schema::array(placement, "Place exactly these (the preview's chosen suggestions) instead of searching.", 1)}}),
             Json{{"perMinute", 3}}, {}, brollCommand}));

    add(registry,
        command::runsModel(command::touchesFiles(
            {"autopilot.publishKit", 1, "Publishing Kit",
             "Prepares a sequence's upload: title ideas (its most telling lines, the hook counting extra), a description "
             "(a short lead, the chapters, hashtags), chapters (the sequence's chapter markers, else its transcript's "
             "topics; the first at 0:00, with warnings when YouTube would not show them), tags (the most specific words "
             "and repeated phrases, at most 15 and 500 characters) and thumbnail candidates (highlight markers, chapter "
             "starts, shot middles and telling lines, scored for sharpness, contrast, colour and exposure, 5 s apart). "
             "With \"folder\" it writes them into a \"<sequence> - publishing kit\" folder there: title.txt, "
             "description.txt, chapters.txt, tags.txt, thumbnail-N.jpg (1280 x 720, under 2 MB), publishing-kit.md and "
             "publishing-kit.json. Nothing is uploaded and the project does not change. \"title\", \"description\" and "
             "\"tags\" replace what it would write. With \"cloud\" the user's chat model writes the title ideas, "
             "description and tags from the transcript text (needs a chat model; never the video).",
             Capability::Read,
             withSequence(Json{{"folder", Json{{"type", "string"}, {"description", "Write the kit into a new folder inside this one."}, {"minLength", 1}, {"maxLength", 4096}}},
                               {"thumbnails", schema::integer("Thumbnail candidates (default 4).", 0, 12)},
                               {"chapters", schema::enumeration({"auto", "markers", "topics", "none"},
                                                                "Chapters from the chapter markers, from the transcript's topics, either (\"auto\", the default: markers when there are any) or none.")},
                               {"title", Json{{"type", "string"}, {"description", "The title to write (default: the first idea)."}, {"maxLength", 200}}},
                               {"description", Json{{"type", "string"}, {"description", "The description to write (default: the one made)."}, {"maxLength", 5000}}},
                               {"tags", schema::array(Json{{"type", "string"}, {"maxLength", 100}}, "The tags to write (default: the ones found).")},
                               {"cloud", schema::boolean("Let the user's chat model write the title ideas, description and tags from the transcript text (default false).", false)}}),
             Json{{"thumbnails", 4}, {"chapters", "auto"}}, {}, publishKitCommand},
            {"folder"})));

    add(registry,
        {"autopilot.pacing", 1, "Pacing Match",
         "Compares the cut rhythm of a reference video (its shot cuts, found by shot detection and cached per file) with "
         "the sequence's (where the picture changes) - cuts per minute, the median and average shot length, the shot "
         "length over the running time - and suggests cuts so the sequence's long shots get about as long as the "
         "reference's at the same point: each moved to the best nearby place (a sentence end, a pause, a beat of the beat "
         "grid; never inside a word). Shots already shorter are left alone: it never removes footage. \"apply\": "
         "\"markers\" adds a marker at each suggestion, \"cuts\" adds an edit on all tracks there (one undo step); "
         "\"only\" applies the chosen suggestions (1-based). \"referenceCuts\" (seconds) with \"referenceLength\" "
         "gives a rhythm instead of a video.",
         Capability::EditSequence,
         withSequence(Json{{"reference", mediaParam("The reference video (a media item of the project).")},
                           {"referenceCuts", schema::array(Json{{"type", "number"}, {"minimum", 0}}, "Or the reference's cuts, in seconds.")},
                           {"referenceLength", schema::number("With referenceCuts: the reference's length in seconds.", 0.1)},
                           {"strength", schema::number("1 = the reference's shot lengths (default); 0.5 = halfway between the sequence's and the reference's.", 0.0, 1.0)},
                           {"tolerance", schema::number("A shot longer than this times the target gets cuts (default 1.35).", 1.0, 5.0)},
                           {"apply", schema::enumeration({"none", "markers", "cuts"}, "What to do with the suggestions: list them (default), add markers, or cut there.")},
                           {"only", schema::array(Json{{"type", "integer"}, {"minimum", 1}}, "Apply only these suggestions (1-based).")}}),
         Json{{"referenceCuts", Json::array({2.5, 5.0, 7.0, 9.5})}, {"referenceLength", 12.0}, {"apply", "markers"}}, {}, pacingCommand});
}

} // namespace wabcut::ai::autopilot
