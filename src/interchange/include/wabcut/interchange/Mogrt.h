#pragma once

// Motion Graphics templates made in Premiere Pro (.mogrt, 0.2, docs/graphics.md "Motion Graphics templates"), imported
// approximately as a Wabcut graphic template.
//
// A .mogrt is a ZIP (Zip.h, read bounded) holding "definition.json" - the template's name, its canvas and length, and
// its Essential Properties ("clientControls": a label, a type and a default value each) - with a thumbnail and the
// project the template was made from: "*.aegraphic" when After Effects made it (an After Effects composition: Wabcut
// cannot run one, so such files are refused with a message that says so; export the composition as Lottie instead), a
// Premiere graphic otherwise. Wabcut does not read the Premiere graphic itself (Adobe's binary project data): it builds
// the template from the definition - every text property becomes a text layer of the graphic and a text field (with
// its label and default text), the first colour property the colour of a box behind the text and the others the texts'
// colours (colour fields), checkboxes show or hide the box; sliders, point, angle, dropdown, font-menu and media
// properties are reported as dropped. The layout is Wabcut's: a lower third on the template's canvas - text lines
// stacked from the lower left, the box pinned around them. The report (ImportReport.h) lists what was carried,
// approximated and dropped, with the JSON path of each property.
//
// Untrusted input: at most kMaxMogrtBytes, the ZIP checked as Zip.h says, definition.json at most 4 MiB and nesting
// bounded, at most kMaxGraphicLayers text properties; the result passes templateProblem().

#include <wabcut/interchange/ImportReport.h>
#include <wabcut/model/Error.h>
#include <wabcut/model/Graphics.h>

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>

namespace wabcut::interchange {

inline constexpr std::size_t kMaxMogrtBytes = 64u * 1024u * 1024u;
inline constexpr std::size_t kMaxMogrtDefinitionBytes = 4u * 1024u * 1024u;

struct MogrtImport {
    GraphicTemplate tmpl;
    /// "Premiere Pro", or what definition.json said made it.
    std::string authorApp;
    ImportReport report;
};

/// Imports a .mogrt's bytes. `fileName` names the source in the report and gives the template its name and id when
/// the definition has none. Errors: ParseError (not a ZIP, no definition.json, damaged JSON), Unsupported (made in
/// After Effects, or no property Wabcut can use).
std::expected<MogrtImport, Error> importMogrt(std::string_view data, std::string_view fileName = "template.mogrt");

} // namespace wabcut::interchange
