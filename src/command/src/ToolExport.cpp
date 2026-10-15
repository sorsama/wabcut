#include <wabcut/command/ToolExport.h>

#include <wabcut/command/Schema.h>

namespace wabcut::command {

std::optional<ToolFormat> toolFormatFromString(std::string_view text) {
    if (text == "jsonschema") {
        return ToolFormat::JsonSchema;
    }
    if (text == "anthropic") {
        return ToolFormat::Anthropic;
    }
    if (text == "openai") {
        return ToolFormat::OpenAI;
    }
    return std::nullopt;
}

Json exportTools(const CommandRegistry& registry, ToolFormat format, CapabilitySet allowed) {
    Json tools = Json::array();
    const auto specs = registry.all();
    for (std::size_t i = 0; i < specs.size(); ++i) {
        const CommandSpec& spec = specs[i];
        // all() is sorted by name then version: the last entry of a name is its newest version.
        if (i + 1 < specs.size() && specs[i + 1].name == spec.name) {
            continue;
        }
        if (!allowed.has(spec.capability)) {
            continue;
        }
        const std::string toolName = toolNameFor(spec.name);
        switch (format) {
        case ToolFormat::JsonSchema: {
            Json parameters = spec.params;
            parameters["$schema"] = kJsonSchemaDialect;
            tools.push_back(Json{{"name", spec.name},
                                 {"toolName", toolName},
                                 {"version", spec.version},
                                 {"title", spec.title},
                                 {"capability", toString(spec.capability)},
                                 {"aliases", spec.aliases},
                                 {"description", spec.description},
                                 {"parameters", std::move(parameters)},
                                 {"example", spec.example}});
            break;
        }
        case ToolFormat::Anthropic:
            tools.push_back(Json{{"name", toolName}, {"description", spec.description}, {"input_schema", spec.params}});
            break;
        case ToolFormat::OpenAI:
            tools.push_back(Json{{"type", "function"},
                                 {"function", Json{{"name", toolName},
                                                   {"description", spec.description},
                                                   {"parameters", spec.params}}}});
            break;
        }
    }
    return tools;
}

} // namespace wabcut::command
