#pragma once

// The host's media reader for media.importImageSequence (CatalogLibrary.cpp; 0.2, Premiere's Import with "Image
// Sequence" ticked). wabcut_command reads no media itself: the app and wabcut-cli (and its MCP server) provide this
// over editor::describeImageSequence through EnginePolicy::services. Without it the command refuses to run.

#include <wabcut/command/ExecutionContext.h>
#include <wabcut/model/Error.h>
#include <wabcut/model/ProjectState.h>
#include <wabcut/model/Time.h>

#include <expected>
#include <filesystem>
#include <functional>
#include <utility>

namespace wabcut::command {

class ImageSequenceServices : public CommandServices {
public:
    /// A media item (not yet in a project) for the numbered stills from `firstFrame` on, at `frameRate`.
    using Describe = std::function<std::expected<MediaItem, Error>(const std::filesystem::path& firstFrame, Rate frameRate)>;

    explicit ImageSequenceServices(Describe describe) : m_describe(std::move(describe)) {}

    std::expected<MediaItem, Error> describe(const std::filesystem::path& firstFrame, Rate frameRate) const {
        return m_describe ? m_describe(firstFrame, frameRate)
                          : std::unexpected(Error{wabcut::ErrorCode::Unsupported, "this host cannot read image sequences"});
    }

private:
    Describe m_describe;
};

/// A frame rate in frames per second as an exact rate: the NTSC rates (23.976, 29.97, 47.952, 59.94, 119.88) as
/// n*1000/1001, whole numbers as n/1, anything else to a thousandth of a frame.
Rate imageSequenceRate(double fps);

} // namespace wabcut::command
