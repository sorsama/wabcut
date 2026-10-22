#pragma once

// Export Frame (P.2 E6): what sequence.exportFrame and source.exportFrame need from the host (EnginePolicy::services,
// found with findService): a writer that renders a sequence frame as an export does and writes it as a still image
// (the export module's stillExportServices, wabcut/export/StillExport.h), and a describer that turns the written file
// into a media item for "Import into project" (the host's probe and locator rules). Without them the commands fail
// with InvalidRequest.

#include <wabcut/command/ExecutionContext.h>
#include <wabcut/model/Error.h>
#include <wabcut/model/ProjectState.h>

#include <expected>
#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace wabcut::command {

class FrameExportServices : public CommandServices {
public:
    struct Written {
        std::filesystem::path file;
        int width = 0;
        int height = 0;
    };
    /// Renders the frame of `sequence` at `time` and writes it to `file` as `format` (one of formats()).
    using Writer = std::function<std::expected<Written, wabcut::Error>(
        const ProjectState& state, SequenceId sequence, Ticks time, const std::filesystem::path& file,
        const std::string& format)>;
    /// The written still as a media item to add to the project (probed, with its locator).
    using Describer = std::function<std::expected<MediaItem, wabcut::Error>(const std::filesystem::path& file)>;

    FrameExportServices(Writer writer, Describer describer, std::vector<std::string> formats)
        : m_writer(std::move(writer)), m_describer(std::move(describer)), m_formats(std::move(formats)) {}

    const Writer& writer() const { return m_writer; }
    /// Empty when the host cannot import (then "importIntoProject" fails).
    const Describer& describer() const { return m_describer; }
    /// The formats the writer supports ("dpx", "jpeg", "png", "tiff", "targa", "bmp", "gif", "openexr").
    const std::vector<std::string>& formats() const { return m_formats; }

private:
    Writer m_writer;
    Describer m_describer;
    std::vector<std::string> m_formats;
};

} // namespace wabcut::command
