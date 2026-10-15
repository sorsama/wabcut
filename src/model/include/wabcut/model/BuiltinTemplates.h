#pragma once

// Built-in graphic templates (C.16): the templates that ship with Wabcut - a gaming pack of lower thirds, banners,
// callouts, stingers and end cards. They live as C.13 template files under src/model/templates/builtin (the editable
// source of truth, docs/graphics.md "Built-in templates") and are compiled into the model, so every host - the app,
// wabcut-cli, MCP clients and the assistant - has them without a resource path. template.list shows them as the
// "builtin" library; template.instantiate finds them after the project's and the user's libraries.

#include <wabcut/model/Graphics.h>
#include <wabcut/model/Time.h>

#include <string_view>
#include <vector>

namespace wabcut {

/// Template ids of the built-ins other code refers to.
inline constexpr std::string_view kReplayTemplateId = "wabcut.gaming.replay";

/// C.16: the graphic templates that ship with Wabcut (src/model/templates/builtin, compiled in), parsed once, in id order.
/// A file that does not read is a build defect: it is skipped here and the model tests fail on it.
const std::vector<GraphicTemplate>& builtinTemplates();
/// The built-in template with this id, or null.
const GraphicTemplate* findBuiltinTemplate(std::string_view id);
/// The design time a thumbnail shows: the end of a responsive template's intro (everything has animated in), else half
/// its length.
Ticks templatePosterTime(const GraphicTemplate& tmpl);
/// The library category of a built-in ("gaming" for wabcut.gaming.*), empty for others.
std::string_view templateCategory(const GraphicTemplate& tmpl);

} // namespace wabcut
