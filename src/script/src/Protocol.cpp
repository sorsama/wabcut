#include <wabcut/script/Protocol.h>

#include <wabcut/model/ProjectIO.h>

#include <algorithm>
#include <cmath>
#include <set>

namespace wabcut::script {

namespace {

constexpr std::size_t kMaxTitleBytes = 100;
constexpr std::size_t kMaxFormMessageBytes = 2000;
constexpr std::size_t kMaxButtonBytes = 40;
constexpr std::size_t kMaxTextFieldBytes = 20000;
constexpr std::size_t kMaxChoiceOptions = 50;
constexpr std::size_t kMaxErrorBytes = 4000;

using Fail = std::unexpected<std::string>;

/// Display text from the helper: cut to `maxBytes` at a character boundary, and every character isDisplayText()
/// refuses replaced by U+FFFD, so nothing an extension writes can pose as editor text.
std::string sanitized(std::string_view s, std::size_t maxBytes, bool multiline) {
    std::string out;
    for (std::size_t i = 0; i < s.size();) {
        const auto b = static_cast<unsigned char>(s[i]);
        const std::size_t len = b < 0x80 ? 1 : b < 0xE0 ? 2 : b < 0xF0 ? 3 : 4;
        const std::string_view ch = s.substr(i, len);
        const std::string_view piece = isDisplayText(ch, multiline) ? ch : std::string_view("\xEF\xBF\xBD");
        if (out.size() + piece.size() > maxBytes) {
            break; // the bound holds for the text as it is sent on (a replacement is 3 bytes)
        }
        out += piece;
        i += len;
    }
    return out;
}

/// A non-negative integer of at most `max` (JSON text gives unsigned numbers, C++ literals signed ones).
bool isCount(const Json& j, std::uint64_t max) {
    if (j.is_number_unsigned()) {
        return j.get<std::uint64_t>() <= max;
    }
    return j.is_number_integer() && j.get<std::int64_t>() >= 0 && static_cast<std::uint64_t>(j.get<std::int64_t>()) <= max;
}

bool isId(const Json& j) { return isCount(j, kMaxMessageId); }

/// A required string member of 1..maxBytes bytes; empty optional (and `error` set) otherwise.
std::optional<std::string> member(const Json& obj, const char* key, std::size_t maxBytes, std::string& error,
                                  bool required = true) {
    if (!obj.contains(key)) {
        if (required) {
            error = std::string("\"") + key + "\" is missing";
        }
        return std::nullopt;
    }
    const Json& v = obj.at(key);
    if (!v.is_string() || v.get_ref<const std::string&>().empty() || v.get_ref<const std::string&>().size() > maxBytes) {
        error = std::string("\"") + key + "\" must be a string of 1 to " + std::to_string(maxBytes) + " bytes";
        return std::nullopt;
    }
    return v.get<std::string>();
}

std::expected<Json, std::string> checkField(const Json& f, std::set<std::string>& ids) {
    if (!f.is_object()) {
        return Fail("a field must be an object");
    }
    std::string error;
    const auto id = member(f, "id", 64, error);
    const auto type = member(f, "type", 16, error);
    const auto label = member(f, "label", kMaxTitleBytes, error);
    if (!id || !type || !label) {
        return Fail(error);
    }
    if (!isValidCommandId(*id) || !ids.insert(*id).second) {
        return Fail("field id \"" + id->substr(0, 64) + "\" is not a valid, unique id");
    }
    Json out{{"id", *id}, {"type", *type}, {"label", sanitized(*label, kMaxTitleBytes, false)}};
    if (*type == "text") {
        const bool readOnly = f.value("readOnly", false);
        const Json& maxJ = f.contains("maxLength") ? f.at("maxLength") : Json(readOnly ? kMaxTextFieldBytes : 1000);
        if (!isCount(maxJ, kMaxTextFieldBytes) || maxJ.get<std::uint64_t>() < 1) {
            return Fail("\"maxLength\" must be 1 to " + std::to_string(kMaxTextFieldBytes));
        }
        const auto maxLength = maxJ.get<std::size_t>();
        const Json value = f.value("value", Json(""));
        if (!value.is_string() || value.get_ref<const std::string&>().size() > maxLength) {
            return Fail("the value of \"" + *id + "\" must be text of at most maxLength bytes");
        }
        out["value"] = sanitized(value.get_ref<const std::string&>(), maxLength, true);
        out["maxLength"] = maxLength;
        out["multiline"] = f.value("multiline", false);
        out["readOnly"] = readOnly;
        if (f.contains("placeholder")) {
            const auto placeholder = member(f, "placeholder", kMaxTitleBytes, error);
            if (!placeholder) {
                return Fail(error);
            }
            out["placeholder"] = sanitized(*placeholder, kMaxTitleBytes, false);
        }
    } else if (*type == "number") {
        for (const char* key : {"min", "max", "step", "value"}) {
            if (f.contains(key) && (!f.at(key).is_number() || !std::isfinite(f.at(key).get<double>()))) {
                return Fail(std::string("\"") + key + "\" must be a finite number");
            }
        }
        const double min = f.value("min", -1e15);
        const double max = f.value("max", 1e15);
        const double value = min <= max ? f.value("value", std::clamp(0.0, min, max)) : 0.0;
        if (min > max || value < min || value > max || (f.contains("step") && f.at("step").get<double>() <= 0)) {
            return Fail("the number field \"" + *id + "\" needs min <= value <= max and a positive step");
        }
        out["min"] = min;
        out["max"] = max;
        out["value"] = value;
        if (f.contains("step")) {
            out["step"] = f.at("step").get<double>();
        }
    } else if (*type == "checkbox") {
        const Json value = f.value("value", Json(false));
        if (!value.is_boolean()) {
            return Fail("the value of \"" + *id + "\" must be true or false");
        }
        out["value"] = value;
    } else if (*type == "choice") {
        if (!f.contains("options") || !f.at("options").is_array() || f.at("options").empty() ||
            f.at("options").size() > kMaxChoiceOptions) {
            return Fail("the choice \"" + *id + "\" needs 1 to " + std::to_string(kMaxChoiceOptions) + " options");
        }
        Json options = Json::array();
        std::set<std::string> values;
        for (const Json& o : f.at("options")) {
            const auto v = o.is_object() ? member(o, "value", kMaxTitleBytes, error) : std::nullopt;
            const auto l = o.is_object() ? member(o, "label", kMaxTitleBytes, error) : std::nullopt;
            if (!v || !l || !values.insert(*v).second) {
                return Fail("each option of \"" + *id + "\" needs a unique \"value\" and a \"label\"");
            }
            options.push_back({{"value", *v}, {"label", sanitized(*l, kMaxTitleBytes, false)}});
        }
        const Json value = f.value("value", options.front()["value"]);
        if (!value.is_string() || !values.contains(value.get<std::string>())) {
            return Fail("the value of \"" + *id + "\" must be one of its options");
        }
        out["options"] = options;
        out["value"] = value;
    } else {
        return Fail("field type \"" + type->substr(0, 16) + "\" is not text, number, checkbox or choice");
    }
    return out;
}

std::expected<Json, std::string> checkHost(HostMessageType type, const Json& m) {
    std::string error;
    switch (type) {
    case HostMessageType::Ready:
        return m;
    case HostMessageType::LoadFailed: {
        const auto message = member(m, "message", kMaxErrorBytes * 4, error);
        if (!message) {
            return Fail(error);
        }
        return Json{{"type", "loadFailed"}, {"message", sanitized(*message, kMaxErrorBytes, true)}};
    }
    case HostMessageType::Request: {
        static const std::set<std::string, std::less<>> kMethods{"commands.run", "commands.list", "commands.describe"};
        const auto method = member(m, "method", 32, error);
        if (!m.contains("id") || !isId(m.at("id")) || !method || !kMethods.contains(*method)) {
            return Fail("a request needs an id and one of the methods commands.run, commands.list, commands.describe");
        }
        const Json params = m.value("params", Json::object());
        if (!params.is_object()) {
            return Fail("request params must be an object");
        }
        if (*method == "commands.run" && (!params.contains("document") || !params.at("document").is_object())) {
            return Fail("commands.run needs a \"document\" object");
        }
        if (*method == "commands.describe" && !member(params, "name", 100, error)) {
            return Fail(error);
        }
        return Json{{"type", "request"}, {"id", m.at("id")}, {"method", *method}, {"params", params}};
    }
    case HostMessageType::Done: {
        if (!m.contains("call") || !isId(m.at("call"))) {
            return Fail("done needs a call number");
        }
        Json out{{"type", "done"}, {"call", m.at("call")}};
        if (m.contains("error")) {
            const Json& e = m.at("error");
            static const std::set<std::string, std::less<>> kKinds{"exception", "timeLimit", "cancelled", "result"};
            const auto kind = e.is_object() ? member(e, "kind", 16, error) : std::nullopt;
            const auto message = e.is_object() ? member(e, "message", kMaxErrorBytes * 4, error) : std::nullopt;
            if (!kind || !kKinds.contains(*kind) || !message) {
                return Fail("a failed call needs an error {kind, message}");
            }
            out["error"] = {{"kind", *kind}, {"message", sanitized(*message, kMaxErrorBytes, true)}};
        } else {
            out["result"] = m.value("result", Json());
        }
        return out;
    }
    case HostMessageType::Notify: {
        const std::string level = m.value("level", "info");
        const auto message = member(m, "message", kMaxNotifyBytes, error);
        if (!message || (level != "info" && level != "warning" && level != "error")) {
            return Fail(message ? "notify level must be info, warning or error" : error);
        }
        return Json{{"type", "notify"}, {"level", level}, {"message", sanitized(*message, kMaxNotifyBytes, true)}};
    }
    case HostMessageType::Toast: {
        const auto message = member(m, "message", kMaxToastBytes, error);
        if (!message) {
            return Fail(error);
        }
        return Json{{"type", "toast"}, {"message", sanitized(*message, kMaxToastBytes, false)}};
    }
    case HostMessageType::Prompt: {
        if (!m.contains("prompt") || !isId(m.at("prompt")) || !m.contains("form")) {
            return Fail("a prompt needs a prompt number and a form");
        }
        auto form = validatePromptForm(m.at("form"));
        if (!form) {
            return Fail("prompt form: " + form.error());
        }
        return Json{{"type", "prompt"}, {"prompt", m.at("prompt")}, {"form", std::move(*form)}};
    }
    case HostMessageType::RegisterCommand: {
        const auto id = member(m, "id", 64, error);
        const auto title = member(m, "title", kMaxTitleBytes, error);
        if (!id || !title || !isValidCommandId(*id)) {
            return Fail(id && title ? "command ids match [A-Za-z][A-Za-z0-9._-]*" : error);
        }
        return Json{{"type", "registerCommand"}, {"id", *id}, {"title", sanitized(*title, kMaxTitleBytes, false)}};
    }
    case HostMessageType::Subscribe: {
        const auto event = member(m, "event", 32, error);
        if (!event || !eventKindFromString(*event)) {
            return Fail("subscribe needs an event: selectionChanged, playheadMoved, projectSaved or commandApplied");
        }
        const Json on = m.value("on", Json(true));
        if (!on.is_boolean()) {
            return Fail("\"on\" must be true or false");
        }
        return Json{{"type", "subscribe"}, {"event", *event}, {"on", on}};
    }
    case HostMessageType::StorageSet:
    case HostMessageType::StorageRemove: {
        const auto key = member(m, "key", 1024, error);
        if (!key) {
            return Fail(error);
        }
        if (type == HostMessageType::StorageRemove) {
            return Json{{"type", "storageRemove"}, {"key", *key}};
        }
        if (!m.contains("value")) {
            return Fail("storageSet needs a value");
        }
        return Json{{"type", "storageSet"}, {"key", *key}, {"value", m.at("value")}};
    }
    case HostMessageType::Log: {
        const std::string level = m.value("level", "log");
        const auto message = member(m, "message", kMaxLogBytes * 4, error);
        if (!message) {
            return Fail(error);
        }
        static const std::set<std::string, std::less<>> kLevels{"log", "debug", "info", "warn", "error"};
        return Json{{"type", "log"},
                    {"level", kLevels.contains(level) ? level : "log"},
                    {"message", sanitized(*message, kMaxLogBytes, true)}};
    }
    case HostMessageType::Pong:
        if (!m.contains("id") || !isId(m.at("id"))) {
            return Fail("pong needs an id");
        }
        return Json{{"type", "pong"}, {"id", m.at("id")}};
    }
    return Fail("unknown message");
}

std::expected<Json, std::string> checkForm(const Json& form);

} // namespace

std::expected<HostMessage, std::string> parseHostMessage(std::string_view line) {
    if (line.size() > kMaxHostMessageBytes) {
        return Fail("a message is larger than " + std::to_string(kMaxHostMessageBytes / 1024) + " KiB");
    }
    if (!io::jsonNestingWithin(line)) {
        return Fail("a message nests too deep");
    }
    const Json m = Json::parse(line, nullptr, false);
    if (!m.is_object() || !m.contains("type") || !m.at("type").is_string()) {
        return Fail("a message must be a JSON object with a \"type\"");
    }
    static const std::pair<std::string_view, HostMessageType> kTypes[] = {
        {"ready", HostMessageType::Ready},
        {"loadFailed", HostMessageType::LoadFailed},
        {"request", HostMessageType::Request},
        {"done", HostMessageType::Done},
        {"notify", HostMessageType::Notify},
        {"toast", HostMessageType::Toast},
        {"prompt", HostMessageType::Prompt},
        {"registerCommand", HostMessageType::RegisterCommand},
        {"subscribe", HostMessageType::Subscribe},
        {"storageSet", HostMessageType::StorageSet},
        {"storageRemove", HostMessageType::StorageRemove},
        {"log", HostMessageType::Log},
        {"pong", HostMessageType::Pong},
    };
    const std::string& name = m.at("type").get_ref<const std::string&>();
    for (const auto& [text, type] : kTypes) {
        if (text == name) {
            std::expected<Json, std::string> body = Fail("");
            try {
                body = checkHost(type, m); // value() throws on a member of another type
            } catch (const Json::exception&) {
                body = Fail("a member has the wrong type");
            }
            if (!body) {
                return Fail(std::string(text) + ": " + body.error());
            }
            return HostMessage{type, std::move(*body)};
        }
    }
    return Fail("unknown message type \"" + name.substr(0, 32) + "\"");
}

std::expected<Json, std::string> validatePromptForm(const Json& form) {
    try {
        return checkForm(form);
    } catch (const Json::exception&) {
        return Fail("a form member has the wrong type");
    }
}

namespace {

std::expected<Json, std::string> checkForm(const Json& form) {
    if (!form.is_object()) {
        return Fail("a form must be an object");
    }
    if (form.dump().size() > kMaxPromptFormBytes) {
        return Fail("a form may be at most " + std::to_string(kMaxPromptFormBytes / 1024) + " KiB");
    }
    std::string error;
    const auto title = member(form, "title", kMaxTitleBytes, error);
    if (!title) {
        return Fail(error);
    }
    Json out{{"title", sanitized(*title, kMaxTitleBytes, false)},
             {"submitLabel", "OK"},
             {"cancelLabel", "Cancel"},
             {"fields", Json::array()}};
    if (form.contains("message")) {
        const auto message = member(form, "message", kMaxFormMessageBytes, error);
        if (!message) {
            return Fail(error);
        }
        out["message"] = sanitized(*message, kMaxFormMessageBytes, true);
    }
    for (const char* key : {"submitLabel", "cancelLabel"}) {
        if (form.contains(key)) {
            const auto label = member(form, key, kMaxButtonBytes, error);
            if (!label) {
                return Fail(error);
            }
            out[key] = sanitized(*label, kMaxButtonBytes, false);
        }
    }
    const Json fields = form.value("fields", Json::array());
    if (!fields.is_array() || fields.size() > kMaxPromptFields) {
        return Fail("\"fields\" must be an array of at most " + std::to_string(kMaxPromptFields) + " fields");
    }
    std::set<std::string> ids;
    for (const Json& f : fields) {
        auto field = checkField(f, ids);
        if (!field) {
            return Fail(field.error());
        }
        out["fields"].push_back(std::move(*field));
    }
    return out;
}

} // namespace

std::expected<Json, std::string> validatePromptAnswer(const Json& form, const Json& values) {
    if (!values.is_object()) {
        return Fail("answers must be an object of field values");
    }
    Json out = Json::object();
    std::set<std::string> known;
    for (const Json& f : form.at("fields")) {
        const std::string id = f.at("id").get<std::string>();
        known.insert(id);
        const std::string type = f.at("type").get<std::string>();
        if (!values.contains(id) || (type == "text" && f.at("readOnly").get<bool>())) {
            out[id] = f.at("value");
            continue;
        }
        const Json& v = values.at(id);
        if (type == "text") {
            if (!v.is_string() || v.get_ref<const std::string&>().size() > f.at("maxLength").get<std::size_t>()) {
                return Fail("\"" + id + "\" must be text of at most " + f.at("maxLength").dump() + " bytes");
            }
        } else if (type == "number") {
            if (!v.is_number() || !std::isfinite(v.get<double>()) || v.get<double>() < f.at("min").get<double>() ||
                v.get<double>() > f.at("max").get<double>()) {
                return Fail("\"" + id + "\" must be a number from " + f.at("min").dump() + " to " + f.at("max").dump());
            }
        } else if (type == "checkbox") {
            if (!v.is_boolean()) {
                return Fail("\"" + id + "\" must be true or false");
            }
        } else if (type == "choice") {
            const Json& options = f.at("options");
            if (!v.is_string() || std::none_of(options.begin(), options.end(),
                                               [&](const Json& o) { return o.at("value") == v; })) {
                return Fail("\"" + id + "\" must be one of its options");
            }
        }
        out[id] = v;
    }
    for (const auto& [key, value] : values.items()) {
        if (!known.contains(key)) {
            return Fail("the form has no field \"" + key.substr(0, 64) + "\"");
        }
    }
    return out;
}

Json defaultPromptAnswer(const Json& form) {
    Json out = Json::object();
    for (const Json& f : form.at("fields")) {
        out[f.at("id").get<std::string>()] = f.at("value");
    }
    return out;
}

Json helloMessage(const Manifest& manifest, command::CapabilitySet granted, std::string_view wabcutVersion, int callMs,
                  const Json& scripts, const Json& storage) {
    Json caps = Json::array();
    for (const command::Capability c : manifest.capabilities) {
        if (granted.has(c)) {
            caps.push_back(command::toString(c));
        }
    }
    return {{"type", "hello"},
            {"protocol", kScriptProtocolVersion},
            {"wabcutVersion", wabcutVersion},
            {"extension", {{"id", manifest.id}, {"name", manifest.name}, {"version", toString(manifest.version)},
                           {"capabilities", caps}}},
            {"limits", {{"callMs", callMs}}},
            {"scripts", scripts},
            {"storage", storage}};
}

} // namespace wabcut::script
