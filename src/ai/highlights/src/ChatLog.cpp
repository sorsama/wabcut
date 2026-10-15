#include <wabcut/ai/highlights/Highlights.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>

namespace wabcut::ai::highlights {

using nlohmann::json;

namespace {

Error invalid(std::string message) { return Error(ErrorCode::InvalidArgument, std::move(message)); }

std::optional<double> numberIn(const json& j, std::initializer_list<const char*> keys) {
    for (const char* key : keys) {
        if (auto it = j.find(key); it != j.end()) {
            if (it->is_number()) {
                return it->get<double>();
            }
            if (it->is_string()) { // yt-dlp writes milliseconds as text
                const std::string& s = it->get_ref<const std::string&>();
                double v = 0.0;
                const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
                if (ec == std::errc() && end == s.data() + s.size()) {
                    return v;
                }
            }
        }
    }
    return std::nullopt;
}

std::string stringIn(const json& j, std::initializer_list<const char*> keys) {
    for (const char* key : keys) {
        if (auto it = j.find(key); it != j.end() && it->is_string()) {
            return it->get<std::string>();
        }
    }
    return {};
}

void sortMessages(ChatLog& log) {
    std::stable_sort(log.messages.begin(), log.messages.end(), [](const ChatMessage& a, const ChatMessage& b) { return a.seconds < b.seconds; });
}

/// TwitchDownloader / Twitch VOD JSON: {"comments": [...]}.
ChatLog twitch(const json& root) {
    ChatLog log;
    log.format = "twitch";
    for (const json& c : root.at("comments")) {
        if (!c.is_object()) {
            continue;
        }
        const auto seconds = numberIn(c, {"content_offset_seconds"});
        if (!seconds) {
            continue;
        }
        ChatMessage m;
        m.seconds = *seconds;
        if (auto msg = c.find("message"); msg != c.end() && msg->is_object()) {
            m.text = stringIn(*msg, {"body"});
            if (auto frags = msg->find("fragments"); frags != msg->end() && frags->is_array()) {
                for (const json& f : *frags) {
                    if (f.is_object() && f.contains("emoticon") && !f["emoticon"].is_null()) {
                        const std::string name = stringIn(f, {"text"});
                        if (!name.empty()) {
                            m.emotes.push_back(name);
                        }
                    }
                }
            }
        }
        log.messages.push_back(std::move(m));
        if (log.messages.size() > kMaxChatMessages) {
            break;
        }
    }
    return log;
}

/// One yt-dlp live_chat.json line: {"replayChatItemAction": {"actions": [...], "videoOffsetTimeMsec": "12345"}}.
void youtubeLine(const json& line, ChatLog& log) {
    const auto replay = line.find("replayChatItemAction");
    if (replay == line.end() || !replay->is_object()) {
        return;
    }
    const auto offset = numberIn(*replay, {"videoOffsetTimeMsec"});
    if (!offset) {
        return;
    }
    const auto actions = replay->find("actions");
    if (actions == replay->end() || !actions->is_array()) {
        return;
    }
    for (const json& action : *actions) {
        const json* item = nullptr;
        if (auto add = action.find("addChatItemAction"); add != action.end() && add->is_object()) {
            if (auto it = add->find("item"); it != add->end() && it->is_object()) {
                item = &*it;
            }
        }
        if (!item) {
            continue;
        }
        for (const char* kind : {"liveChatTextMessageRenderer", "liveChatPaidMessageRenderer"}) {
            const auto renderer = item->find(kind);
            if (renderer == item->end() || !renderer->is_object()) {
                continue;
            }
            ChatMessage m;
            m.seconds = *offset / 1000.0;
            if (auto message = renderer->find("message"); message != renderer->end() && message->is_object()) {
                if (auto runs = message->find("runs"); runs != message->end() && runs->is_array()) {
                    for (const json& run : *runs) {
                        if (!run.is_object()) {
                            continue;
                        }
                        if (auto text = run.find("text"); text != run.end() && text->is_string()) {
                            m.text += text->get<std::string>();
                        } else if (auto emoji = run.find("emoji"); emoji != run.end() && emoji->is_object()) {
                            std::string name;
                            if (auto shortcuts = emoji->find("shortcuts"); shortcuts != emoji->end() && shortcuts->is_array() &&
                                                                            !shortcuts->empty() && (*shortcuts)[0].is_string()) {
                                name = (*shortcuts)[0].get<std::string>();
                                std::erase(name, ':');
                            } else {
                                name = stringIn(*emoji, {"emojiId"});
                            }
                            if (!name.empty()) {
                                m.emotes.push_back(name);
                                m.text += (m.text.empty() ? "" : " ") + name + " ";
                            }
                        }
                    }
                }
            }
            log.messages.push_back(std::move(m));
        }
    }
}

/// chat-downloader ([{"time_in_seconds", "message", "emotes": [{"name"}]}]) or a generic [{"time", "text"}] list.
std::optional<ChatLog> list(const json& items) {
    ChatLog log;
    for (const json& item : items) {
        if (!item.is_object()) {
            continue;
        }
        const bool downloader = item.contains("time_in_seconds");
        const auto seconds = numberIn(item, {"time_in_seconds", "content_offset_seconds", "time", "seconds", "offset"});
        if (!seconds) {
            continue;
        }
        log.format = downloader ? "chat-downloader" : (log.format.empty() ? "generic" : log.format);
        ChatMessage m;
        m.seconds = *seconds;
        m.text = stringIn(item, {"message", "text", "body"});
        if (auto emotes = item.find("emotes"); emotes != item.end() && emotes->is_array()) {
            for (const json& e : *emotes) {
                const std::string name = e.is_object() ? stringIn(e, {"name"}) : (e.is_string() ? e.get<std::string>() : std::string());
                if (!name.empty()) {
                    m.emotes.push_back(name);
                }
            }
        }
        log.messages.push_back(std::move(m));
        if (log.messages.size() > kMaxChatMessages) {
            break;
        }
    }
    if (log.messages.empty()) {
        return std::nullopt;
    }
    return log;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.remove_prefix(1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.remove_suffix(1);
    }
    return s;
}

/// "1:02:03.5", "62:03", "3723.5" -> seconds.
std::optional<double> parseClock(std::string_view s) {
    double total = 0.0;
    int parts = 0;
    while (!s.empty()) {
        const std::size_t colon = s.find(':');
        const std::string_view part = s.substr(0, colon);
        if (part.empty()) {
            return std::nullopt;
        }
        double v = 0.0;
        const auto [end, ec] = std::from_chars(part.data(), part.data() + part.size(), v);
        if (ec != std::errc() || end != part.data() + part.size() || !(v >= 0.0)) {
            return std::nullopt;
        }
        total = total * 60.0 + v;
        ++parts;
        if (colon == std::string_view::npos) {
            break;
        }
        s.remove_prefix(colon + 1);
    }
    if (parts == 0 || parts > 3 || !std::isfinite(total)) {
        return std::nullopt;
    }
    return total;
}

} // namespace

std::expected<ChatLog, Error> parseChatLog(std::string_view text) {
    json root = json::parse(text.begin(), text.end(), nullptr, false);
    ChatLog log;
    if (!root.is_discarded()) {
        if (root.is_object() && root.contains("comments") && root["comments"].is_array()) {
            log = twitch(root);
        } else if (root.is_object() && root.contains("replayChatItemAction")) {
            log.format = "youtube";
            youtubeLine(root, log);
        } else if (root.is_array()) {
            if (auto l = list(root)) {
                log = std::move(*l);
            }
        } else if (root.is_object()) {
            for (const char* key : {"messages", "chat"}) {
                if (root.contains(key) && root[key].is_array()) {
                    if (auto l = list(root[key])) {
                        log = std::move(*l);
                        break;
                    }
                }
            }
        }
    } else {
        // yt-dlp's live_chat.json: one JSON object per line.
        log.format = "youtube";
        std::size_t lines = 0;
        while (!text.empty()) {
            const std::size_t nl = text.find('\n');
            const std::string_view line = trim(text.substr(0, nl));
            text.remove_prefix(nl == std::string_view::npos ? text.size() : nl + 1);
            if (line.empty()) {
                continue;
            }
            ++lines;
            const json j = json::parse(line.begin(), line.end(), nullptr, false);
            if (j.is_discarded() || !j.is_object()) {
                return std::unexpected(invalid("line " + std::to_string(lines) + " of the chat log is not JSON"));
            }
            youtubeLine(j, log);
            if (log.messages.size() > kMaxChatMessages) {
                break;
            }
        }
    }
    if (log.messages.size() > kMaxChatMessages) {
        return std::unexpected(invalid("the chat log has more than " + std::to_string(kMaxChatMessages) + " messages"));
    }
    if (log.messages.empty()) {
        return std::unexpected(invalid("no chat messages found: expected a Twitch (TwitchDownloader), YouTube (yt-dlp live_chat.json) "
                                       "or chat-downloader JSON log"));
    }
    sortMessages(log);
    return log;
}

std::string toJson(const ChatLog& chat, double offsetSeconds) {
    json messages = json::array();
    for (const ChatMessage& m : chat.messages) {
        std::string text = m.text.substr(0, 500);
        // A cut inside a UTF-8 sequence would not be valid JSON text: drop the partial character.
        while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xC0) == 0x80) {
            text.pop_back();
        }
        if (!text.empty() && (static_cast<unsigned char>(text.back()) & 0x80) != 0) {
            text.pop_back();
        }
        messages.push_back(json::array({m.seconds, std::move(text), m.emotes}));
    }
    return json{{"schema", "wabcut.highlight-chat"}, {"version", kVersion}, {"format", chat.format}, {"offset", offsetSeconds},
                {"messages", std::move(messages)}}
        .dump(-1, ' ', false, json::error_handler_t::replace);
}

