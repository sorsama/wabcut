#pragma once

// The host's readers for the 0.2 media formats (CatalogLibrary.cpp): layered Photoshop files (media.layeredFileInfo,
// media.importLayeredFile; model/LayeredFile.h) and camera clips from the Media Browser (media.browse,
// media.importCameraClips; editor/CameraCards.h). wabcut_command reads no media itself: the app and wabcut-cli (and its
// MCP server) provide these over the editor (editor::mediaFormatServices). Without them the commands refuse to run.

#include <wabcut/command/ExecutionContext.h>
#include <wabcut/model/Error.h>
#include <wabcut/model/LayeredFile.h>
#include <wabcut/model/ProjectState.h>

#include <expected>
#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace wabcut::command {

class MediaFormatServices : public CommandServices {
public:
    /// A layer of a Photoshop file as Import Layered File lists it.
    struct Layer {
        int index = 0;
        std::string name;
        bool visible = true;
        bool hasPixels = true;
        bool group = false;
        int left = 0, top = 0, right = 0, bottom = 0;
        std::string blendMode;
        std::vector<std::string> groups;
    };
    struct LayeredDocument {
        int width = 0;
        int height = 0;
        int depth = 8;                ///< bits per channel: 8, 16 or 32 (float)
        std::string colorMode = "rgb"; ///< "rgb", "cmyk" or "grayscale" (CMYK is converted to RGB)
        std::vector<Layer> layers;    ///< bottom to top
    };
    /// A Media Browser entry: a folder, or a clip (its files; a camera card's name).
    struct Entry {
        bool folder = false;
        std::string name;
        std::filesystem::path path;
        std::string card;
        std::vector<std::filesystem::path> files;
        std::size_t audioChannels = 0; ///< P2: audio files beside the picture
        std::uintmax_t bytes = 0;
    };

    struct Readers {
        std::function<std::expected<LayeredDocument, Error>(const std::filesystem::path&)> readLayers;
        /// A media item (not yet in a project) for `spec`, named `name`.
        std::function<std::expected<MediaItem, Error>(const LayeredFileSpec& spec, std::string name)> describeLayered;
        std::function<std::expected<std::vector<Entry>, Error>(const std::filesystem::path& folder)> browse;
        /// The items of the clip that starts with `firstFile` (one; P2: the picture, then one per audio channel).
        std::function<std::expected<std::vector<MediaItem>, Error>(const std::filesystem::path& firstFile)> describeClip;
        /// Ingest (Premiere's Media Browser > Ingest, Copy): copies the clip's files into `destination`/<clip name>/ and
        /// describes the copies (as describeClip). Files already there with the same size are not copied again.
        std::function<std::expected<std::vector<MediaItem>, Error>(const std::filesystem::path& firstFile,
                                                                   const std::filesystem::path& destination)>
            ingestClip;
    };

    explicit MediaFormatServices(Readers readers) : m_readers(std::move(readers)) {}

    const Readers& readers() const { return m_readers; }

private:
    Readers m_readers;
};

} // namespace wabcut::command
