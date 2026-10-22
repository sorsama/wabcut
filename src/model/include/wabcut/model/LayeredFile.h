#pragma once

// Layered Photoshop files (0.2, Premiere's Import Layered File dialog). A PSD / PSB file imports as one picture of all
// its layers merged, as one picture of the layers chosen, as one item per layer, or as a sequence with one layer per
// track. Each such item names its file and its layers in its locator path:
//
//     <file>.psd?layers=<index>,<index>,...&size=document|layer
//
// `layers` are layer indices of the file (0 = the bottom layer, as Photoshop stores them), merged bottom to top; "all"
// means every visible layer. `size=document` shows the layers on the document's canvas (each where it sits in the
// document), `size=layer` on the bounds of the layers' pixels only (Premiere's Layer Size). media::detail::openInput
// composites the layers (media/Psd.h) and hands FFmpeg the picture, so probing, thumbnails, playback and export need
// nothing else; saving the file in Photoshop updates the items when they are opened again. The file itself stands for
// the item in file checks (ImageSequence.h: mediaFileToCheck). Paths read from a project are untrusted:
// parseLayeredFilePath bounds every field and returns nullopt for anything else.

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut {

struct LayeredFileSpec {
    std::filesystem::path file;
    /// Layer indices merged, bottom to top; empty = every visible layer ("all").
    std::vector<int> layers;
    /// The document's canvas (true) or the layers' own bounds (false).
    bool documentSize = true;

    bool operator==(const LayeredFileSpec&) const = default;
};

inline constexpr std::size_t kMaxLayeredFileLayers = 4096;

/// True for ".psd" and ".psb" (any case).
bool isLayeredFileExtension(std::string_view extension);
/// The media path of `spec` (see the file comment); empty when out of bounds.
std::string layeredFilePath(const LayeredFileSpec& spec);
/// The layered-file item a media path names, or nullopt for any other path.
std::optional<LayeredFileSpec> parseLayeredFilePath(std::string_view utf8);

} // namespace wabcut
