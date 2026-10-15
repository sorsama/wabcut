#pragma once

// Media-backed auto zoom (full builds only: wabcut_ai_autozoom_media links wabcut_media): the frames of a recording
// decoded forward at the analysis width (search::VideoPictureReader: hardware decoding when available, sample aspect
// and rotation applied) as grey pictures, and the cursor sidecar next to its file.

#include <wabcut/ai/autozoom/AutoZoomCommands.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace wabcut::ai::autozoom {

/// A reader of `path`'s video stream (`containerStream` -1 = the best one) as grey analysis frames.
std::expected<FrameReader, Error> openFileFrames(const std::string& path, int containerStream);

/// The content of "<file>.cursor.json", "<file>.cursor.csv", "<stem>.cursor.json" or "<stem>.cursor.csv" next to
/// `path` (at most 128 MiB), if there is one.
std::optional<std::string> findCursorSidecar(const std::string& path);

/// The services of autozoom.apply for a project file: frames through the media's locator, sidecars next to them.
std::shared_ptr<AutoZoomServices> mediaAutoZoomServices(const std::filesystem::path& projectFile, bool readFiles);

} // namespace wabcut::ai::autozoom
