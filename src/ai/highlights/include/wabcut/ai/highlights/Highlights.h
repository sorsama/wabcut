#pragma once

// G.2 gameplay highlights (docs/ai.md "Gameplay highlights"): the best moments of a gaming recording, found on this
// computer from what it sounds and looks like, with the reasons in words. No model and no cloud: every signal is a
// measurement (Features.h) compared with what is usual for the same recording.
//
//   game audio   loudness spikes over the game's moving baseline (fights, explosions, kills)
//   mic          the player's voice: louder or higher than they usually talk, shouting, laughter
//   visual       motion above the usual, brightness flashes, bursts of picture changes
//   chat         an imported Twitch / YouTube chat log: message-rate spikes and emote / keyword hits
//   hints        OBS chapter markers in the recording and marker files next to it (a hotkey pressed after a play)
//
// MediaSignals holds one recording's signals in media time; mapToSequence() lays them on a sequence's timeline
// through its clips; findMoments() fuses them into ranked moments (range, peak, score, confidence, reasons).

#include <wabcut/ai/highlights/Features.h>
#include <wabcut/model/Error.h>
#include <wabcut/model/ProjectState.h>

#include <expected>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::ai::highlights {

/// Bumped when the signals or the scoring change (cache keys, the generator recorded with derived sequences).
inline constexpr int kVersion = 1;
inline constexpr std::string_view kGenerator = "highlights/1";

// --- Inputs that are not media ---------------------------------------------------------------------------------------

/// A moment someone marked: an OBS chapter marker, a line of a marker file next to the recording.
struct Hint {
    Ticks at;           ///< media time (timeline time once mapped)
    std::string label;  ///< "" when it has none
    std::string source; ///< "chapter" or "marker file"
    bool operator==(const Hint&) const = default;
};

/// Marker files: "<recording>.markers.txt|csv|json" or "<stem>_markers.*" (docs/ai.md): lines "1:02:03.5 label",
/// "62:03 label" or "3723.5 label" (also comma / tab separated, with an optional "time,label" header), or JSON
/// [{"time": 3723.5, "label": "..."}] (also {"markers": [...]}, "seconds"/"offset" and "name"/"text").
/// Errors: InvalidArgument when nothing in it reads as a marker.
std::expected<std::vector<Hint>, Error> parseMarkerFile(std::string_view text);

struct ChatMessage {
    double seconds = 0.0;             ///< from the start of the stream (the VOD)
    std::string text;                 ///< the message, emotes as their names
    std::vector<std::string> emotes;  ///< emote names the log marks as emotes
    bool operator==(const ChatMessage&) const = default;
};

struct ChatLog {
    std::string format; ///< "twitch", "youtube", "chat-downloader" or "generic"
    std::vector<ChatMessage> messages; ///< sorted by time
};

inline constexpr std::size_t kMaxChatMessages = 2'000'000;

/// Reads a chat log: TwitchDownloader / Twitch VOD JSON ({"comments": [{"content_offset_seconds", "message":
/// {"body", "fragments"}}]}), yt-dlp's YouTube live_chat.json (one JSON object per line, "videoOffsetTimeMsec"),
/// chat-downloader JSON ([{"time_in_seconds", "message", "emotes"}]) or a generic [{"time", "text"}] list.
/// Errors: InvalidArgument (not a chat log it knows, more than kMaxChatMessages).
std::expected<ChatLog, Error> parseChatLog(std::string_view text);

/// A kept chat log: {"schema":"wabcut.highlight-chat","version":1,"format","offset","messages":[[seconds, text,
/// [emotes]], ...]} (texts cut to 500 bytes).
std::string toJson(const ChatLog& chat, double offsetSeconds);
std::expected<std::pair<ChatLog, double>, Error> chatFromJson(std::string_view json);

/// Emotes and words that mark a reaction in chat (lower case; matched as whole words, case-insensitively).
const std::vector<std::string>& defaultChatKeywords();

// --- Signals -------------------------------------------------------------------------------------------------------

/// What an audio stream carries.
enum class AudioRole { Game, Mic, Mixed };
std::string_view toString(AudioRole role);
std::optional<AudioRole> audioRoleFromString(std::string_view text);

/// The role an audio stream most likely has, from how much of its sound is voiced: with several streams (OBS
/// multi-track), the most voice-like one (when it is clearly speech) is the mic and the others are game; a single
/// stream is Mixed.
std::vector<AudioRole> classifyStreams(const std::vector<const AudioFeatures*>& streams);
/// Share of the active (above -50 dBFS) 100 ms samples that are voiced, 0..1.
double voicedShare(const AudioFeatures& features);

struct KeywordHit {
    std::size_t index = 0; ///< sample
    std::string word;
    bool operator==(const KeywordHit&) const = default;
};

