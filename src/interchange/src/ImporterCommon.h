#pragma once

// Pieces several parsers share: media registration, marker colours, A/V link inference.

#include "Ir.h"
#include "TextUtil.h"

#include <wabcut/interchange/FileUrl.h>

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>

namespace wabcut::interchange::detail {

/// Deduplicating media table for a document.
class MediaTable {
public:
    explicit MediaTable(IrDocument& document) : m_doc(document) {}

    /// Index of the media with `key`, or kNoMedia.
    std::size_t find(const std::string& key) const {
        const auto it = m_index.find(key);
        return it == m_index.end() ? kNoMedia : it->second;
    }

    /// Adds `media` unless its key is known; returns its index either way.
    std::size_t add(IrMedia media) {
        if (const std::size_t existing = find(media.key); existing != kNoMedia) {
            return existing;
        }
        const std::size_t index = m_doc.media.size();
        m_index.emplace(media.key, index);
        m_doc.media.push_back(std::move(media));
        return index;
    }

    IrMedia& operator[](std::size_t index) { return m_doc.media[index]; }

private:
    IrDocument& m_doc;
    std::unordered_map<std::string, std::size_t> m_index;
};

/// Builds a MediaReference from a URL or path as written in the file. Undecodable references keep the
/// original text and an empty path; the caller reports them.
inline MediaReference referenceFrom(std::string_view original, std::string_view name, bool& decoded) {
    MediaReference ref;
    ref.original = std::string(original);
    ref.name = std::string(name);
    decoded = false;
    if (!trim(original).empty()) {
        if (auto path = mediaReferenceToPath(original)) {
            ref.path = std::move(*path);
            decoded = true;
        }
    }
    if (ref.name.empty()) {
        ref.name = std::string(fileNameOf(ref.path));
    }
    return ref;
}

/// True for file names of still images (which have no duration of their own).
inline bool isStillImageName(std::string_view name) {
    static constexpr std::string_view kExtensions[] = {".png",  ".jpg", ".jpeg", ".tif", ".tiff",
                                                       ".bmp",  ".gif", ".psd",  ".tga", ".webp",
                                                       ".heic", ".ai",  ".exr",  ".svg"};
    for (const std::string_view ext : kExtensions) {
        if (iendsWith(name, ext)) {
            return true;
        }
    }
    return false;
}

/// Wabcut marker colour index, following Premiere Pro's marker colour order:
/// 0 green, 1 red, 2 purple (magenta), 3 orange, 4 yellow, 5 white, 6 blue, 7 cyan.
inline std::int32_t markerColorFromName(std::string_view name) {
    const std::string n = toLowerAscii(trim(name));
    if (n == "red") {
        return 1;
    }
    if (n == "purple" || n == "magenta" || n == "pink" || n == "violet" || n == "lavender") {
        return 2;
    }
    if (n == "orange") {
        return 3;
    }
    if (n == "yellow") {
        return 4;
    }
    if (n == "white" || n == "cream" || n == "black") {
        return 5;
    }
    if (n == "blue") {
        return 6;
    }
    if (n == "cyan" || n == "teal") {
        return 7;
    }
    return 0; // green and anything unknown
}

/// Nearest of the eight Premiere marker colours to an RGB value.
inline std::int32_t markerColorFromRgb(int r, int g, int b) {
    struct Swatch {
        int r, g, b;
    };
    static constexpr Swatch kPalette[] = {{0x2D, 0xC7, 0x4E}, {0xE0, 0x2F, 0x2F}, {0xB2, 0x3C, 0xE0},
                                          {0xF2, 0x8A, 0x1F}, {0xF2, 0xD7, 0x1F}, {0xF0, 0xF0, 0xF0},
                                          {0x2F, 0x6E, 0xE0}, {0x2F, 0xD0, 0xE0}};
    std::int32_t best = 0;
    long bestDistance = -1;
    for (std::int32_t i = 0; i < 8; ++i) {
        const long dr = r - kPalette[i].r;
        const long dg = g - kPalette[i].g;
        const long db = b - kPalette[i].b;
        const long distance = dr * dr + dg * dg + db * db;
        if (bestDistance < 0 || distance < bestDistance) {
            bestDistance = distance;
            best = i;
        }
    }
    return best;
}

/// Links clips that formats without explicit links (OTIO, some EDLs) imply belong together: clips of
/// the same media with the same timeline range and source In on a video track and on audio tracks.
/// `nextGroup` is the document's link group counter.
inline void inferLinks(IrSequence& sequence, std::int64_t& nextGroup) {
    struct Key {
        std::size_t media;
        std::int64_t start, duration, sourceIn;
        bool operator<(const Key& o) const {
            return std::tie(media, start, duration, sourceIn) <
                   std::tie(o.media, o.start, o.duration, o.sourceIn);
        }
    };
    struct Members {
        std::vector<IrClip*> video;
        std::vector<IrClip*> audio;
    };
    std::map<Key, Members> groups;
    for (auto* list : {&sequence.videoTracks, &sequence.audioTracks}) {
        for (IrTrack& t : *list) {
            for (IrClip& c : t.clips) {
                if (c.media == kNoMedia || c.linkGroup >= 0) {
                    continue;
                }
                Members& m = groups[{c.media, c.start.value, c.duration.value, c.sourceIn.value}];
                (t.kind == TrackKind::Video ? m.video : m.audio).push_back(&c);
            }
        }
    }
    for (auto& [key, members] : groups) {
        // One video clip plus its audio: an unambiguous A/V pair.
        if (members.video.size() != 1 || members.audio.empty()) {
            continue;
        }
        const std::int64_t group = nextGroup++;
        members.video.front()->linkGroup = group;
        for (IrClip* a : members.audio) {
            a->linkGroup = group;
        }
    }
}

} // namespace wabcut::interchange::detail
