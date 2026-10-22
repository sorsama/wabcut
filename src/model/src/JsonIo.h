#pragma once

// The model's JSON reading kit, shared by ProjectIO.cpp and GraphicJson.cpp (internal to wabcut_model): a Reader that
// walks untrusted JSON with the path of every value, throwing FormatError (turned into ParseError by the public entry
// points), and the readers and writers of the values several formats share.

#include <wabcut/model/Effects.h>
#include <wabcut/model/Graphics.h>
#include <wabcut/model/Masks.h>
#include <wabcut/model/Parameters.h>
#include <wabcut/model/Time.h>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>

namespace wabcut::io::detail {

using json = nlohmann::json;

template <class E>
struct EnumName {
    E value;
    const char* name;
};

/// Thrown inside the model's JSON readers only; the public entry points turn it into an Error.
struct FormatError {
    std::string message;
};

class Reader {
public:
    Reader(const json& value, std::string path) : m_value(value), m_path(std::move(path)) {}

    const json& value() const { return m_value; }
    const std::string& path() const { return m_path; }

    [[noreturn]] void fail(const std::string& what) const {
        throw FormatError{(m_path.empty() ? std::string("/") : m_path) + ": " + what};
    }

    bool has(const char* key) const { return m_value.is_object() && m_value.contains(key) && !m_value.at(key).is_null(); }

    Reader child(const char* key) const {
        if (!m_value.is_object() || !m_value.contains(key)) {
            fail(std::string("missing \"") + key + "\"");
        }
        return {m_value.at(key), m_path + "/" + key};
    }

    Reader element(std::size_t i) const { return {m_value.at(i), m_path + "/" + std::to_string(i)}; }

    std::size_t arraySize() const {
        if (!m_value.is_array()) {
            fail("expected an array");
        }
        return m_value.size();
    }

    std::int64_t asInt64() const {
        if (!m_value.is_number_integer()) {
            fail("expected an integer");
        }
        if (m_value.is_number_unsigned() &&
            m_value.get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            fail("integer out of range");
        }
        return m_value.get<std::int64_t>();
    }

    std::int32_t asInt32() const {
        const std::int64_t v = asInt64();
        if (v < std::numeric_limits<std::int32_t>::min() || v > std::numeric_limits<std::int32_t>::max()) {
            fail("integer out of range");
        }
        return static_cast<std::int32_t>(v);
    }

    std::uint64_t asUInt64() const {
        if (m_value.is_number_unsigned()) {
            return m_value.get<std::uint64_t>();
        }
        // Parsed text gives unsigned numbers; values a migration wrote are signed.
        if (m_value.is_number_integer() && m_value.get<std::int64_t>() >= 0) {
            return static_cast<std::uint64_t>(m_value.get<std::int64_t>());
        }
        fail("expected a non-negative integer");
    }

    double asDouble() const {
        if (!m_value.is_number()) {
            fail("expected a number");
        }
        return m_value.get<double>();
    }

    bool asBool() const {
        if (!m_value.is_boolean()) {
            fail("expected true or false");
        }
        return m_value.get<bool>();
    }

    std::string asString() const {
        if (!m_value.is_string()) {
            fail("expected a string");
        }
        return m_value.get<std::string>();
    }