/// One recording's signals (or, after mapToSequence(), a sequence's). Sample i covers [i, i + 1) x 100 ms. Each
/// activation is 0..1 (how strongly it says "something happened"), empty when the signal is absent.
struct MediaSignals {
    std::size_t length = 0;
    std::vector<float> game;
    std::vector<float> mic;
    std::vector<float> visual;
    std::vector<float> chat;
    // What the reasons quote.
    std::vector<float> gameRiseDb;  ///< game audio above its moving baseline, dB
    std::vector<float> voiceRiseDb; ///< the voice above the speaker's usual level, dB
    std::vector<float> pitchRise;   ///< semitones above the speaker's usual pitch (voiced samples)
    std::vector<float> laugh;       ///< 0..1
    std::vector<float> motionRatio; ///< motion / the usual motion
    std::vector<float> flash;       ///< 0..1
    std::vector<float> cuts;        ///< picture changes in the 5 s around
    std::vector<float> chatRatio;   ///< chat messages per second / the usual rate
    std::vector<float> chatCount;   ///< chat messages in the 5 s around
    std::vector<KeywordHit> keywords; ///< sorted by index
    std::vector<Hint> hints;          ///< sorted by time

    bool hasAny() const { return !game.empty() || !mic.empty() || !visual.empty() || !chat.empty() || !hints.empty(); }
};

/// Adds an audio stream's signals (`role` Mixed: both game and mic from one stream). Several streams of one role
/// combine by the louder activation.
void addAudio(MediaSignals& into, const AudioFeatures& features, AudioRole role);
void addVideo(MediaSignals& into, const VisualFeatures& features);

struct ChatOptions {
    /// Chat time + offset = media time (the recording may start before or after the stream's VOD).
    double offsetSeconds = 0.0;
    /// Viewers react after the play (and see the stream late): chat activity counts this much earlier.
    double reactionSeconds = 6.0;
    std::vector<std::string> keywords = defaultChatKeywords();
};
void addChat(MediaSignals& into, const ChatLog& chat, const ChatOptions& options);
void addHints(MediaSignals& into, std::vector<Hint> hints);

/// How a media item's signals are laid on a sequence: the media's signals by MediaId (keyed .value).
using SignalsByMedia = std::map<std::uint64_t, const MediaSignals*>;

/// The sequence's signals: the visual signal from the topmost enabled video clip showing analysed media; game, mic
/// and chat signals and hints wherever an enabled clip (video or audio) plays analysed media (the strongest when
/// several do), at the clip's source time (speed, reverse and time remapping applied). A nested or multicam clip reads
/// what it shows or plays (resolveLeaves: the active angle's media, the nested sequence's clips, through every level's
/// time map). Generated clips have no signals; media without analysis gets a warning. The result covers [0, sequence
/// duration).
MediaSignals mapToSequence(const ProjectState& state, SequenceId sequence, const SignalsByMedia& media,
                           std::vector<std::string>* warnings = nullptr);

// --- Moments -------------------------------------------------------------------------------------------------------

struct FindOptions {
    std::size_t maxMoments = 20;
    double minScore = 0.25;     ///< fused activation a peak needs
    double minGapSeconds = 20.0; ///< between two moments' peaks
    double maxLeadSeconds = 12.0; ///< build-up kept before a peak (at most)
    double minLeadSeconds = 4.0;
    double maxTailSeconds = 6.0;
    double minTailSeconds = 2.0;
    double weightGame = 1.0;
    double weightMic = 1.2;
    double weightVisual = 0.8;
    double weightChat = 1.0;
};

struct Moment {
    TimeRange range;      ///< media (or timeline) time
    Ticks peak;
    double score = 0.0;   ///< 0..1
    double confidence = 0.0; ///< 0..1: how many independent signals agree, and how clearly
    std::vector<std::string> reasons; ///< strongest first
    std::map<std::string, double> levels; ///< activation of each signal at the peak ("game", "mic", ...)
    std::string title;    ///< a short label from the strongest reason
};

/// Ranked moments (best first). `duration`: the end of the material (moments stay inside it).
std::vector<Moment> findMoments(const MediaSignals& measured, Ticks duration, const FindOptions& options = {});

/// "high" / "medium" / "low".
std::string_view confidenceLabel(double confidence);

// --- Measuring against scripted events (tests, docs/ai.md accuracy) --------------------------------------------------

struct Accuracy {
    std::size_t truePositives = 0;
    std::size_t falsePositives = 0;
    std::size_t falseNegatives = 0;
    double precision = 0.0;
    double recall = 0.0;
};
/// A moment matches an event when the event lies in the moment's range or within `toleranceSeconds` of its peak;
/// each event matches at most one moment.
Accuracy measure(const std::vector<Moment>& moments, const std::vector<Ticks>& events, double toleranceSeconds = 3.0);

// --- The cache's JSON -------------------------------------------------------------------------------------------------

/// The measurements are what the AnalysisCache keeps (decoding is the slow part; signals and moments are quick to
/// make again with other settings): {"schema":"wabcut.highlight-audio"|"wabcut.highlight-video","version":1,...},
/// values rounded to 2-3 decimals.
std::string toJson(const AudioFeatures& features);
std::string toJson(const VisualFeatures& features);
std::expected<AudioFeatures, Error> audioFeaturesFromJson(std::string_view json);
std::expected<VisualFeatures, Error> visualFeaturesFromJson(std::string_view json);
/// {"schema":"wabcut.highlights","version":1,"moments":[{start,end,peak (seconds),score,confidence,reasons,...}]}.
std::string toJson(const std::vector<Moment>& moments);

} // namespace wabcut::ai::highlights
