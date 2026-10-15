#pragma once

// C.16: the authoring code of the built-in gaming templates. The files under src/model/templates/builtin are what the
// app ships (compiled in, BuiltinTemplates.h); these builders are how they are made: layers built with the model API,
// animation presets baked with edit::applyPresetToLayer (editable keys, as the graphic.applyPreset command writes them),
// fields bound with edit::makeTemplate. test_builtin_templates.cpp checks that the files are what the builders make and
// rewrites them when WABCUT_UPDATE_TEMPLATES=1 (docs/graphics.md, "Built-in templates (C.16)").

#include <wabcut/model/Graphics.h>

#include <vector>

namespace wabcut::test {

/// The 12 gaming templates, in id order. Throws std::runtime_error when one cannot be built (a preset or a template
/// problem).
std::vector<GraphicTemplate> authorGamingTemplates();

} // namespace wabcut::test