std::expected<std::pair<ChatLog, double>, Error> chatFromJson(std::string_view text) {
    const json j = json::parse(text.begin(), text.end(), nullptr, false);
    if (j.is_discarded() || !j.is_object() || j.value("schema", std::string()) != "wabcut.highlight-chat" || j.value("version", 0) != kVersion ||
        !j.contains("messages") || !j["messages"].is_array()) {
        return std::unexpected(invalid("not a kept chat log"));
    }
    ChatLog log;
    log.format = j.value("format", std::string());
    for (const json& m : j["messages"]) {
        if (!m.is_array() || m.size() != 3 || !m[0].is_number() || !m[1].is_string() || !m[2].is_array()) {
            return std::unexpected(invalid("a kept chat message is damaged"));
        }
        ChatMessage message;
        message.seconds = m[0].get<double>();
        message.text = m[1].get<std::string>();
        for (const json& e : m[2]) {
            if (e.is_string()) {
                message.emotes.push_back(e.get<std::string>());
            }
        }
        log.messages.push_back(std::move(message));
    }
    const double offset = j.contains("offset") && j["offset"].is_number() ? j["offset"].get<double>() : 0.0;
    return std::pair<ChatLog, double>{std::move(log), offset};
}

const std::vector<std::string>& defaultChatKeywords() {
    static const std::vector<std::string> words{
        // Twitch / BTTV / 7TV emotes that mean "that was something"
        "pog", "pogchamp", "poggers", "pogu", "kekw", "lul", "lulw", "omegalul", "monkas", "monkaw", "pepega", "ez", "clap",
        "pogo", "sadge", "copium", "aware", "wicked", "kreygasm", "residentsleeper", "gigachad", "catjam", "dinkdonk",
        // words
        "clip", "clipit", "clipped", "insane", "omg", "wtf", "lol", "lmao", "lmfao", "rofl", "holy", "nooo", "noway", "gg",
        "ggs", "wow", "crazy", "sheesh", "goated", "clutch", "ace", "nuts", "cracked", "hype", "w", "dub", "rip", "f"};
    return words;
}

