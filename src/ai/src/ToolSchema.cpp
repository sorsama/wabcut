#include <wabcut/ai/ToolSchema.h>

#include <algorithm>
#include <array>
#include <set>
#include <string>

namespace wabcut::ai {

namespace {

constexpr std::array<std::string_view, 22> kPortableKeywords{
    "type",      "properties", "required",  "additionalProperties", "items",     "enum",
    "const",     "anyOf",      "description", "title",              "default",   "examples",
    "minimum",   "maximum",    "exclusiveMinimum", "exclusiveMaximum", "minLength", "maxLength",
    "minItems",  "maxItems",   "$schema",   "$comment",
};

constexpr std::array<std::string_view, 7> kTypes{"string", "number", "integer", "boolean", "object", "array", "null"};

bool isPortableKeyword(std::string_view k) {
    return std::find(kPortableKeywords.begin(), kPortableKeywords.end(), k) != kPortableKeywords.end();
}

AiError invalid(const std::string& path, const std::string& what) {
    return AiError(AiErrorCode::InvalidRequest, (path.empty() ? std::string("/") : path) + ": " + what);
}

std::expected<void, AiError> checkNode(const Json& schema, const std::string& path) {
    if (!schema.is_object()) {
        return std::unexpected(invalid(path, "a schema must be an object"));
    }
    for (const auto& [key, value] : schema.items()) {
        if (!isPortableKeyword(key)) {
            return std::unexpected(invalid(path, "'" + key + "' is not portable"));
        }
    }
    if (auto it = schema.find("type"); it != schema.end()) {
        auto validType = [](const Json& t) {
            return t.is_string() && std::find(kTypes.begin(), kTypes.end(), t.get<std::string>()) != kTypes.end();
        };
        if (it->is_array()) {
            if (it->empty() || !std::all_of(it->begin(), it->end(), validType)) {
                return std::unexpected(invalid(path, "bad 'type' list"));
            }
        } else if (!validType(*it)) {
            return std::unexpected(invalid(path, "bad 'type'"));
        }
    }
    if (auto it = schema.find("additionalProperties"); it != schema.end()) {
        if (!it->is_boolean() || it->get<bool>()) {
            return std::unexpected(invalid(path, "'additionalProperties' must be false (closed objects only)"));
        }
    }
    if (auto it = schema.find("properties"); it != schema.end()) {
        if (!it->is_object()) {
            return std::unexpected(invalid(path, "'properties' must be an object"));
        }
        for (const auto& [name, sub] : it->items()) {
            if (auto r = checkNode(sub, path + "/properties/" + name); !r) {
                return r;
            }
        }
    }
    if (auto it = schema.find("required"); it != schema.end()) {
        if (!it->is_array() || !std::all_of(it->begin(), it->end(), [](const Json& r) { return r.is_string(); })) {
            return std::unexpected(invalid(path, "'required' must be an array of names"));
        }
        const auto props = schema.find("properties");
        for (const auto& name : *it) {
            if (props == schema.end() || !props->contains(name.get<std::string>())) {
                return std::unexpected(invalid(path, "required '" + name.get<std::string>() + "' is not a property"));
            }
        }
    }
    if (auto it = schema.find("items"); it != schema.end()) {
        if (auto r = checkNode(*it, path + "/items"); !r) {
            return r;
        }
    }
    if (auto it = schema.find("anyOf"); it != schema.end()) {
        if (!it->is_array() || it->empty()) {
            return std::unexpected(invalid(path, "'anyOf' must be a non-empty array"));
        }
        for (std::size_t i = 0; i < it->size(); ++i) {
            if (auto r = checkNode((*it)[i], path + "/anyOf/" + std::to_string(i)); !r) {
                return r;
            }
        }
    }
    if (auto it = schema.find("enum"); it != schema.end() && (!it->is_array() || it->empty())) {
        return std::unexpected(invalid(path, "'enum' must be a non-empty array"));
    }
    return {};
}

bool typeIncludes(const Json& schema, std::string_view type) {
    auto it = schema.find("type");
    if (it == schema.end()) {
        return false;
    }
    if (it->is_string()) {
        return it->get<std::string>() == type;
    }
    return std::any_of(it->begin(), it->end(), [&](const Json& t) { return t.is_string() && t.get<std::string>() == type; });
}

std::string formatValue(const Json& v) { return v.is_string() ? v.get<std::string>() : v.dump(); }

/// Moves `keyword` into the description as "<label>: <value>.".
void moveToDescription(Json& schema, std::string_view keyword, std::string_view label) {
    auto it = schema.find(std::string(keyword));
    if (it == schema.end()) {
        return;
    }
    std::string note = std::string(label) + ": " + formatValue(*it) + ".";
    schema.erase(std::string(keyword));
    std::string description = schema.value("description", std::string());
    if (!description.empty() && description.back() != ' ') {
        description += description.back() == '.' ? " " : ". ";
    }
    schema["description"] = description + note;
}

void constToEnum(Json& schema) {
    if (auto it = schema.find("const"); it != schema.end()) {
        Json value = *it;
        schema.erase("const");
        schema["enum"] = Json::array({value});
    }
}

void makeNullable(Json& schema) {
    if (auto it = schema.find("type"); it != schema.end()) {
        if (it->is_string()) {
            if (it->get<std::string>() != "null") {
                *it = Json::array({it->get<std::string>(), "null"});
            }
        } else if (it->is_array() && !typeIncludes(schema, "null")) {
            it->push_back("null");
        }
    } else if (auto any = schema.find("anyOf"); any != schema.end()) {
        any->push_back(Json{{"type", "null"}});
    }
    if (auto e = schema.find("enum"); e != schema.end() && std::find(e->begin(), e->end(), Json(nullptr)) == e->end()) {
        e->push_back(nullptr);
    }
}

Json adaptNode(const Json& in, SchemaDialect dialect, bool strict) {
    Json schema = in;
    schema.erase("$schema");
    schema.erase("$comment");
    schema.erase("examples");

    const bool isObject = typeIncludes(schema, "object") || schema.contains("properties");

    if (strict && dialect == SchemaDialect::Anthropic) {
        for (auto [kw, label] : {std::pair{"minimum", "Minimum"}, std::pair{"maximum", "Maximum"},
                                 std::pair{"exclusiveMinimum", "Greater than"}, std::pair{"exclusiveMaximum", "Less than"},
                                 std::pair{"minLength", "Minimum length"}, std::pair{"maxLength", "Maximum length"},
                                 std::pair{"maxItems", "At most items"}, std::pair{"default", "Default"}}) {
            moveToDescription(schema, kw, label);
        }
        if (auto it = schema.find("minItems"); it != schema.end() && it->is_number_integer() && it->get<long long>() > 1) {
            moveToDescription(schema, "minItems", "At least items");
        }
    }
    if (strict && dialect == SchemaDialect::OpenAiStrict) {
        moveToDescription(schema, "minLength", "Minimum length");
        moveToDescription(schema, "maxLength", "Maximum length");
        moveToDescription(schema, "default", "Default");
    }
    if (dialect != SchemaDialect::Anthropic) {
        constToEnum(schema);
    }
    if (dialect == SchemaDialect::Gemini) {
        schema.erase("additionalProperties");
    } else if (isObject && (strict || schema.contains("additionalProperties"))) {
        schema["additionalProperties"] = false;
    }

    if (auto props = schema.find("properties"); props != schema.end()) {
        std::set<std::string> required;
        if (auto r = schema.find("required"); r != schema.end()) {
            for (const auto& name : *r) {
                required.insert(name.get<std::string>());
            }
        }
        Json adapted = Json::object();
        Json allNames = Json::array();
        for (const auto& [name, sub] : props->items()) {
            Json child = adaptNode(sub, dialect, strict);
            if (strict && dialect == SchemaDialect::OpenAiStrict && !required.contains(name)) {
                makeNullable(child); // optional -> required but nullable
            }
            adapted[name] = std::move(child);
            allNames.push_back(name);
        }
        *props = std::move(adapted);
        if (strict && dialect == SchemaDialect::OpenAiStrict) {
            schema["required"] = std::move(allNames);
        }
    } else if (isObject && strict && dialect == SchemaDialect::OpenAiStrict) {
        schema["properties"] = Json::object();
        schema["required"] = Json::array();
    }
    if (auto items = schema.find("items"); items != schema.end()) {
        *items = adaptNode(*items, dialect, strict);
    }
    if (auto any = schema.find("anyOf"); any != schema.end()) {
        for (auto& option : *any) {
            option = adaptNode(option, dialect, strict);
        }
    }
    return schema;
}

} // namespace

std::expected<void, AiError> checkPortableSchema(const Json& schema) { return checkNode(schema, ""); }

Json adaptSchema(const Json& schema, SchemaDialect dialect, bool strict) { return adaptNode(schema, dialect, strict); }

bool isValidToolName(std::string_view name) {
    if (name.empty() || name.size() > 64) {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}

std::expected<void, AiError> validateChatRequest(const ChatRequest& request) {
    std::set<std::string> names;
    for (const auto& tool : request.tools) {
        if (!isValidToolName(tool.name)) {
            return std::unexpected(AiError(AiErrorCode::InvalidRequest, "invalid tool name '" + tool.name + "'"));
        }
        if (!names.insert(tool.name).second) {
            return std::unexpected(AiError(AiErrorCode::InvalidRequest, "duplicate tool '" + tool.name + "'"));
        }
        if (!tool.parameters.is_object() || !typeIncludes(tool.parameters, "object")) {
            return std::unexpected(
                AiError(AiErrorCode::InvalidRequest, "tool '" + tool.name + "': parameters must be an object schema"));
        }
        if (auto r = checkPortableSchema(tool.parameters); !r) {
            return std::unexpected(AiError(AiErrorCode::InvalidRequest, "tool '" + tool.name + "' " + r.error().message));
        }
    }
    if (request.toolChoice.mode == ToolChoiceMode::Named && !names.contains(request.toolChoice.name)) {
        return std::unexpected(
            AiError(AiErrorCode::InvalidRequest, "tool choice names unknown tool '" + request.toolChoice.name + "'"));
    }
    if (request.toolChoice.mode != ToolChoiceMode::Auto && request.toolChoice.mode != ToolChoiceMode::None &&
        request.tools.empty()) {
        return std::unexpected(AiError(AiErrorCode::InvalidRequest, "a forced tool choice needs tools"));
    }
    if (request.responseFormat) {
        if (!isValidToolName(request.responseFormat->name)) {
            return std::unexpected(AiError(AiErrorCode::InvalidRequest, "invalid response format name"));
        }
        if (!typeIncludes(request.responseFormat->schema, "object")) {
            return std::unexpected(AiError(AiErrorCode::InvalidRequest, "response format schema must be an object"));
        }
        if (auto r = checkPortableSchema(request.responseFormat->schema); !r) {
            return std::unexpected(AiError(AiErrorCode::InvalidRequest, "response format " + r.error().message));
        }
    }
    if (request.messages.empty()) {
        return std::unexpected(AiError(AiErrorCode::InvalidRequest, "no messages"));
    }
    if (request.maxOutputTokens <= 0) {
        return std::unexpected(AiError(AiErrorCode::InvalidRequest, "maxOutputTokens must be positive"));
    }
    return {};
}

std::expected<Json, AiError> parseToolArguments(std::string_view text) {
    if (text.find_first_not_of(" \t\r\n") == std::string_view::npos) {
        return Json::object(); // tools without parameters stream no arguments at all
    }
    auto j = Json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        return std::unexpected(AiError(AiErrorCode::InvalidRequest, "tool arguments are not a JSON object"));
    }
    return j;
}

} // namespace wabcut::ai
