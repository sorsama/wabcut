#pragma once

// Graphics as JSON (C.13, docs/graphics.md "JSON"): the format of a clip's "graphic" in project files, of the graphic
// commands' layer parameters, of brand kits, and of template files in the user's template library folder.
//
// Readers take untrusted text: they bound its size and nesting, check every field's type, name the path of the first
// problem, and pass the result through graphicProblem / brandKitProblem / templateProblem. Writers produce what the
// readers read back unchanged (numbers with round-trip precision).
//
// Template library folder: one "<id>.wabcut-template.json" per template, {"format": "wabcut-graphic-template",
// "version": 1, "template": {...}}; a newer version is refused, not half-read.

#include <wabcut/model/Error.h>
#include <wabcut/model/Graphics.h>

#include <cstddef>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::io {

inline constexpr std::size_t kMaxGraphicJsonBytes = 16 * 1024 * 1024;
inline constexpr std::string_view kTemplateFileFormat = "wabcut-graphic-template";
inline constexpr int kTemplateFileVersion = 1;
inline constexpr std::string_view kTemplateFileSuffix = ".wabcut-template.json";
inline constexpr std::size_t kMaxLibraryTemplates = 1000;

std::string graphicToJson(const Graphic& graphic);
std::expected<Graphic, Error> graphicFromJson(std::string_view text);

/// One layer (a command's "layer" parameter). Checked on its own as the only layer of a canvas of `width` x `height`.
std::string layerToJson(const GraphicLayer& layer);
std::expected<GraphicLayer, Error> layerFromJson(std::string_view text, std::int32_t width = 1920,
                                                 std::int32_t height = 1080);

/// A template field's value as JSON (the "value" of a field in the formats above): text a string, colour [r, g, b, a] or
/// {"brand", "rgba"} (or "#RRGGBB[AA]" on reading), font {"family", "style", "weight"} (or the family alone), image a
/// media id, duration ticks, checkbox a boolean.
std::string fieldValueToJson(FieldKind kind, const FieldValue& value);
std::expected<FieldValue, Error> fieldValueFromJson(FieldKind kind, std::string_view text);

std::string brandKitToJson(const BrandKit& kit);
std::expected<BrandKit, Error> brandKitFromJson(std::string_view text);

/// A template as its library file holds it.
std::string templateFileToJson(const GraphicTemplate& tmpl);
std::expected<GraphicTemplate, Error> templateFileFromJson(std::string_view text);

struct LibraryTemplate {
    std::filesystem::path file;
    std::expected<GraphicTemplate, Error> tmpl;
};

/// The templates in a library folder ("*.wabcut-template.json", at most kMaxLibraryTemplates, each at most
/// kMaxGraphicJsonBytes), sorted by file name; files that do not read are listed with their error.
std::vector<LibraryTemplate> readTemplateLibrary(const std::filesystem::path& folder);
/// Writes `tmpl` to "<folder>/<id>.wabcut-template.json" (atomically), creating the folder. Returns the file.
std::expected<std::filesystem::path, Error> writeTemplateToLibrary(const std::filesystem::path& folder,
                                                                  const GraphicTemplate& tmpl);

} // namespace wabcut::io
