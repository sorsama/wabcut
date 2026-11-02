#pragma once

// Creator autopilot (P1 #62, docs/creator-autopilot.md): three creator jobs over what Wabcut already has - the
// sequence's transcript, the library's words and (when its model is installed) the smart search's pictures, shot
// cuts, chapter and highlight markers, and still frames.
//
//   Auto B-roll     planBroll: the transcript's lines (sentences, with their most specific words), each matched
//                   against the library's video items the sequence does not use (by picture through PictureSearch,
//                   by words: the item's name, bin, Description, Comment, clip markers and transcript); the best
//                   matches within the density rules (at most perMinute, gapSeconds apart, never over the intro).
//                   placeBroll puts them, video only, on the lowest free video track above the A-roll (or a new
//                   "B-roll" track) in one edit.
//   Publishing kit  makeKit: title ideas, a description (lead, chapters, hashtags), chapters (the chapter markers or
//                   the transcript's topics), tags, and thumbnail candidates scored for sharpness, contrast, colour
//                   and exposure (frameQuality) at highlight markers, chapter starts, shot middles and telling lines.
//   Pacing match    rhythmOf: the cut rhythm of a reference (its shot cuts) and of the edit (its clip boundaries);
//                   planPacing: cuts for the edit's shots that are much longer than the reference's at the same point
//                   of the running time, each moved to the best nearby place (a sentence end, a pause, a beat; never
//                   inside a word).
//
// Pure C++ (model + ai_edit + ai_shorts + ai_search + ai_workflow): everything runs on this computer. The kit's
// optional cloud copy (kitChatRequest / parseKitReply) is text only and runs through the host's guarded chat model.

#include <wabcut/ai/Chat.h>
#include <wabcut/ai/edit/SequenceTranscript.h>
#include <wabcut/ai/search/Embedding.h>
#include <wabcut/model/Error.h>
#include <wabcut/model/ProjectState.h>

#include <array>
#include <cstddef>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::ai::autopilot {

// ---- Words ------------------------------------------------------------------------------------------------------

/// A word as matching sees it: lowercased, common English endings removed ("drones" -> "drone", "flying" -> "fly").
std::string stem(std::string_view token);
/// The words of `text` for matching: names split at "_", "-", ".", digits and camelCase ("DroneCity_night2" ->
/// drone, city, night), stop words dropped, stemmed. Words shorter than 2 characters are dropped.
std::vector<std::string> keywordTokens(std::string_view text);

/// A line of what is said: a sentence of the sequence's transcript.
struct Line {
    TimeRange range;                   ///< on the timeline
    std::string text;
    std::vector<std::string> keywords; ///< its most specific words (stemmed), best first
    double weight = 0.0;               ///< how telling the line is (the sum of its words' specificity)
};

/// The transcript as lines with their keywords (`keywords` per line at most).
std::vector<Line> transcriptLines(const edit::SequenceTranscript& transcript, std::size_t keywords = 4);

// ---- Auto B-roll ---------------------------------------------------------------------------------------------------

struct BrollOptions {
    double minSeconds = 2.0;    ///< a B-roll clip is as long as its line, within these
    double maxSeconds = 6.0;
    double perMinute = 4.0;     ///< at most this many per minute of the sequence
    double gapSeconds = 4.0;    ///< between the end of one B-roll clip and the next
    double introSeconds = 3.0;  ///< nothing over the first seconds (the hook stays on the speaker)
    double minScore = 0.3;      ///< matches below this (0..1) are dropped
    std::size_t usesPerMedia = 2;
};

/// A frame of the library that the picture search found for a text.
struct PictureHit {
    MediaId media;
    TimeRange range;   ///< source range
    Ticks thumbnail;   ///< the frame that matched (source time)
    float similarity = 0.0f;
};
/// The smart search's pictures for `query`, within `media` (best first); empty when visual search is off.
using PictureSearch = std::function<std::vector<PictureHit>(const std::string& query, std::span<const MediaId> media)>;

