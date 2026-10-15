#include <wabcut/ai/JsonAccess.h>

#include <cmath>
#include <limits>

namespace wabcut::ai {

const Json* jsonMember(const Json& j, std::string_view key) {
    if (!j.is_object()) {
        return nullptr;
    }
    const auto it = j.find(key);
    return it == j.end() ? nullptr : &*it;
}

const Json* jsonObject(const Json& j, std::string_view key) {
    const Json* m = jsonMember(j, key);
    return m && m->is_object() ? m : nullptr;
}

const Json* jsonArray(const Json& j, std::string_view key) {
    const Json* m = jsonMember(j, key);
    return m && m->is_array() ? m : nullptr;
}

const Json* jsonAt(const Json& j, std::initializer_list<std::string_view> path) {
    const Json* cur = &j;
    for (const std::string_view key : path) {
        cur = jsonMember(*cur, key);
        if (!cur) {
            return nullptr;
        }
    }
    return cur;
}

std::optional<std::string> asString(const Json* j) {
    if (!j || !j->is_string()) {
        return std::nullopt;
    }
    return j->get_ref<const std::string&>();
}

std::optional<std::int64_t> asInt(const Json* j) {
    if (!j) {
        return std::nullopt;
    }
    if (j->is_number_integer() && !j->is_number_unsigned()) {
        return j->get<std::int64_t>();
    }
    if (j->is_number_unsigned()) {
        const auto u = j->get<std::uint64_t>();
        if (u > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return std::nullopt;
        }
        return static_cast<std::int64_t>(u);
    }
    if (j->is_number_float()) {
        const double d = j->get<double>();
        // 2^63 is exactly representable; anything at or beyond it does not fit.
        if (!std::isfinite(d) || d != std::floor(d) || d < -9223372036854775808.0 || d >= 9223372036854775808.0) {
            return std::nullopt;
        }
        return static_cast<std::int64_t>(d);
    }
    return std::nullopt;
}

std::optional<double> asNumber(const Json* j) {
    if (!j || !j->is_number()) {
        return std::nullopt;
    }
    const double d = j->get<double>();
    return std::isfinite(d) ? std::optional<double>(d) : std::nullopt;
}

std::optional<std::string> jsonString(const Json& j, std::string_view key) { return asString(jsonMember(j, key)); }

std::string jsonStringOr(const Json& j, std::string_view key, std::string fallback) {
    auto s = jsonString(j, key);
    return s ? std::move(*s) : std::move(fallback);
}

std::optional<std::int64_t> jsonInt(const Json& j, std::string_view key) { return asInt(jsonMember(j, key)); }

std::int64_t jsonIntOr(const Json& j, std::string_view key, std::int64_t fallback) {
    return jsonInt(j, key).value_or(fallback);
}

std::optional<double> jsonNumber(const Json& j, std::string_view key) { return asNumber(jsonMember(j, key)); }

double jsonNumberOr(const Json& j, std::string_view key, double fallback) { return jsonNumber(j, key).value_or(fallback); }

std::optional<bool> jsonBool(const Json& j, std::string_view key) {
    const Json* m = jsonMember(j, key);
    if (!m || !m->is_boolean()) {
        return std::nullopt;
    }
    return m->get<bool>();
}

bool jsonBoolOr(const Json& j, std::string_view key, bool fallback) { return jsonBool(j, key).value_or(fallback); }

std::string jsonIdOr(const Json& j, std::string_view key) {
    const Json* m = jsonMember(j, key);
    if (!m) {
        return {};
    }
    if (m->is_string()) {
        return m->get_ref<const std::string&>();
    }
    if (const auto i = asInt(m)) {
        return std::to_string(*i);
    }
    return {};
}

} // namespace wabcut::ai
