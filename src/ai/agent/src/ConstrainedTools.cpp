#include <wabcut/ai/agent/ConstrainedTools.h>

#include <algorithm>
#include <cctype>

namespace wabcut::ai::agent {

namespace {

constexpr std::string_view kReplyFormat = R"(# How to reply
Reply with exactly ONE JSON object and nothing else:
- to use a tool: {"name": "<tool name>", "parameters": {<its parameters>}}
- when the edit is done, or to answer a question: {"answer": "<one or two sentences>"}
One tool per reply: you get its result, then reply again. Parameters marked * are required.)";

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.remove_prefix(1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.remove_suffix(1);
    }
    return s;
}

/// The first `sentences` sentences of `text`, at most `maxChars` characters (cut at a UTF-8 boundary).
std::string firstSentences(std::string_view text, int sentences, std::size_t maxChars) {
    std::size_t end = 0;
    for (int i = 0; i < sentences; ++i) {
        const std::size_t dot = text.find(". ", end);
        if (dot == std::string_view::npos) {
            end = text.size();
            break;
        }
        end = dot + 1;
    }
    std::string out(trim(text.substr(0, end)));
    if (out.size() > maxChars) {
        std::size_t cut = maxChars;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) {
            --cut;
        }
        out = out.substr(0, cut) + "...";
    }
    return out;
}

std::string scalarText(const Json& v) { return v.is_string() ? v.get<std::string>() : v.dump(); }

/// "int 0..15", "comment|chapter", "[string]", "{name: string, time: int|string}".
std::string typeText(const Json& s, int depth = 0) {
    if (!s.is_object()) {
        return "any";
    }
    if (const auto e = s.find("enum"); e != s.end() && e->is_array()) {
        std::string out;
        std::size_t shown = 0;
        for (const Json& v : *e) {
            if (v.is_null()) {
                continue;
            }
            if (shown == 10) {
                out += "|...";
                break;
            }
            out += (shown++ ? "|" : "") + scalarText(v);
        }
        return out;
    }
    if (const auto c = s.find("const"); c != s.end()) {
        return scalarText(*c);
    }
    if (const auto a = s.find("anyOf"); a != s.end() && a->is_array()) {
        std::string out;
        for (const Json& branch : *a) {
            const std::string t = typeText(branch, depth);
            if (t != "null") {
                out += (out.empty() ? "" : "|") + t;
            }
        }
        return out;
    }
    std::vector<std::string> types;
    if (const auto t = s.find("type"); t != s.end()) {
        if (t->is_string()) {
            types.push_back(t->get<std::string>());
        } else if (t->is_array()) {
            for (const Json& name : *t) {
                if (name.is_string() && name != "null") {
                    types.push_back(name.get<std::string>());
                }
            }
        }
    }
    std::string out;
    for (const std::string& type : types) {
        std::string one;
        if (type == "integer" || type == "number") {
            one = type == "integer" ? "int" : "number";
            const auto lo = s.find("minimum");
            const auto hi = s.find("maximum");
            if (lo != s.end() && hi != s.end()) {
                one += " " + lo->dump() + ".." + hi->dump();
            } else if (lo != s.end()) {
                one += " >=" + lo->dump();
            }
        } else if (type == "boolean") {
            one = "bool";
        } else if (type == "array") {
            one = "[" + (s.contains("items") ? typeText(s["items"], depth + 1) : std::string("any")) + "]";
        } else if (type == "object") {
            if (depth >= 2 || !s.contains("properties") || !s["properties"].is_object()) {
                one = "object";
            } else {
                one = "{";
                bool first = true;
                for (const auto& [name, prop] : s["properties"].items()) {
                    one += (first ? "" : ", ") + name + ": " + typeText(prop, depth + 1);
                    first = false;
                }
                one += "}";
            }
        } else {
            one = type;
        }
        out += (out.empty() ? "" : "|") + one;
    }
    return out.empty() ? "any" : out;
}

/// The tool's parameter schema without "sequence" (the assistant adds the sequence being edited).
Json slimParameters(const Json& parameters) {
    Json p = parameters.is_object() ? parameters : Json::object();
    if (auto props = p.find("properties"); props != p.end() && props->is_object()) {
        props->erase("sequence");
    }
    if (auto req = p.find("required"); req != p.end() && req->is_array()) {
        Json kept = Json::array();
        for (const Json& name : *req) {
            if (name != "sequence") {
                kept.push_back(name);
            }
        }
        *req = std::move(kept);
    }
    if (!p.contains("type")) {
        p["type"] = "object";
    }
    return p;
}