struct BrollMatch {
    std::size_t line = 0;   ///< index into BrollPlan::lines
    MediaId media;
    std::string mediaName;
    TimeRange source;       ///< what to show (source time); its duration is the clip's
    Ticks at;               ///< where on the timeline
    double score = 0.0;     ///< 0..1
    std::string how;        ///< "picture" | "words"
    std::string evidence;   ///< "picture" | "name" | "bin" | "description" | "marker" | "transcript"
    std::string reason;     ///< in words: "shows: city at night", "name: drone_city.mp4"
};

struct BrollPlan {
    std::vector<Line> lines;
    std::vector<BrollMatch> matches;    ///< chosen, in time order
    std::vector<std::size_t> unmatched; ///< lines that got nothing (long enough and outside the intro)
    std::size_t candidates = 0;         ///< library items looked at
    bool visual = false;                ///< the picture search ran
    std::vector<std::string> warnings;
};

/// Plans B-roll for `sequence`. `mediaTranscripts` (may be null) lets transcripts of library items match; `pictures`
/// may be empty (no visual search). `progress(done, total)` is called per line (it may throw to cancel).
BrollPlan planBroll(const ProjectState& state, SequenceId sequence, const edit::SequenceTranscript& transcript,
                    const edit::TranscriptProvider* mediaTranscripts, const PictureSearch& pictures, const BrollOptions& options,
                    const std::function<void(std::size_t, std::size_t)>& progress = {});

struct BrollTrack {
    TrackId track;
    std::size_t index = 0; ///< 0 = V1
    std::string name;
    bool created = false;
};
struct PlacedBroll {
    ProjectState state;
    BrollTrack track;
    std::vector<ClipId> clips;
};
/// Places `matches` (video only) on the lowest unlocked video track above the A-roll that is free for all of them,
/// else on a new top track named "B-roll", in one edit. Errors: NotFound (sequence, media), InvalidArgument (nothing
/// to place, overlapping placements, a source range outside its media).
std::expected<PlacedBroll, Error> placeBroll(const ProjectState& state, SequenceId sequence, std::span<const BrollMatch> matches);

/// A prompt for generating B-roll for a line nothing in the library matched (the Generate panel's).
std::string generationPrompt(const Line& line);

// ---- Publishing kit ------------------------------------------------------------------------------------------------

struct KitChapter {
    Ticks start;
    std::string title;
};

/// How good a frame looks as a thumbnail, 0..1 each.
struct FrameQuality {
    double score = 0.0;
    double sharpness = 0.0;
    double contrast = 0.0;
    double colour = 0.0;
    double exposure = 0.0;
    bool usable = true;       ///< false for a (nearly) black, blown-out or flat frame
    std::vector<std::string> reasons;
};
FrameQuality frameQuality(const search::Image& picture);

/// The picture a media item shows at a source time (small is fine: about 320 pixels wide), or nullopt.
using FramePicture = std::function<std::optional<search::Image>(const MediaItem& media, Ticks sourceTime)>;

struct ThumbnailPick {
    Ticks time;                 ///< on the timeline (frame-aligned)
    double score = 0.0;
    std::vector<std::string> reasons;
    MediaId media;              ///< what the top video track shows there (invalid: nothing)
    Ticks sourceTime;
};

struct KitOptions {
    std::size_t titleIdeas = 3;
    std::size_t thumbnails = 4;
    std::string chapters = "auto"; ///< "auto" (the markers, else topics) | "markers" | "topics" | "none"
    double thumbnailGapSeconds = 5.0;
};

struct PublishingKit {
    std::vector<std::string> titles;
    std::string lead;                  ///< the description's opening
    std::vector<KitChapter> chapters;
    std::string chaptersFrom;          ///< "markers" | "topics" | "none"
    std::vector<std::string> tags;
    std::vector<ThumbnailPick> thumbnails;
    std::vector<std::string> warnings;
    std::string description() const;  ///< lead, chapters and hashtags
};

/// The kit of `sequence`. `frames` may be empty (thumbnails then ranked by what happens there only). `progress(done,
/// total)` per thumbnail candidate.
PublishingKit makeKit(const ProjectState& state, SequenceId sequence, const edit::SequenceTranscript& transcript,
                      const FramePicture& frames, const KitOptions& options,
                      const std::function<void(std::size_t, std::size_t)>& progress = {});

