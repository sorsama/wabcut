#pragma once

// What P.2's editing commands (CatalogEditing.cpp) need from the host besides the project.

#include <wabcut/command/ExecutionContext.h>
#include <wabcut/model/Error.h>
#include <wabcut/model/ProjectState.h>

#include <expected>
#include <filesystem>
#include <functional>
#include <utility>

namespace wabcut::command {

/// Reading a media file into a MediaItem (media.replaceFootage): the host's probe with the project's locator rules
/// (the app and wabcut-cli wrap editor::describeMediaFile). Without it, media.replaceFootage is refused.
class MediaProbeServices : public CommandServices {
public:
    using Describe = std::function<std::expected<MediaItem, wabcut::Error>(const std::filesystem::path& file)>;
    explicit MediaProbeServices(Describe describe) : m_describe(std::move(describe)) {}
    std::expected<MediaItem, wabcut::Error> describe(const std::filesystem::path& file) const { return m_describe(file); }

private:
    Describe m_describe;
};

} // namespace wabcut::command
