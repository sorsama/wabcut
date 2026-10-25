// Built-in graphic templates (C.16): BuiltinTemplates.h.
//
// The template files of src/model/templates/builtin are compiled in by cmake/EmbedTemplates.cmake (a generated
// EmbeddedTemplates.cpp in the build folder); they are parsed with the template file reader on first use.

#include <wabcut/model/BuiltinTemplates.h>

#include <wabcut/model/GraphicJson.h>

#include <algorithm>
#include <string>
#include <utility>

namespace wabcut::detail {
/// The embedded files (generated): file name and bytes, sorted by file name.
std::vector<std::pair<std::string_view, std::string_view>> embeddedTemplateFiles();
} // namespace wabcut::detail

namespace wabcut {

namespace {

constexpr std::string_view kGamingPrefix = "wabcut.gaming.";
constexpr std::string_view kEssayPrefix = "wabcut.essay."; // 0.4: the essay and documentary pack

std::vector<GraphicTemplate> readEmbedded() {
    std::vector<GraphicTemplate> out;
    for (const auto& [name, bytes] : detail::embeddedTemplateFiles()) {
        // A file that does not read is a build defect (the model tests read every file): it is left out here.
        if (auto tmpl = io::templateFileFromJson(bytes)) {
            out.push_back(std::move(*tmpl));
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const GraphicTemplate& a, const GraphicTemplate& b) { return a.id < b.id; });
    // Ids are unique (file names are ids); a repeat keeps the first.
    out.erase(std::unique(out.begin(), out.end(), [](const GraphicTemplate& a, const GraphicTemplate& b) { return a.id == b.id; }),
              out.end());
    return out;
}

} // namespace

const std::vector<GraphicTemplate>& builtinTemplates() {
    static const std::vector<GraphicTemplate> templates = readEmbedded(); // thread-safe initialisation
    return templates;
}

const GraphicTemplate* findBuiltinTemplate(std::string_view id) {
    const std::vector<GraphicTemplate>& all = builtinTemplates();
    const auto it = std::lower_bound(all.begin(), all.end(), id,
                                     [](const GraphicTemplate& t, std::string_view key) { return std::string_view(t.id) < key; });
    return it != all.end() && it->id == id ? &*it : nullptr;
}

Ticks templatePosterTime(const GraphicTemplate& tmpl) {
    if (tmpl.graphic.responsive && tmpl.graphic.responsive->intro.value > 0) {
        return tmpl.graphic.responsive->intro;
    }
    // Half the default clip length (edit::instantiateTemplate's): the template's duration, its responsive duration or 5 s.
    Ticks length = Ticks::fromSeconds(5);
    if (tmpl.duration.value > 0) {
        length = tmpl.duration;
    } else if (tmpl.graphic.responsive) {
        length = tmpl.graphic.responsive->duration;
    }
    return Ticks{length.value / 2};
}

std::string_view templateCategory(const GraphicTemplate& tmpl) {
    if (tmpl.id.size() > kGamingPrefix.size() && std::string_view(tmpl.id).starts_with(kGamingPrefix)) {
        return "gaming";
    }
    if (tmpl.id.size() > kEssayPrefix.size() && std::string_view(tmpl.id).starts_with(kEssayPrefix)) {
        return "essay";
    }
    return {};
}

} // namespace wabcut