Json answerBranch() {
    return Json{{"type", "object"},
                {"properties", {{"answer", {{"type", "string"}}}}},
                {"required", Json::array({"answer"})},
                {"additionalProperties", false}};
}

/// The first JSON object in `s` (balanced braces outside strings). A reply cut off inside the object is closed when
/// the cut is outside a string.
std::optional<std::string> firstObject(std::string_view s, bool& repaired) {
    const std::size_t start = s.find('{');
    if (start == std::string_view::npos) {
        return std::nullopt;
    }
    int depth = 0;
    bool inString = false;
    bool escaped = false;
    std::string closers;
    for (std::size_t i = start; i < s.size(); ++i) {
        const char c = s[i];
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                inString = false;
            }
            continue;
        }
        if (c == '"') {
            inString = true;
        } else if (c == '{' || c == '[') {
            ++depth;
            closers.push_back(c == '{' ? '}' : ']');
        } else if (c == '}' || c == ']') {
            --depth;
            if (!closers.empty()) {
                closers.pop_back();
            }
            if (depth == 0) {
                return std::string(s.substr(start, i - start + 1));
            }
        }
    }
    if (inString) {
        return std::nullopt;
    }
    repaired = true;
    std::string out(s.substr(start));
    out.append(closers.rbegin(), closers.rend());
    return out;
}

const ToolSpec* findTool(const std::vector<ToolSpec>& tools, std::string_view name) {
    for (const ToolSpec& t : tools) {
        if (t.name == name) {
            return &t;
        }
    }
    return nullptr;
}

/// "marker.add" or "Marker_Add" -> "marker_add" when that is a tool.
std::optional<std::string> canonicalTool(const std::vector<ToolSpec>& tools, std::string name) {
    std::replace(name.begin(), name.end(), '.', '_');
    for (const ToolSpec& t : tools) {
        if (t.name.size() == name.size() &&
            std::equal(t.name.begin(), t.name.end(), name.begin(), [](char a, char b) {
                return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
            })) {
            return t.name;
        }
    }
    return std::nullopt;
}

ParsedReply interpret(Json obj, const std::vector<ToolSpec>& tools, bool repaired) {
    ParsedReply out;
    out.repaired = repaired;
    if (!obj.is_object()) {
        out.error = "the reply is not a JSON object";
        return out;
    }
    if (const auto calls = obj.find("tool_calls"); calls != obj.end() && calls->is_array() && !calls->empty()) {
        obj = Json((*calls)[0]);
        out.repaired = true;
    }
    if (const auto fn = obj.find("function"); fn != obj.end() && fn->is_object()) {
        obj = Json(*fn);
        out.repaired = true;
    }
    std::optional<std::string> name;
    for (const char* key : {"name", "tool", "tool_name", "function"}) {
        if (const auto it = obj.find(key); it != obj.end() && it->is_string()) {
            name = it->get<std::string>();
            out.repaired = out.repaired || std::string_view(key) != "name";
            break;
        }
    }
    if (!name) {
        if (const auto a = obj.find("answer"); a != obj.end()) {
            out.answer = a->is_string() ? a->get<std::string>() : a->dump();
            out.repaired = out.repaired || !a->is_string();
            return out;
        }
        out.error = "the object has neither \"name\" nor \"answer\"";
        return out;
    }
    Json arguments = Json::object();
    bool found = false;
    for (const char* key : {"parameters", "arguments", "args", "params", "input"}) {
        if (const auto it = obj.find(key); it != obj.end()) {
            arguments = *it;
            found = true;
            out.repaired = out.repaired || std::string_view(key) != "parameters";
            break;
        }
    }
    if (!found || arguments.is_null()) {
        arguments = Json::object();
        out.repaired = true;
    }
    if (arguments.is_string()) {
        Json parsed = Json::parse(arguments.get<std::string>(), nullptr, false);
        if (parsed.is_discarded()) {
            out.error = "the parameters are a string that is not JSON";
            return out;
        }
        arguments = std::move(parsed);
        out.repaired = true;
    }
    if (!arguments.is_object()) {
        out.error = "the parameters are not a JSON object";
        return out;
    }
    if (!findTool(tools, *name)) {
        if (auto canonical = canonicalTool(tools, *name)) {
            name = std::move(canonical);
            out.repaired = true;
        }
    }
    ToolCall call;
    call.name = std::move(*name);
    call.argumentsText = arguments.dump();
    call.arguments = std::move(arguments);
    out.call = std::move(call);
    return out;
}