std::expected<std::vector<Hint>, Error> parseMarkerFile(std::string_view text) {
    std::vector<Hint> hints;
    const std::string_view body = trim(text);
    if (!body.empty() && (body.front() == '[' || body.front() == '{')) {
        json root = json::parse(body.begin(), body.end(), nullptr, false);
        if (root.is_discarded()) {
            return std::unexpected(invalid("the marker file is not valid JSON"));
        }
        const json* items = &root;
        if (root.is_object()) {
            for (const char* key : {"markers", "chapters", "highlights"}) {
                if (root.contains(key) && root[key].is_array()) {
                    items = &root[key];
                    break;
                }
            }
        }
        if (items->is_array()) {
            for (const json& item : *items) {
                if (!item.is_object()) {
                    continue;
                }
                std::optional<double> seconds = numberIn(item, {"time", "seconds", "offset", "start"});
                if (!seconds) {
                    const std::string clock = stringIn(item, {"time", "timestamp"});
                    seconds = parseClock(clock);
                }
                if (!seconds || *seconds < 0.0 || *seconds > 7.0 * 24 * 3600) {
                    continue;
                }
                hints.push_back({Ticks{static_cast<std::int64_t>(std::llround(*seconds * static_cast<double>(kTicksPerSecond)))},
                                 stringIn(item, {"label", "name", "text", "title"}), "marker file"});
            }
        }
    } else {
        std::string_view rest = text;
        while (!rest.empty()) {
            const std::size_t nl = rest.find('\n');
            std::string_view line = trim(rest.substr(0, nl));
            rest.remove_prefix(nl == std::string_view::npos ? rest.size() : nl + 1);
            if (line.empty() || line.front() == '#') {
                continue;
            }
            std::size_t split = 0;
            while (split < line.size() && !std::isspace(static_cast<unsigned char>(line[split])) && line[split] != ',' && line[split] != ';') {
                ++split;
            }
            const auto seconds = parseClock(line.substr(0, split));
            if (!seconds || *seconds > 7.0 * 24 * 3600) {
                continue; // a header ("time,label") or prose
            }
            std::string_view label = trim(line.substr(split));
            while (!label.empty() && (label.front() == ',' || label.front() == ';' || label.front() == '-')) {
                label = trim(label.substr(1));
            }
            if (label.size() >= 2 && label.front() == '"' && label.back() == '"') {
                label = label.substr(1, label.size() - 2);
            }
            hints.push_back({Ticks{static_cast<std::int64_t>(std::llround(*seconds * static_cast<double>(kTicksPerSecond)))},
                             std::string(label.substr(0, 200)), "marker file"});
        }
    }
    if (hints.empty()) {
        return std::unexpected(invalid("no markers found in the marker file"));
    }
    if (hints.size() > 10000) {
        hints.resize(10000);
    }
    std::stable_sort(hints.begin(), hints.end(), [](const Hint& a, const Hint& b) { return a.at < b.at; });
    return hints;
}

} // namespace wabcut::ai::highlights
