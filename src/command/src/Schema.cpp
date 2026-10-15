#include <wabcut/command/Schema.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <set>
#include <string_view>

namespace wabcut::command {

namespace {

constexpr std::array<std::string_view, 21> kKeywords{
    "type",       "properties", "required",         "additionalProperties", "items",    "enum",
    "const",      "minimum",    "maximum",          "exclusiveMinimum",     "exclusiveMaximum",
    "minLength",  "maxLength",  "minItems",         "maxItems",             "title",    "description",
    "default",    "examples",   "$schema",          "$comment",
};

bool isKnownKeyword(std::string_view k) { return std::find(kKeywords.begin(), kKeywords.end(), k) != kKeywords.end(); }

std::string typeOf(const Json& v) {
    switch (v.type()) {
    case Json::value_t::object:
        return "object";
    case Json::value_t::array:
        return "array";
    case Json::value_t::string:
        return "string";
    case Json::value_t::boolean:
        return "boolean";
    case Json::value_t::null:
        return "null";
    case Json::value_t::number_integer:
    case Json::value_t::number_unsigned:
        return "integer";
    case Json::value_t::number_float:
        return "number";
    default:
        return "unknown";
    }
}

bool isIntegral(const Json& v) {
    if (v.is_number_integer()) {
        return true;
    }
    if (v.is_number_float()) {
        const double d = v.get<double>();
        return std::isfinite(d) && std::floor(d) == d;
    }
    return false;
}

bool matchesType(const Json& value, std::string_view type) {
    if (type == "integer") {
        return isIntegral(value);
    }
    if (type == "number") {
        return value.is_number();
    }
    return typeOf(value) == type;
}

std::string escapePointer(const std::string& key) {
    std::string out;
    for (const char c : key) {
        if (c == '~') {
            out += "~0";
        } else if (c == '/') {
            out += "~1";
        } else {
            out += c;
        }
    }
    return out;
}

std::string typeList(const Json& type) {
    if (type.is_string()) {
        return type.get<std::string>();
    }
    std::string out;
    for (const Json& t : type) {
        out += (out.empty() ? "" : " or ") + t.get<std::string>();
    }
    return out;
}

double asDouble(const Json& v) { return v.is_number_unsigned() ? static_cast<double>(v.get<std::uint64_t>()) : v.get<double>(); }

/// Compares a number against a bound without losing precision for integers.
int compareNumbers(const Json& value, const Json& bound) {
    if (value.is_number_integer() && bound.is_number_integer()) {
        // Both integral: compare as signed/unsigned 64-bit.
        if (value.is_number_unsigned() || bound.is_number_unsigned()) {
            const bool valueNeg = !value.is_number_unsigned() && value.get<std::int64_t>() < 0;
            const bool boundNeg = !bound.is_number_unsigned() && bound.get<std::int64_t>() < 0;
            if (valueNeg != boundNeg) {
                return valueNeg ? -1 : 1;
            }
            const auto a = value.get<std::uint64_t>();
            const auto b = bound.get<std::uint64_t>();
            return a < b ? -1 : (a > b ? 1 : 0);
        }
        const auto a = value.get<std::int64_t>();
        const auto b = bound.get<std::int64_t>();
        return a < b ? -1 : (a > b ? 1 : 0);
    }
    const double a = asDouble(value);
    const double b = asDouble(bound);
    return a < b ? -1 : (a > b ? 1 : 0);
}

class Validator {
public:
    std::vector<SchemaIssue> issues;

    void check(const Json& schema, const Json& value, const std::string& path) {
        if (!schema.is_object()) {
            return; // `true`-like schema
        }
        if (schema.contains("type")) {
            const Json& type = schema["type"];
            bool ok = false;
            if (type.is_string()) {
                ok = matchesType(value, type.get<std::string>());
            } else {
                for (const Json& t : type) {
                    ok = ok || matchesType(value, t.get<std::string>());
                }
            }
            if (!ok) {
                add(path, "expected " + typeList(type) + ", got " + typeOf(value));
                return; // the other keywords would only repeat the problem
            }
        }
        if (schema.contains("enum")) {
            const Json& values = schema["enum"];
            if (std::find(values.begin(), values.end(), value) == values.end()) {
                add(path, "must be one of " + values.dump());
            }
        }
        if (schema.contains("const") && schema["const"] != value) {
            add(path, "must be " + schema["const"].dump());
        }
        if (value.is_number()) {
            checkNumber(schema, value, path);
        }
        if (value.is_string()) {
            const auto length = static_cast<std::int64_t>(value.get<std::string>().size());
            if (schema.contains("minLength") && length < schema["minLength"].get<std::int64_t>()) {
                add(path, "must be at least " + schema["minLength"].dump() + " characters long");
            }
            if (schema.contains("maxLength") && length > schema["maxLength"].get<std::int64_t>()) {
                add(path, "must be at most " + schema["maxLength"].dump() + " characters long");
            }
        }
        if (value.is_array()) {
            checkArray(schema, value, path);
        }
        if (value.is_object()) {
            checkObject(schema, value, path);
        }
    }

private:
    void add(const std::string& path, std::string message) { issues.push_back({path.empty() ? "/" : path, std::move(message)}); }

