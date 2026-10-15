#pragma once

// autozoom.apply (AI-11, docs/commands.md, docs/ai.md "Creator superpowers"), EditSequence: zooms a screen-recording
// clip in where things happen and out when nothing does, as editable Position / Scale keyframes; one undo step, dryRun
// previews it (with "preview": the window over time, for drawing over the monitor); running it again replaces its
// keys with the same result.
//
// Activity comes from, in order: "cursor" (a sidecar's content passed in the request), "sidecar" (a file path, where
// the host allows reading files), the media's own sidecar next to its file (AutoZoomServices::sidecar), or local
// detection on the frames (AutoZoomServices::frames). Without services, only "cursor" works.

#include <wabcut/ai/autozoom/AutoZoom.h>
#include <wabcut/command/ExecutionContext.h>
#include <wabcut/command/Registry.h>

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>

namespace wabcut::ai::autozoom {

class AutoZoomServices : public command::CommandServices {
public:
    /// A reader of a media item's video stream frames (grey, at a requested width).
    using FrameOpener = std::function<std::expected<FrameReader, Error>(const MediaItem& media, int videoStream)>;
    /// The content of the cursor sidecar recorded next to a media file, if there is one.
    using SidecarFinder = std::function<std::optional<std::string>(const MediaItem& media)>;

    AutoZoomServices(FrameOpener frames, SidecarFinder sidecar = {}, bool readFiles = false)
        : frames(std::move(frames)), sidecar(std::move(sidecar)), readFiles(readFiles) {}

    FrameOpener frames;
    SidecarFinder sidecar;
    /// "sidecar" may name a file (local callers: the CLI's cmd, the app). MCP clients and the assistant never.
    bool readFiles = false;

    /// Detected activity of (media, stream, range, settings), kept for this host so a preview and its apply analyse
    /// once.
    std::optional<ActivityTrack> cached(const std::string& key) const;
    void remember(const std::string& key, ActivityTrack track);

private:
    mutable std::mutex m_mutex;
    std::map<std::string, ActivityTrack> m_cache;
};

/// Adds autozoom.apply.
void registerAutoZoomCommands(command::CommandRegistry& registry);

/// The zoom as the command reports it: segments (timeline seconds), counts, keyframes and, with `preview`, the window
/// over time in picture fractions ({t, zoom, x, y, w, h} at `previewFps`).
command::Json zoomResultJson(const Clip& clip, const ZoomResult& result, const ActivityTrack& activity, bool preview, double previewFps = 10.0);

} // namespace wabcut::ai::autozoom
