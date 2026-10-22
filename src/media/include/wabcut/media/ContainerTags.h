#pragma once

// The metadata tags FFmpeg's demuxer exposes for a file (0.2, P.3: the Metadata panel's File section): the
// container's (title, artist, comment, encoder, creation_time, QuickTime keys, Matroska tags, ...) and each stream's
// (language, handler_name, ...). Reads headers only (no stream probing, no decoding). Untrusted: bounded.

#include <wabcut/model/Error.h>

#include <cstddef>
#include <expected>
#include <string>
#include <vector>

namespace wabcut::media {

inline constexpr std::size_t kMaxContainerTags = 512;      ///< tags read per dictionary
inline constexpr std::size_t kMaxContainerTagBytes = 4096; ///< longer values are cut (at a UTF-8 boundary)

struct ContainerTag {
    /// -1 for the container, else the stream's index.
    int stream = -1;
    /// "video", "audio", "subtitle", "data" for a stream; empty for the container.
    std::string kind;
    std::string key;
    std::string value;

    bool operator==(const ContainerTag&) const = default;
};

/// The tags of `path` (UTF-8): the container's first, then each stream's in order. Errors as media::probe.
std::expected<std::vector<ContainerTag>, Error> readContainerTags(const std::string& path);

} // namespace wabcut::media
