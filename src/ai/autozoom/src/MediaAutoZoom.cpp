#include <wabcut/ai/autozoom/MediaAutoZoom.h>

#include <wabcut/ai/edit/MediaServices.h>
#include <wabcut/ai/search/MediaSearchAccess.h>

#include <fstream>
#include <sstream>

namespace wabcut::ai::autozoom {

std::expected<FrameReader, Error> openFileFrames(const std::string& path, int containerStream) {
    // One reader per analysis width; the decoder is opened on first use and kept (frames are read forward).
    struct State {
        std::string path;
        int stream = -1;
        int width = 0;
        std::unique_ptr<search::VideoPictureReader> reader;
    };
    auto state = std::make_shared<State>();
    state->path = path;
    state->stream = containerStream;
    std::error_code ec;
    if (!std::filesystem::exists(std::filesystem::path(std::u8string(path.begin(), path.end())), ec)) {
        return std::unexpected(Error(ErrorCode::NotFound, "the recording's file is missing"));
    }
    return FrameReader([state](Ticks time, int width) -> std::expected<GreyFrame, Error> {
        if (!state->reader || state->width != width) {
            auto opened = search::VideoPictureReader::open(state->path, state->stream, width);
            if (!opened) {
                return std::unexpected(opened.error());
            }
            state->reader = std::move(*opened);
            state->width = width;
        }
        auto picture = state->reader->at(time);
        if (!picture) {
            return std::unexpected(picture.error());
        }
        if (!picture->valid()) {
            return std::unexpected(Error(ErrorCode::DecoderError, "the decoder returned an empty picture"));
        }
        return toAnalysisFrame(picture->rgb.data(), picture->width, picture->height, width, time);
    });
}

std::optional<std::string> findCursorSidecar(const std::string& path) {
    const std::filesystem::path file(std::u8string(path.begin(), path.end()));
    std::vector<std::filesystem::path> candidates;
    for (const char* suffix : {".cursor.json", ".cursor.csv"}) {
        candidates.push_back(std::filesystem::path(file).concat(suffix));
        candidates.push_back(std::filesystem::path(file).replace_extension().concat(suffix));
    }
    for (const auto& candidate : candidates) {
        std::error_code ec;
        const auto size = std::filesystem::file_size(candidate, ec);
        if (ec || size > 128ull * 1024 * 1024) {
            continue;
        }
        std::ifstream in(candidate, std::ios::binary);
        std::ostringstream text;
        text << in.rdbuf();
        return text.str();
    }
    return std::nullopt;
}

std::shared_ptr<AutoZoomServices> mediaAutoZoomServices(const std::filesystem::path& projectFile, bool readFiles) {
    auto frames = [projectFile](const MediaItem& media, int videoStream) -> std::expected<FrameReader, Error> {
        const auto path = edit::mediaFilePath(media, projectFile);
        if (!path) {
            return std::unexpected(Error(ErrorCode::NotFound, "'" + media.name + "' is offline"));
        }
        const int stream = videoStream >= 0 && static_cast<std::size_t>(videoStream) < media.info.video.size()
                               ? media.info.video[static_cast<std::size_t>(videoStream)].index
                               : -1;
        return openFileFrames(*path, stream);
    };
    auto sidecar = [projectFile](const MediaItem& media) -> std::optional<std::string> {
        const auto path = edit::mediaFilePath(media, projectFile);
        return path ? findCursorSidecar(*path) : std::nullopt;
    };
    return std::make_shared<AutoZoomServices>(std::move(frames), std::move(sidecar), readFiles);
}

} // namespace wabcut::ai::autozoom
