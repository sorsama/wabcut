#pragma once

// 0.2 media formats for the hosts: the readers behind media.layeredFileInfo / importLayeredFile and media.browse /
// importCameraClips (command/MediaFormatServices.h), over media/Psd.h and CameraCards.h.

#include <wabcut/command/MediaFormatServices.h>
#include <wabcut/model/Error.h>
#include <wabcut/model/LayeredFile.h>
#include <wabcut/model/ProjectState.h>

#include <expected>
#include <filesystem>
#include <memory>
#include <string>

namespace wabcut::editor {

/// A media item (not yet in a project) showing `spec`'s layers of a Photoshop file (model/LayeredFile.h), named `name`.
std::expected<MediaItem, Error> describeLayeredFile(const LayeredFileSpec& spec, std::string name,
                                                    const std::filesystem::path& projectFile);

/// The services for a project saved at `projectFile` (empty: not saved yet; paths stay absolute).
std::shared_ptr<command::MediaFormatServices> mediaFormatServices(const std::filesystem::path& projectFile);

} // namespace wabcut::editor
