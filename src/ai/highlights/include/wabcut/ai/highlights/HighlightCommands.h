#pragma once

// The gameplay-highlight commands of the catalog (G.2; docs/commands.md, docs/ai.md "Gameplay highlights"):
//
//   highlights.find     EditSequence  the best moments of a sequence (or a media item) from game audio, the player's
//                                     voice, the picture, an imported chat log and OBS / marker-file markers: ranked,
//                                     with a score, a confidence and the reasons, as markers (one undo step)
//   highlights.importChat MediaAnalysis  a Twitch / YouTube chat log for a recording, kept with its analyses (the one
//                                     command that reads a file the caller names: the assistant never runs it itself)
//   highlights.toReel   EditProject   a new highlight-reel sequence from moments: cuts with handles, in score or time
//                                     order, optionally cut to the beats of a music track, with transitions and a
//                                     slow-motion peak (one undo step)
//   highlights.toShort  EditProject   one moment as a vertical (9:16) sequence (shorts.create's reframing, captions
//                                     and hook title; a gaming layout once layout.apply exists)
//
// Measuring needs the host's HighlightServices (wabcut_ai_highlights_media decodes and caches; tests hand in
// features). Everything runs on this computer: nothing is uploaded.

#include <wabcut/ai/analysis/AnalysisCommands.h>
#include <wabcut/ai/highlights/Highlights.h>
#include <wabcut/command/ExecutionContext.h>
#include <wabcut/command/Registry.h>

#include <expected>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace wabcut::ai::highlights {

/// The markers highlights.find adds: named "Highlight <rank>: <title>" with the comment "score 0.82, confidence high:
/// <reasons>" for people, and C.15's Marker::highlight (score, confidence, reasons, peak, source "highlights") for the
/// Highlights panel and the reel. highlights.find replaces its earlier ones (its source, or the name and comment form
/// of projects from before C.15); highlights.toReel and highlights.toShort read every marker with highlight metadata.
inline constexpr std::string_view kMarkerPrefix = "Highlight ";
inline constexpr std::string_view kCommentPrefix = "score ";
inline constexpr std::int32_t kMarkerColor = 3; ///< orange
/// MarkerHighlight::source of the markers highlights.find writes (C.15's Marker::highlight).
inline constexpr std::string_view kHighlightSource = "highlights";
inline constexpr std::size_t kMaxReelMoments = 50;

/// What a recording sounds and looks like (features per audio stream, by MediaInfo::audio index; the best video
/// stream's) and the markers found in or next to it.
struct Measurement {
    std::vector<AudioFeatures> audio;
    std::optional<VisualFeatures> video;
    std::vector<Hint> hints;
    std::vector<std::string> notes; ///< e.g. "3 OBS chapter markers"
    bool fromCache = false;
};

class HighlightProvider {
public:
    virtual ~HighlightProvider() = default;
    /// Measures `media` (all its audio streams; the video too when `video`). Errors: Unsupported (generated media),
    /// NotFound (offline), the decoder's, Cancelled.
    virtual std::expected<Measurement, Error> measure(const MediaItem& media, bool video, const analysis::AnalysisProgress& progress) = 0;
    /// Keeps the moments found in `media` (media time) where analyses are kept (the AnalysisCache); optional.
    virtual void remember(const MediaItem& media, const std::vector<Moment>& moments) {
        (void)media;
        (void)moments;
    }
    /// Keeps an imported chat log with `media`'s analyses, replacing an earlier one (`offsetSeconds`: chat time +
    /// offset = media time). Errors: Unsupported (this provider keeps none), the cache's.
    virtual std::expected<void, Error> keepChat(const MediaItem& media, const ChatLog& chat, double offsetSeconds) {
        (void)media;
        (void)chat;
        (void)offsetSeconds;
        return std::unexpected(Error(wabcut::ErrorCode::Unsupported, "chat logs cannot be kept here"));
    }
    struct KeptChat {
        ChatLog chat;
        double offsetSeconds = 0.0;
    };
    /// The chat log kept for `media`, if any.
    virtual std::optional<KeptChat> chatFor(const MediaItem& media) {
        (void)media;
        return std::nullopt;
    }
};

/// Measurements handed over in memory (tests, an app that already has them), by media id.
class MapHighlightProvider : public HighlightProvider {
public:
    void add(MediaId media, Measurement m) { m_items[media.value] = std::move(m); }
    std::expected<Measurement, Error> measure(const MediaItem& media, bool video, const analysis::AnalysisProgress& progress) override;
    void remember(const MediaItem& media, const std::vector<Moment>& moments) override { remembered[media.id.value] = moments; }
    std::expected<void, Error> keepChat(const MediaItem& media, const ChatLog& chat, double offsetSeconds) override {
        chats[media.id.value] = KeptChat{chat, offsetSeconds};
        return {};
    }
    std::optional<KeptChat> chatFor(const MediaItem& media) override {
        const auto it = chats.find(media.id.value);
        return it == chats.end() ? std::nullopt : std::optional<KeptChat>(it->second);
    }
    std::map<std::uint64_t, std::vector<Moment>> remembered;
    std::map<std::uint64_t, KeptChat> chats;

private:
    std::map<std::uint64_t, Measurement> m_items;
};

class HighlightServices : public command::CommandServices {
public:
    explicit HighlightServices(std::shared_ptr<HighlightProvider> provider) : provider(std::move(provider)) {}
    std::shared_ptr<HighlightProvider> provider;
};

/// Adds highlights.find, highlights.importChat, highlights.toReel and highlights.toShort.
void registerHighlightCommands(command::CommandRegistry& registry);

/// A moment as the commands report it (`rank` from 1).
command::Json momentToJson(const Moment& moment, std::size_t rank);

/// The moments highlights.find marked on a sequence (its markers in the kMarkerPrefix / kCommentPrefix form), best
/// first.
std::vector<Moment> markedMoments(const Sequence& sequence);

} // namespace wabcut::ai::highlights
