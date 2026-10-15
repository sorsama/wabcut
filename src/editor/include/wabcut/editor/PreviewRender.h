#pragma once

// Preview files (C.16): the PreviewRenderer of the preview render cache (model/PreviewCache.h, C.15) over the export
// path. A segment is rendered like an export of that range - the same offscreen compositor, the sequence's own frame
// size and rate, every frame of [start, end) - into an intermediate file in the project's preview folder: ProRes 422 LT
// (10-bit 4:2:2, .mov container) by default, or DNxHR LB (8-bit 4:2:2). No audio (previews are pictures; the timeline
// audio always plays live) and no burned-in captions (they are drawn live on top, and the content hash leaves them out).
//
// Playback switches to a rendered file inside a valid segment (PreviewCache::validAt; the playback scheduler does it
// when PreviewSettings::previews is set). The host keeps the cache in step with edits: PreviewCache::invalidate with
// every ChangeSet (an EditSession listener), load() when a project opens, save() after renders and on close.

#include <wabcut/export/ExportJob.h>
#include <wabcut/model/PreviewCache.h>

#include <cstdint>
#include <filesystem>
#include <memory>

namespace wabcut::editor {

enum class PreviewCodec : std::uint8_t {
    ProResLt, ///< ProRes 422 LT, 10-bit 4:2:2 (prores_ks): Premiere's default preview format on Windows too
    DnxhrLb,  ///< DNxHR LB, 8-bit 4:2:2: smaller, faster to encode
};

struct PreviewRenderOptions {
    PreviewCodec codec = PreviewCodec::ProResLt;
    /// The frames to encode; empty = exporter::defaultVideoSourceFactory() (the offscreen compositor: the process needs a
    /// QGuiApplication).
    exporter::VideoSourceFactory videoSource;
    /// The project file, to resolve media paths relative to it.
    std::filesystem::path projectFile;
};

/// The export settings a preview of [range) renders with: a custom range, the sequence's size and rate (unset), the
/// codec in a .mov container, no audio, no captions.
exporter::ExportSettings previewExportSettings(PreviewCodec codec, TimeRange range);

/// The PreviewRenderer over exporter::ExportJob. render() runs on the calling thread (a job worker, never the GUI thread);
/// it writes `file` atomically (a cancelled or failed render leaves none) and returns its size.
std::shared_ptr<PreviewRenderer> makeExportPreviewRenderer(PreviewRenderOptions options = {});

/// Where a project keeps its preview files: "<project stem> Preview Files" next to the project file (Premiere's
/// "Adobe Premiere Pro Video Previews" folder is per project too).
std::filesystem::path previewFolderFor(const std::filesystem::path& projectFile);

} // namespace wabcut::editor
