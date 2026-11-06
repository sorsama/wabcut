// audioDescription.findGaps and audioDescription.createTrack (0.7, card 175; AudioDescription.h): the gaps in a sequence's
// dialogue where a description fits, and the caption track "Audio Description" that holds the lines to be read, edited,
// exported as a narrator's script or voiced (dub.create). Neither calls a model: the app's Audio Description dialog (or an
// assistant that can see pictures) writes the lines.

#include <wabcut/ai/dub/AudioDescription.h>
#include <wabcut/ai/dub/DubCommands.h>
#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/command/Schema.h>
#include <wabcut/model/CaptionOps.h>
#include <wabcut/model/FinishingOps.h>

#include <algorithm>
#include <cmath>

namespace wabcut::ai::dub {

using command::CommandOutput;
using command::ErrorCode;
using command::ExecutionContext;
using command::fail;
using command::Json;
namespace schema = command::schema;

namespace {

constexpr const char* kTrackName = "Audio Description";
constexpr std::size_t kMaxLines = 2000;
constexpr std::size_t kMaxLineBytes = 1000;

Json timeJson(ExecutionContext& c, Ticks t) { return Json{{"ticks", t.value}, {"timecode", c.timecode(t)}}; }

AdOptions optionsOf(ExecutionContext& c) {
    AdOptions options;
    if (c.has("minGap")) {
        options.minGap = Ticks{static_cast<std::int64_t>(std::llround(c.number("minGap") * static_cast<double>(kTicksPerSecond)))};
    }
    if (c.has("wordsPerSecond")) {
        options.wordsPerSecond = c.number("wordsPerSecond");
    }
    if (c.has("maxWords")) {
        options.maxWords = static_cast<int>(c.integer("maxWords"));
    }
    return options;
}

CommandOutput findGapsCommand(ExecutionContext& c) {
    const Sequence& sequence = c.sequence();
    auto* services = command::findService<ai::edit::TranscriptServices>(c.services());
    const bool silent = c.boolean("silent", false);
    const AdOptions options = optionsOf(c);
    std::vector<AdGap> gaps;
    std::size_t words = 0;
    double speechSeconds = 0.0;
    if (silent) {
        gaps = findGaps(std::span<const TimeRange>{}, sequence.duration, options, nullptr);
    } else {
        if (!services || !services->transcripts) {
            fail(ErrorCode::InvalidRequest, "this host holds no transcripts: pass \"silent\": true for a sequence without speech");
        }
        const ai::edit::SequenceTranscript transcript = ai::edit::SequenceTranscript::build(c.state(), sequence.id, *services->transcripts);
        if (transcript.empty()) {
            fail(ErrorCode::NotFound,
                 "the sequence has no transcript: transcribe it first, or pass \"silent\": true when it has no speech");
        }
        words = transcript.size();
        for (const TimeRange& range : speechRanges(transcript, options)) {
            speechSeconds += range.duration.toSeconds();
        }
        gaps = findGaps(transcript, sequence.duration, options);
    }
    Json list = Json::array();
    for (const AdGap& gap : gaps) {
        list.push_back(Json{{"start", timeJson(c, gap.range.start)},
                            {"end", timeJson(c, gap.range.end())},
                            {"seconds", std::round(gap.range.duration.toSeconds() * 100.0) / 100.0},
                            {"maxWords", gap.maxWords},
                            {"before", gap.before},
                            {"after", gap.after}});
    }
    CommandOutput out;
    out.state = c.state();
    out.data = Json{{"gaps", std::move(list)},
                    {"count", gaps.size()},
                    {"words", words},
                    {"speechSeconds", std::round(speechSeconds * 10.0) / 10.0},
                    {"sequenceSeconds", std::round(sequence.duration.toSeconds() * 10.0) / 10.0}};
    return out;
}

CommandOutput createTrackCommand(ExecutionContext& c) {
    const Sequence& sequence = c.sequence();
    const Json& list = c.params().at("lines");
    if (!list.is_array() || list.empty()) {
        fail(ErrorCode::InvalidParams, "\"lines\" needs at least one line", Json{{"path", "/lines"}});
    }
    if (list.size() > kMaxLines) {
        fail(ErrorCode::InvalidParams, "more than " + std::to_string(kMaxLines) + " lines", Json{{"path", "/lines"}});
    }
    struct Entry {
        Ticks start;
        Ticks end;
        std::string text;
    };
    std::vector<Entry> entries;
    for (std::size_t i = 0; i < list.size(); ++i) {
        const Json& line = list[i];
        const std::string path = "/lines/" + std::to_string(i);
        if (!line.is_object() || !line.contains("text") || !line["text"].is_string() || !line.contains("start")) {
            fail(ErrorCode::InvalidParams, "a line needs \"start\", \"end\" (or \"duration\") and \"text\"", Json{{"path", path}});
        }
        Entry entry;
        entry.start = c.timeValue(line["start"], path + "/start");
        if (line.contains("end")) {
            entry.end = c.timeValue(line["end"], path + "/end");
        } else if (line.contains("duration")) {
            entry.end = entry.start + c.timeValue(line["duration"], path + "/duration");
        } else {
            fail(ErrorCode::InvalidParams, "a line needs \"end\" or \"duration\"", Json{{"path", path}});
        }
        entry.text = line["text"].get<std::string>();
        // Folded to one line: a description is spoken as one phrase.
        std::string folded;
        for (const char ch : entry.text) {
            folded += (ch == '\n' || ch == '\r' || ch == '\t') ? ' ' : ch;
        }
        entry.text = fitDescription(folded, 1000);
        if (entry.text.empty()) {
            continue; // a line with nothing to say (or NONE) is a gap left silent
        }
        if (entry.text.size() > kMaxLineBytes) {
            fail(ErrorCode::InvalidParams, "a line is longer than " + std::to_string(kMaxLineBytes) + " bytes", Json{{"path", path + "/text"}});
        }
        if (entry.start.value < 0 || entry.end <= entry.start) {
            fail(ErrorCode::InvalidParams, "a line must start at 0 or later and end after it", Json{{"path", path}});
        }
        entries.push_back(std::move(entry));
    }
    if (entries.empty()) {
        fail(ErrorCode::InvalidParams, "no line has any text", Json{{"path", "/lines"}});
    }
    std::ranges::sort(entries, {}, &Entry::start);
    for (std::size_t i = 1; i < entries.size(); ++i) {
        if (entries[i].start < entries[i - 1].end) {
            fail(ErrorCode::InvalidParams, "two lines overlap: a description is spoken one line at a time",
                 Json{{"path", "/lines/" + std::to_string(i)}});
        }
    }
    const std::string name = c.string("name", kTrackName);
    const std::string language = c.string("language", "");
    ProjectState state = c.state();
    // Making the lines again replaces the earlier track of that name (the person's edits in it with it): "replace" false adds.
    if (c.boolean("replace", true)) {
        for (const CaptionTrack& t : sequence.captionTracks) {
            if (t.name == name) {
                state = c.take(wabcut::edit::removeCaptionTrack(state, sequence.id, t.id)).state;
                break;
            }
        }
    }
    wabcut::edit::EditResult added = c.take(wabcut::edit::addCaptionTrack(state, sequence.id, name, language));
    state = std::move(added.state);
    const CaptionTrackId track = added.createdCaptionTrack;
    for (const Entry& entry : entries) {
        state = c.take(wabcut::edit::addCaptionCue(state, sequence.id, track, CaptionCue{entry.start, entry.end - entry.start, entry.text, {}})).state;
    }
    // Off: the lines are a script to read and voice, not subtitles to burn into the picture.
    wabcut::edit::CaptionTrackChange change;
    change.enabled = c.boolean("enabled", false);
    state = c.take(wabcut::edit::setCaptionTrackInfo(state, sequence.id, track, change)).state;
    CommandOutput out;
    out.state = std::move(state);
    out.createdIds.push_back(track.value);
    out.data = Json{{"captionTrack", track.value}, {"name", name}, {"lines", entries.size()}};
    return out;
}

} // namespace

void registerAudioDescriptionCommands(command::CommandRegistry& registry) {
    Json findProps = Json::object();
    findProps["sequence"] = schema::integer("Sequence id; only needed when the project has several.", 1);
    findProps["minGap"] = schema::number("The shortest silence worth a line, seconds (default 2), after 0.3 s are kept clear of the speech on each side.", 0.5, 60.0);
    findProps["wordsPerSecond"] = schema::number("How fast the lines are read, words per second (default 2.5, about 150 a minute).", 0.5, 6.0);
    findProps["maxWords"] = schema::integer("Most words in a line whatever the silence (default 40).", 1, 200);
    findProps["silent"] = schema::boolean("The sequence has no speech: the whole sequence is silence (default false).", false);
    Json findParams{{"type", "object"}, {"properties", std::move(findProps)}, {"additionalProperties", false}};
    [[maybe_unused]] const auto find = registry.add(
        {"audioDescription.findGaps", 1, "Find Gaps for Audio Description",
         "The gaps in a sequence's dialogue (from its transcript) long enough for an audio description line: where each is on "
         "the timeline, how many words fit at a speaking pace, and the words said just before and after. The first step of "
         "Sequence > Generate Audio Description; changes nothing. Needs a transcript (or \"silent\": true).",
         command::Capability::Read, std::move(findParams), Json{{"minGap", 2.5}}, {}, findGapsCommand});

    Json lineSchema = schema::object(
        {{"start", Json{{"type", Json::array({"integer", "string"})}, {"description", "Where the line starts: a timecode or ticks."}}},
         {"end", Json{{"type", Json::array({"integer", "string"})}, {"description", "Where it ends (or give \"duration\")."}}},
         {"duration", Json{{"type", Json::array({"integer", "string"})}, {"description", "How long the slot is (instead of \"end\")."}}},
         {"text", Json{{"type", "string"}, {"minLength", 1}, {"maxLength", 1000}, {"description", "What is said, one phrase."}}}},
        {"start", "text"});
    Json createProps = Json::object();
    createProps["sequence"] = schema::integer("Sequence id; only needed when the project has several.", 1);
    createProps["lines"] = schema::array(std::move(lineSchema), "The lines, each in its gap; they may not overlap. A line of \"NONE\" or no words is left out.", 1);
    createProps["name"] = Json{{"type", "string"}, {"minLength", 1}, {"maxLength", 128}, {"description", "The track's name (default \"Audio Description\")."}};
    createProps["language"] = Json{{"type", "string"}, {"minLength", 2}, {"maxLength", 16}, {"description", "The lines' language (ISO 639-1)."}};
    createProps["replace"] = schema::boolean("Replace the earlier track of that name (default true); false adds another.", true);
    createProps["enabled"] = schema::boolean("Show the lines as subtitles (default false: they are a script to read and voice, not burned in).", false);
    Json createParams{{"type", "object"}, {"properties", std::move(createProps)}, {"additionalProperties", false}, {"required", Json::array({"lines"})}};
    [[maybe_unused]] const auto create = registry.add(
        {"audioDescription.createTrack", 1, "Audio Description Track",
         "Puts audio description lines on a caption track \"Audio Description\" (switched off, so they are not burned into the "
         "picture): one cue per line at its gap, editable in the Captions tab, exported as a narrator's script with "
         "captions.export, or voiced with dub.create (name \"Audio Description\", duck about -12 dB so the dialogue and music "
         "go down under each line). One undo step.",
         command::Capability::EditSequence, std::move(createParams),
         Json{{"lines", Json::array({Json{{"start", "00:00:03:00"}, {"end", "00:00:06:00"}, {"text", "A woman opens a red door."}}})}}, {},
         createTrackCommand});
}

} // namespace wabcut::ai::dub