    // Field helpers: required, or optional with a default; ranged ones reject out-of-domain values.
    std::int64_t int64(const char* key) const { return child(key).asInt64(); }
    std::int64_t int64(const char* key, std::int64_t fallback) const { return has(key) ? int64(key) : fallback; }
    std::int64_t int64In(const char* key, std::int64_t fallback, std::int64_t lo, std::int64_t hi) const {
        const std::int64_t v = int64(key, fallback);
        if (v < lo || v > hi) {
            child(key).fail("value out of range");
        }
        return v;
    }
    std::int32_t int32(const char* key) const { return child(key).asInt32(); }
    std::int32_t int32(const char* key, std::int32_t fallback) const { return has(key) ? int32(key) : fallback; }
    std::int32_t int32In(const char* key, std::int32_t fallback, std::int32_t lo, std::int32_t hi) const {
        return static_cast<std::int32_t>(int64In(key, fallback, lo, hi));
    }
    std::uint64_t uint64(const char* key) const { return child(key).asUInt64(); }
    std::uint64_t uint64(const char* key, std::uint64_t fallback) const { return has(key) ? uint64(key) : fallback; }
    Ticks ticks(const char* key) const { return {int64(key)}; }
    /// A duration or position that may not be negative.
    Ticks nonNegativeTicks(const char* key, Ticks fallback) const {
        const Ticks t = has(key) ? ticks(key) : fallback;
        if (t.value < 0) {
            child(key).fail("must not be negative");
        }
        return t;
    }
    std::optional<Ticks> optionalTicks(const char* key) const {
        return has(key) ? std::optional<Ticks>(ticks(key)) : std::nullopt;
    }
    bool boolean(const char* key, bool fallback) const { return has(key) ? child(key).asBool() : fallback; }
    std::string string(const char* key, std::string fallback = {}) const {
        return has(key) ? child(key).asString() : std::move(fallback);
    }
    /// A nominal rate: both parts positive.
    Rate rate(const char* key) const {
        const Reader r = child(key);
        const Rate rate{r.int32("num"), r.int32("den")};
        if (!rate.isValid()) {
            r.fail("a rate needs a positive numerator and denominator");
        }
        return rate;
    }
    /// A probed rate: numerator 0 means unknown.
    Rate probedRate(const char* key, Rate fallback) const {
        if (!has(key)) {
            return fallback;
        }
        const Reader r = child(key);
        const Rate rate{r.int32("num"), r.int32("den")};
        if (rate.num < 0 || rate.den <= 0) {
            r.fail("invalid rate");
        }
        return rate;
    }

    template <class E, std::size_t N>
    E enumeration(const char* key, const EnumName<E> (&table)[N]) const {
        if (!has(key)) {
            return table[0].value;
        }
        const std::string name = child(key).asString();
        for (const auto& entry : table) {
            if (name == entry.name) {
                return entry.value;
            }
        }
        child(key).fail("unknown value \"" + name + "\"");
    }

private:
    const json& m_value;
    std::string m_path;
};

/// {"value": x} or {"keys": [...]} (ProjectIO.h, schema 3), and back.
json paramValueToJson(const ParamValue& value);
ParamValue paramValueFromJson(const Reader& reader);
/// A mask (Masks.h, I.6), and back.
json maskToJson(const Mask& mask);
Mask maskFromJson(const Reader& reader);
/// An effect stack as a clip's "effects" (every instance as it is), and back (shape only: the caller bounds the count and
/// checks the values, effectStackProblem). Effect presets (EffectPresets.cpp) use them.
json effectStackToJsonValue(const EffectStack& stack);
EffectStack effectStackFromJsonValue(const Reader& reader);

/// Graphics (GraphicJson.cpp, C.13): a clip's graphic, the brand kit and a template as project files hold them. The
/// readers check the result (graphicProblem, ...) and fail through the Reader.
json graphicToJsonValue(const Graphic& graphic);
Graphic graphicFromJsonValue(const Reader& reader);
json brandKitToJsonValue(const BrandKit& kit);
BrandKit brandKitFromJsonValue(const Reader& reader);
json templateToJsonValue(const GraphicTemplate& tmpl);
GraphicTemplate templateFromJsonValue(const Reader& reader);
/// 0.2: a project text style ({"name", "text": {"runs": [style], "align", "background"}, "shadow"}).
json textStyleToJsonValue(const ProjectTextStyle& style);
ProjectTextStyle textStyleFromJsonValue(const Reader& reader);

} // namespace wabcut::io::detail