/// Earlier turns in the envelope format: calls as the JSON the model writes, results as text.
std::vector<ChatMessage> envelopeHistory(const std::vector<ChatMessage>& messages) {
    std::vector<ChatMessage> out;
    out.reserve(messages.size());
    for (const ChatMessage& m : messages) {
        ChatMessage converted;
        converted.role = m.role;
        if (m.role == Role::Assistant) {
            const std::vector<ToolCall> calls = m.toolCalls();
            std::string text;
            for (const ToolCall& c : calls) {
                const Json args = c.arguments.is_object() ? c.arguments : Json::object();
                text += (text.empty() ? "" : "\n") + Json{{"name", c.name}, {"parameters", args}}.dump();
            }
            if (calls.empty()) {
                const std::string answer = m.text();
                text = answer.empty() ? std::string() : Json{{"answer", answer}}.dump();
            }
            converted.parts.push_back(TextPart{text});
        } else {
            std::string results;
            for (const ContentPart& part : m.parts) {
                if (const auto* r = std::get_if<ToolResult>(&part)) {
                    results += (results.empty() ? "" : "\n") + std::string(r->isError ? "Error from " : "Result of ") + r->name +
                               ": " + r->content;
                } else if (std::holds_alternative<ReasoningPart>(part)) {
                    continue;
                } else {
                    converted.parts.push_back(part);
                }
            }
            if (!results.empty()) {
                converted.parts.insert(converted.parts.begin(), TextPart{results});
            }
        }
        out.push_back(std::move(converted));
    }
    return out;
}

} // namespace

Json toolEnvelopeSchema(const std::vector<ToolSpec>& tools, const ToolChoice& choice) {
    Json branches = Json::array();
    if (choice.mode != ToolChoiceMode::None) {
        for (const ToolSpec& tool : tools) {
            if (choice.mode == ToolChoiceMode::Named && tool.name != choice.name) {
                continue;
            }
            branches.push_back(Json{{"type", "object"},
                                    {"properties", {{"name", {{"const", tool.name}}}, {"parameters", slimParameters(tool.parameters)}}},
                                    {"required", Json::array({"name", "parameters"})},
                                    {"additionalProperties", false}});
        }
    }
    if (choice.mode == ToolChoiceMode::Auto || choice.mode == ToolChoiceMode::None || branches.empty()) {
        branches.push_back(answerBranch());
    }
    return Json{{"type", "object"}, {"anyOf", std::move(branches)}};
}

std::string toolSignature(const ToolSpec& tool) {
    std::string out = tool.name + ": " + firstSentences(tool.description, 2, 240) + "\n";
    const Json p = slimParameters(tool.parameters);
    std::vector<std::string> required;
    if (const auto r = p.find("required"); r != p.end() && r->is_array()) {
        for (const Json& name : *r) {
            if (name.is_string()) {
                required.push_back(name.get<std::string>());
            }
        }
    }
    const auto props = p.find("properties");
    if (props == p.end() || !props->is_object() || props->empty()) {
        return out + "  (no parameters)\n";
    }
    // Required parameters first, then the rest in the schema's order.
    std::vector<std::string> order = required;
    for (const auto& [name, prop] : props->items()) {
        if (std::ranges::find(order, name) == order.end()) {
            order.push_back(name);
        }
    }
    for (const std::string& name : order) {
        const auto prop = props->find(name);
        if (prop == props->end()) {
            continue;
        }
        const bool isRequired = std::ranges::find(required, name) != required.end();
        out += "  " + name + (isRequired ? "*" : "") + " (" + typeText(*prop) + ")";
        if (const auto d = prop->find("description"); d != prop->end() && d->is_string() && !d->get<std::string>().empty()) {
            out += ": " + firstSentences(d->get<std::string>(), 1, 110);
        }
        out += "\n";
    }
    return out;
}

ParsedReply parseEnvelopeReply(std::string_view text, const std::vector<ToolSpec>& tools) {
    std::string s(trim(text));
    bool repaired = false;
    // A thinking block a server did not strip.
    if (const std::size_t open = s.find("<think>"); open != std::string::npos) {
        const std::size_t close = s.find("</think>", open);
        s.erase(open, close == std::string::npos ? std::string::npos : close + 8 - open);
        s = std::string(trim(s));
        repaired = true;
    }
    ParsedReply out;
    if (s.empty()) {
        out.error = "the reply is empty";
        return out;
    }
    if (s.find('{') == std::string::npos) {
        out.answer = s;
        out.repaired = true;
        return out;
    }
    if (s.front() != '{') {
        repaired = true; // a code fence or words around the object
    }
    const std::optional<std::string> object = firstObject(s, repaired);
    if (!object) {
        out.error = "the reply is cut off inside a string";
        return out;
    }
    Json j = Json::parse(*object, nullptr, false);
    if (j.is_discarded()) {
        out.error = "the reply is not valid JSON";
        return out;
    }
    return interpret(std::move(j), tools, repaired);
}