    void checkNumber(const Json& schema, const Json& value, const std::string& path) {
        if (schema.contains("minimum") && compareNumbers(value, schema["minimum"]) < 0) {
            add(path, "must be at least " + schema["minimum"].dump());
        }
        if (schema.contains("maximum") && compareNumbers(value, schema["maximum"]) > 0) {
            add(path, "must be at most " + schema["maximum"].dump());
        }
        if (schema.contains("exclusiveMinimum") && compareNumbers(value, schema["exclusiveMinimum"]) <= 0) {
            add(path, "must be greater than " + schema["exclusiveMinimum"].dump());
        }
        if (schema.contains("exclusiveMaximum") && compareNumbers(value, schema["exclusiveMaximum"]) >= 0) {
            add(path, "must be less than " + schema["exclusiveMaximum"].dump());
        }
    }

    void checkArray(const Json& schema, const Json& value, const std::string& path) {
        const auto size = static_cast<std::int64_t>(value.size());
        if (schema.contains("minItems") && size < schema["minItems"].get<std::int64_t>()) {
            add(path, "needs at least " + schema["minItems"].dump() + " items");
        }
        if (schema.contains("maxItems") && size > schema["maxItems"].get<std::int64_t>()) {
            add(path, "allows at most " + schema["maxItems"].dump() + " items");
        }
        if (schema.contains("items")) {
            for (std::size_t i = 0; i < value.size(); ++i) {
                check(schema["items"], value[i], path + "/" + std::to_string(i));
            }
        }
    }

    void checkObject(const Json& schema, const Json& value, const std::string& path) {
        if (schema.contains("required")) {
            for (const Json& key : schema["required"]) {
                if (!value.contains(key.get<std::string>())) {
                    add(path, "is missing required \"" + key.get<std::string>() + "\"");
                }
            }
        }
        const Json* properties = schema.contains("properties") ? &schema["properties"] : nullptr;
        const bool closed = schema.contains("additionalProperties") && schema["additionalProperties"].is_boolean() &&
                            !schema["additionalProperties"].get<bool>();
        for (const auto& item : value.items()) {
            const std::string childPath = path + "/" + escapePointer(item.key());
            if (properties && properties->contains(item.key())) {
                check((*properties)[item.key()], item.value(), childPath);
            } else if (closed) {
                add(childPath, "is not a known parameter");
            }
        }
    }
};

void collectUnsupported(const Json& schema, std::set<std::string>& out) {
    if (!schema.is_object()) {
        return;
    }
    for (const auto& item : schema.items()) {
        if (!isKnownKeyword(item.key())) {
            out.insert(item.key());
        }
    }
    if (schema.contains("properties") && schema["properties"].is_object()) {
        for (const auto& item : schema["properties"].items()) {
            collectUnsupported(item.value(), out);
        }
    }
    if (schema.contains("items")) {
        collectUnsupported(schema["items"], out);
    }
}

} // namespace

std::vector<SchemaIssue> validateAgainstSchema(const Json& schema, const Json& value) {
    Validator v;
    v.check(schema, value, "");
    return std::move(v.issues);
}

std::vector<std::string> unsupportedSchemaKeywords(const Json& schema) {
    std::set<std::string> found;
    collectUnsupported(schema, found);
    return {found.begin(), found.end()};
}

namespace schema {

Json object(std::initializer_list<std::pair<const char*, Json>> properties, std::vector<std::string> required,
            std::string description) {
    Json props = Json::object();
    for (const auto& [name, s] : properties) {
        props[name] = s;
    }
    Json out{{"type", "object"}, {"properties", std::move(props)}, {"additionalProperties", false}};
    if (!required.empty()) {
        out["required"] = std::move(required);
    }
    if (!description.empty()) {
        out["description"] = std::move(description);
    }
    return out;
}

Json string(std::string description) { return Json{{"type", "string"}, {"description", std::move(description)}}; }

Json integer(std::string description, std::optional<std::int64_t> minimum, std::optional<std::int64_t> maximum) {
    Json out{{"type", "integer"}, {"description", std::move(description)}};
    if (minimum) {
        out["minimum"] = *minimum;
    }
    if (maximum) {
        out["maximum"] = *maximum;
    }
    return out;
}

Json number(std::string description, std::optional<double> minimum, std::optional<double> maximum) {
    Json out{{"type", "number"}, {"description", std::move(description)}};
    if (minimum) {
        out["minimum"] = *minimum;
    }
    if (maximum) {
        out["maximum"] = *maximum;
    }
    return out;
}

Json boolean(std::string description, std::optional<bool> defaultValue) {
    Json out{{"type", "boolean"}, {"description", std::move(description)}};
    if (defaultValue) {
        out["default"] = *defaultValue;
    }
    return out;
}

Json array(Json items, std::string description, std::optional<std::int64_t> minItems) {
    Json out{{"type", "array"}, {"items", std::move(items)}, {"description", std::move(description)}};
    if (minItems) {
        out["minItems"] = *minItems;
    }
    return out;
}

Json enumeration(std::vector<std::string> values, std::string description) {
    return Json{{"type", "string"}, {"enum", std::move(values)}, {"description", std::move(description)}};
}

Json described(Json s, std::string description) {
    s["description"] = std::move(description);
    return s;
}

} // namespace schema

} // namespace wabcut::command
