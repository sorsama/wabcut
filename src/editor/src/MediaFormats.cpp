#include <wabcut/editor/MediaFormats.h>

#include <wabcut/editor/CameraCards.h>
#include <wabcut/editor/MediaImport.h>
#include <wabcut/media/Psd.h>
#include <wabcut/model/MediaLocator.h>

#include <utility>

namespace wabcut::editor {

std::expected<MediaItem, Error> describeLayeredFile(const LayeredFileSpec& spec, std::string name,
                                                    const std::filesystem::path& projectFile) {
    const std::string path = layeredFilePath(spec);
    if (path.empty()) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "Not a layered file: " + pathToUtf8(spec.file)});
    }
    auto item = describeMediaFile(pathFromUtf8(path), projectFile);
    if (!item) {
        return std::unexpected(std::move(item.error()));
    }
    item->name = std::move(name);
    return item;
}

std::shared_ptr<command::MediaFormatServices> mediaFormatServices(const std::filesystem::path& projectFile,
                                                                  bool readChapters) {
    command::MediaFormatServices::Readers readers;
    readers.readLayers = [](const std::filesystem::path& file)
        -> std::expected<command::MediaFormatServices::LayeredDocument, Error> {
        auto doc = media::readPsdDocument(file);
        if (!doc) {
            return std::unexpected(std::move(doc.error()));
        }
        command::MediaFormatServices::LayeredDocument out;
        out.width = doc->width;
        out.height = doc->height;
        out.depth = doc->depth;
        out.colorMode = doc->cmyk ? "cmyk" : doc->grayscale ? "grayscale" : "rgb";
        for (const media::PsdLayer& l : doc->layers) {
            out.layers.push_back({l.index, l.name, l.visible, l.hasPixels(), l.group, l.left, l.top, l.right, l.bottom,
                                  l.blendMode, l.groups});
        }
        return out;
    };
    readers.describeLayered = [projectFile](const LayeredFileSpec& spec, std::string name) {
        return describeLayeredFile(spec, std::move(name), projectFile);
    };
    readers.browse = [](const std::filesystem::path& folder)
        -> std::expected<std::vector<command::MediaFormatServices::Entry>, Error> {
        std::error_code ec;
        if (!std::filesystem::is_directory(folder, ec)) {
            return std::unexpected(Error{ErrorCode::NotFound, "Not a folder: " + pathToUtf8(folder)});
        }
        std::vector<command::MediaFormatServices::Entry> out;
        for (BrowserEntry& e : browseFolder(folder)) {
            command::MediaFormatServices::Entry entry;
            entry.folder = e.folder;
            entry.name = e.name;
            entry.path = e.path;
            if (e.clip) {
                entry.card = std::string(displayName(e.clip->card));
                entry.files = e.clip->files;
                entry.audioChannels = e.clip->audio.size();
                entry.bytes = e.clip->bytes();
            }
            out.push_back(std::move(entry));
        }
        return out;
    };
    // P0 round: a clip's chapter markers come in with it, as with File > Import (the picture's file; a spanned clip's
    // parts and a P2 clip's audio files have none to read).
    const auto withChapters = [readChapters](std::expected<std::vector<MediaItem>, Error> items) {
        if (items && readChapters) {
            for (MediaItem& item : *items) {
                addChapterMarkers(item);
            }
        }
        return items;
    };
    readers.describeClip = [projectFile, withChapters](const std::filesystem::path& firstFile) {
        return withChapters(describeCameraClip(cameraClipOf(firstFile), projectFile));
    };
    readers.ingestClip = [projectFile, withChapters](const std::filesystem::path& firstFile,
                                                     const std::filesystem::path& destination)
        -> std::expected<std::vector<MediaItem>, Error> {
        auto copied = copyCameraClip(cameraClipOf(firstFile), destination);
        if (!copied) {
            return std::unexpected(std::move(copied.error()));
        }
        return withChapters(describeCameraClip(*copied, projectFile));
    };
    return std::make_shared<command::MediaFormatServices>(std::move(readers));
}

} // namespace wabcut::editor
