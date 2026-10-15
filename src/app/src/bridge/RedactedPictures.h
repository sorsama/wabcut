#pragma once

// RedactedPictures: source-anchored redactions (I.6) on the pictures the app shows outside the compositor - library
// thumbnails, timeline filmstrips and smart-search hit frames (Codex wave-4 P2-9). The program and Source Monitors
// render redactions on the GPU; these small pictures are decoded straight from the file, so they get the same regions
// here on the CPU: the coverage of the CPU reference (render::maskCoverageCpu, as hardened as playback's), then the
// blur, mosaic or solid fill mixed in by it. Approximate in look (a box blur, the mosaic grid of the small picture),
// exact in where.
//
// Threading: the producers run on worker threads, so the GUI thread publishes each project snapshot (publish()) and the
// producers read the latest one. Everything here is thread-safe.
//
// Not covered: the thumbnail disk cache and the smart-search index hold unredacted pictures of the source on this
// computer (docs/masks.md).

#include <wabcut/model/EditSession.h>
#include <wabcut/model/Ids.h>
#include <wabcut/model/ProjectState.h>
#include <wabcut/model/Time.h>

#include <QImage>

#include <cstdint>
#include <string>

namespace wabcut::app::redacted {

/// Makes `project` the one producers on other threads redact against (GUI thread, on every project change).
void publish(ProjectSnapshot project);
/// The project last published (null before the first).
ProjectSnapshot published();

/// A key of the picture redactions of `media` in `project` (enabled ones: style, region, range); 0 when it has none.
/// Producers put it in their cache keys, so a changed redaction re-renders their pictures.
std::uint64_t keyOf(const ProjectState& project, MediaId media);

/// Redacts `image`, a picture of media item `media` at media time `time` in display orientation (the stream's
/// rotation applied, any size), with every enabled redaction of the media covering that moment. Returns whether it
/// changed anything.
bool apply(QImage& image, const ProjectState& project, MediaId media, Ticks time);
/// The same for every media item of the published project whose file (or proxy) is `utf8Path`.
bool applyToFile(QImage& image, const std::string& utf8Path, Ticks time);
/// keyOf() over the media items of the published project with that file.
std::uint64_t keyOfFile(const std::string& utf8Path);

} // namespace wabcut::app::redacted
