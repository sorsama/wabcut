#pragma once

// OGraf graphics (the EBU's open, HTML-based broadcast graphics format, https://ograf.ebu.io), 0.7 (card 178).
//
// An OGraf graphic is a package: a manifest "<name>.ograf.json" (id, name, main, what the graphic supports, and a JSON
// Schema of its data - the fields an editor shows) and a JavaScript module "main" that draws the graphic as a web
// component. Wabcut has no web engine and never runs scripts, so what it can do with a package is:
//   * read the manifest and the fields it declares (parseOGrafManifest) - bounded, with every path checked;
//   * draw a package that also ships a Lottie version of the graphic: one .json / .lottie animation in the package, or the
//     one the manifest names in its vendor field "v_wabcut_lottie" (OGraf allows vendor fields that start with "v_").
//     It imports as a motion graphic template (Lottie.h) whose text, colour and show/hide fields are named after the
//     manifest's: a Lottie layer called like a field (ignoring case, spaces and punctuation; "<layer>Color" for the colour
//     of a layer) is that field's layer. The manifest's other fields, and its defaults, are listed in the report: the
//     Lottie's own texts and colors are the starting values.
//   * refuse a package without a Lottie version with a message that says why (importOGraf: Unsupported): its drawing is a
//     web page.
//
// Untrusted input: the manifest at most kMaxOGrafManifestBytes with bounded nesting and counts, "main", the Lottie path and
// the thumbnails safe relative paths (Zip.h isSafeZipPath), a ZIP read as Zip.h says, a folder read without following
// links; nothing is executed and nothing is written; the result passes templateProblem().

#include <wabcut/interchange/ImportReport.h>
#include <wabcut/model/Error.h>
#include <wabcut/model/Graphics.h>
#include <wabcut/model/Time.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::interchange {

inline constexpr std::size_t kMaxOGrafManifestBytes = 1024u * 1024u;
inline constexpr std::size_t kMaxOGrafProperties = 256;
inline constexpr std::size_t kMaxOGrafPackageFiles = 4096;
inline constexpr std::size_t kMaxOGrafPackageBytes = 64u * 1024u * 1024u;

/// What a field of the manifest's data schema holds, by its JSON Schema type and GDD type.
enum class OGrafValueKind : std::uint8_t { Text, Color, Number, Boolean, Choice, Image, Other };

struct OGrafProperty {
    std::string key;         ///< the property's name in the data (at most 128 bytes)
    std::string title;       ///< its title ("" = none)
    std::string description;
    OGrafValueKind kind = OGrafValueKind::Other;
    std::string gddType;     ///< "single-line", "color-rrggbb", ... as written
    std::string defaultText; ///< a string default as written (a colour: "#rrggbb")
    double defaultNumber = 0.0;
    bool defaultBool = false;
    bool hasDefault = false;
    std::vector<std::string> choices; ///< the "enum" of a string
    bool hidden = false;
    double order = 0.0;
};

struct OGrafManifest {
    std::string id;
    std::string version;
    std::string name;
    std::string description;
    std::string author;
    std::string main;   ///< the script that draws the graphic - never run: a safe relative path
    std::string lottie; ///< "v_wabcut_lottie": the package's Lottie version, a safe relative path ("" = none named)
    bool supportsRealTime = false;
    bool supportsNonRealTime = false;
    std::int32_t stepCount = 1; ///< -1: dynamic
    std::vector<OGrafProperty> properties; ///< by "order", then by name
    std::vector<std::string> customActions; ///< their ids
    std::vector<std::string> thumbnails;    ///< safe relative paths of the manifest's thumbnail images
    ImportReport report;                    ///< what was ignored or unusual in the manifest
};

/// Reads a manifest. Errors: ParseError (too big or deep, not JSON, not an object, a required field missing or of the wrong
/// type, "main" or the Lottie path not a safe relative path).
std::expected<OGrafManifest, Error> parseOGrafManifest(std::string_view json, std::string_view fileName = "graphic.ograf.json");

/// The files of a package: safe relative '/'-separated paths and a way to read one of them (bounded).
struct OGrafPackage {
    std::vector<std::string> files;
    std::function<std::expected<std::string, Error>(const std::string&)> read;
};

/// A package from a ZIP's bytes (Zip.h), or from a folder (no links followed, at most kMaxOGrafPackageFiles files, 6 levels,
/// each file read at most 16 MiB). Errors: ParseError, IoError.
std::expected<OGrafPackage, Error> openOGrafZip(std::string_view bytes);
std::expected<OGrafPackage, Error> openOGrafFolder(const std::string& utf8Folder);

/// The manifests ("*.ograf.json") of a package, sorted.
std::vector<std::string> ografManifests(const OGrafPackage& package);

struct OGrafImport {
    GraphicTemplate tmpl;     ///< the Lottie version as a template with the manifest's field names
    Ticks duration{};         ///< the animation's length
    Rate frameRate;
    OGrafManifest manifest;
    std::string lottieFile;   ///< the package file that draws it
    ImportReport report;      ///< the manifest's notes, the Lottie's report and the fields that found no layer
};

/// Reads the manifest of one graphic of a package (`manifestFile`: its path; empty when the package holds just one), without
/// importing it - what an import dialog shows. `chosenOut`, when given, receives the manifest's path. Errors as importOGraf's
/// for the manifest.
std::expected<OGrafManifest, Error> readOGrafManifest(const OGrafPackage& package, std::string_view manifestFile = {},
                                                      std::string* chosenOut = nullptr);

/// Imports one graphic of a package (`manifestFile`: its manifest's path; empty when the package holds just one). Errors:
/// ParseError (no manifest, several and none named, a bad manifest or Lottie), Unsupported (no Lottie version: the message
/// says the graphic is a web page and lists its fields).
std::expected<OGrafImport, Error> importOGraf(const OGrafPackage& package, std::string_view manifestFile = {});

/// A template id from an OGraf graphic id ("com.example.lower-third" -> "ograf.com.example.lower-third").
std::string ografTemplateId(std::string_view graphicId);

} // namespace wabcut::interchange