/// "0:00", "4:05", "1:02:03".
std::string youtubeTime(Ticks t);
/// YouTube chapter lines: "0:00 Intro\n1:20 The setup\n...".
std::string chaptersText(std::span<const KitChapter> chapters);
/// "#Tag" words for the description (the first `count` tags that are single words or join without spaces).
std::string hashtags(std::span<const std::string> tags, std::size_t count = 3);
/// A file or folder name from any text: no path or reserved characters, no Windows device names, at most 120 bytes.
std::string safeFileName(std::string_view text);

/// The transcript as plain text for the cloud copy (at most `maxChars` bytes, whole sentences).
std::string kitTranscriptText(const edit::SequenceTranscript& transcript, std::size_t maxChars = 24000);
/// The chat request "Write with my AI" sends: the transcript text and the chapter titles, asking for JSON.
ChatRequest kitChatRequest(std::string_view transcriptText, std::span<const KitChapter> chapters, std::string_view sequenceName);
struct CloudCopy {
    std::vector<std::string> titles;
    std::string description;
    std::vector<std::string> tags;
};
/// The model's answer to kitChatRequest, or nullopt when it cannot be read.
std::optional<CloudCopy> parseKitReply(std::string_view reply);

// ---- Pacing match --------------------------------------------------------------------------------------------------

inline constexpr std::size_t kPaceParts = 8;

/// The cut rhythm of a stretch of time.
struct Rhythm {
    Ticks start;
    Ticks end;
    std::vector<Ticks> cuts;          ///< inside (start, end), sorted
    std::vector<double> shots;        ///< shot lengths in seconds, in order
    double median = 0.0;              ///< seconds
    double mean = 0.0;
    double cutsPerMinute = 0.0;
    std::array<double, kPaceParts> curve{}; ///< median shot length in each eighth of the running time
};
/// Cuts closer than `minShot` seconds to the previous one are merged (flashes, detector noise).
Rhythm rhythmOf(Ticks start, Ticks end, std::span<const Ticks> cuts, double minShot = 0.25);
/// The edit's cuts: clip boundaries on its enabled video tracks, in [0, content end).
Rhythm editRhythm(const Sequence& sequence);
/// The median shot length the rhythm has at `fraction` (0..1) of its running time.
double targetAt(const Rhythm& rhythm, double fraction);

/// A place the edit could be cut at.
struct CutCandidate {
    Ticks time;
    double strength = 0.0;  ///< 0..1
    std::string kind;       ///< "sentence" | "pause" | "beat"
};
/// Sentence ends and pauses between words (from the transcript), and beats of the sequence's beat grid.
std::vector<CutCandidate> cutCandidates(const Sequence& sequence, const edit::SequenceTranscript& transcript);

struct PacingOptions {
    double tolerance = 1.35; ///< a shot longer than this times the target gets cuts
    double strength = 1.0;   ///< 1 = the reference's shot lengths; 0.5 = halfway between yours and the reference's
    double minShot = 0.6;    ///< seconds: no cut closer than this to another
};

struct CutSuggestion {
    Ticks time;
    std::string kind;       ///< "sentence" | "pause" | "beat" | "between words" | "even"
    std::string reason;
    double targetSeconds = 0.0;
    double shotSeconds = 0.0; ///< the length of the shot it cuts
};

struct PacingPlan {
    Rhythm reference;
    Rhythm edit;
    Rhythm after;           ///< the edit with the suggested cuts
    std::vector<CutSuggestion> cuts;
    std::size_t fasterShots = 0; ///< edit shots already well shorter than the reference's (left alone)
    std::vector<std::string> warnings;
};
/// Cuts for the edit so its shots are about as long as the reference's at the same point of the running time.
/// `words` (may be null) keeps fallback cuts out of words.
PacingPlan planPacing(const Rhythm& reference, const Rhythm& edit, std::span<const CutCandidate> candidates, Rate frameRate,
                      const PacingOptions& options = {}, const edit::SequenceTranscript* words = nullptr);

} // namespace wabcut::ai::autopilot
