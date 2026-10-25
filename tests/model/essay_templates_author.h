#pragma once

// 0.4: the authoring code of the built-in essay and documentary templates (wabcut.essay.*), next to the gaming pack's
// (gaming_templates_author.h). The files under src/model/templates/builtin are what the app ships (compiled in,
// BuiltinTemplates.h); these builders are how they are made. Unlike the gaming pack, the essay templates take the
// project's brand kit: box and bar fills refer to the kit's "Primary" / "Accent" colours (with neutral fallbacks) and
// text to its "heading" / "body" font roles (with IBM Plex Sans Thai as the family, so Thai renders).
// test_builtin_templates.cpp checks that the files are what the builders make and rewrites them when
// WABCUT_UPDATE_TEMPLATES=1 (docs/graphics.md, "Built-in templates (C.16)").

#include <wabcut/model/Graphics.h>

#include <vector>

namespace wabcut::test {

/// The 5 essay templates (chapterCard, location, nameTitle, quote, source), in id order. Throws std::runtime_error when
/// one cannot be built (a preset or a template problem).
std::vector<GraphicTemplate> authorEssayTemplates();

} // namespace wabcut::test