ConstrainedToolChat::ConstrainedToolChat(IChatModel& inner) : m_inner(inner) {}

ChatRequest ConstrainedToolChat::prepare(const ChatRequest& request) const {
    if (request.tools.empty() || request.responseFormat) {
        return request;
    }
    ChatRequest out = request;
    std::string catalog = "# Tools\n";
    for (const ToolSpec& tool : request.tools) {
        if (request.toolChoice.mode == ToolChoiceMode::Named && tool.name != request.toolChoice.name) {
            continue;
        }
        catalog += toolSignature(tool);
    }
    if (request.toolChoice.mode == ToolChoiceMode::None) {
        catalog = "# Tools\nNo tools for this reply: answer.\n";
    }
    out.system = (request.system.empty() ? std::string() : request.system + "\n\n") + catalog + "\n" + std::string(kReplyFormat);
    out.messages = envelopeHistory(request.messages);
    out.tools.clear();
    out.toolChoice = {};
    ResponseFormat format;
    format.name = "tool_call";
    format.schema = toolEnvelopeSchema(request.tools, request.toolChoice);
    format.strict = false;
    out.responseFormat = std::move(format);
    return out;
}

ConstrainedStats ConstrainedToolChat::stats() const {
    std::lock_guard lock(m_mutex);
    return m_stats;
}

std::expected<ChatResponse, AiError> ConstrainedToolChat::chat(const ChatRequest& request, const ChatEventHandler& onEvent,
                                                               std::stop_token stop) {
    if (request.tools.empty() || request.responseFormat) {
        return m_inner.chat(request, onEvent, stop);
    }
    ChatRequest prepared = prepare(request);
    // The JSON streams as it is generated; the panel gets the answer's text once it is parsed.
    const ChatEventHandler filtered = [&onEvent](const ChatEvent& e) {
        if (onEvent && !std::holds_alternative<TextDelta>(e)) {
            onEvent(e);
        }
    };
    Usage usage;
    for (int attempt = 0;; ++attempt) {
        auto response = m_inner.chat(prepared, filtered, stop);
        if (!response) {
            return response;
        }
        usage += response->usage;
        {
            std::lock_guard lock(m_mutex);
            ++m_stats.replies;
        }
        if (const std::vector<ToolCall> native = response->message.toolCalls(); !native.empty()) {
            std::lock_guard lock(m_mutex);
            m_stats.calls += static_cast<int>(native.size());
            response->usage = usage;
            return response;
        }
        const std::string text = response->message.text();
        ParsedReply parsed = parseEnvelopeReply(text, request.tools);
        if (!parsed.call && !parsed.answer) {
            {
                std::lock_guard lock(m_mutex);
                ++m_stats.malformed;
            }
            if (attempt == 0 && !stop.stop_requested()) {
                prepared.messages.push_back(ChatMessage::assistant(text));
                prepared.messages.push_back(ChatMessage::user("That reply was not one JSON object in the reply format (" +
                                                              parsed.error +
                                                              "). Reply again with exactly one JSON object: a tool call or an answer."));
                continue;
            }
            parsed.answer = text; // ends the turn; the checks report what was (not) done
        }
        ChatResponse out;
        out.usage = usage;
        out.model = response->model;
        out.id = response->id;
        out.rawStopReason = response->rawStopReason;
        out.message.role = Role::Assistant;
        {
            std::lock_guard lock(m_mutex);
            m_stats.repaired += parsed.repaired ? 1 : 0;
            if (parsed.call) {
                ToolCall call = std::move(*parsed.call);
                call.id = "call_" + std::to_string(++m_nextId);
                out.message.parts.push_back(std::move(call));
                out.stopReason = StopReason::ToolUse;
                ++m_stats.calls;
            } else {
                out.message.parts.push_back(TextPart{*parsed.answer});
                out.stopReason = response->stopReason == StopReason::ToolUse ? StopReason::EndTurn : response->stopReason;
                ++m_stats.answers;
            }
        }
        if (!parsed.call && onEvent && !parsed.answer->empty()) {
            onEvent(TextDelta{0, *parsed.answer});
        }
        return out;
    }
}

} // namespace wabcut::ai::agent
