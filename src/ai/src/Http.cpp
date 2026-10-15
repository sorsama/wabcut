#include <wabcut/ai/Http.h>
#include <wabcut/ai/Redact.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <string>

namespace wabcut::ai {

namespace {

char asciiLower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

std::string lowerCopy(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), asciiLower);
    return out;
}

bool containsAny(std::string_view haystack, std::initializer_list<std::string_view> needles) {
    const std::string lower = lowerCopy(haystack);
    return std::any_of(needles.begin(), needles.end(),
                       [&](std::string_view n) { return lower.find(n) != std::string::npos; });
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
        s.remove_suffix(1);
    }
    return s;
}

std::string stringAt(const nlohmann::json& j, std::initializer_list<const char*> path) {
    const nlohmann::json* cur = &j;
    for (const char* key : path) {
        if (!cur->is_object()) {
            return {};
        }
        auto it = cur->find(key);
        if (it == cur->end()) {
            return {};
        }
        cur = &*it;
    }
    if (cur->is_string()) {
        return cur->get<std::string>();
    }
    if (cur->is_number_integer()) {
        return std::to_string(cur->get<long long>());
    }
    return {};
}

// Message and provider code from the error bodies of the supported APIs:
//   Anthropic   {"type":"error","error":{"type","message","details":{"error_code"}}}
//   OpenAI-ish  {"error":{"message","type","code","param"}}
//   Gemini      {"error":{"code":"invalid_request","message"}} / legacy {"error":{"code":429,"status","message"}}
//   Deepgram    {"err_code","err_msg"} / {"category","message"}
//   ElevenLabs  {"detail":{"type","code","message"}} / {"detail":[{"msg"}]}
//   AssemblyAI  {"error":"..."}
void describeBody(std::string_view body, std::string& message, std::string& code) {
    const auto j = nlohmann::json::parse(body, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        message = std::string(body.substr(0, 300));
        return;
    }
    if (auto it = j.find("error"); it != j.end()) {
        if (it->is_string()) {
            message = it->get<std::string>();
        } else if (it->is_object()) {
            message = stringAt(*it, {"message"});
            // Most specific first: Anthropic's details.error_code, a textual "code" (OpenAI insufficient_quota,
            // Gemini rate_limit_exceeded; Google's legacy numeric code is skipped), then "type" and "status".
            const std::string textual = (*it).contains("code") && (*it)["code"].is_string() ? stringAt(*it, {"code"})
                                                                                             : std::string();
            for (const std::string& candidate : {stringAt(*it, {"details", "error_code"}), textual,
                                                 stringAt(*it, {"type"}), stringAt(*it, {"status"})}) {
                if (!candidate.empty()) {
                    code = candidate;
                    break;
                }
            }
        }
    }
    if (message.empty()) {
        message = stringAt(j, {"err_msg"});
    }
    if (code.empty()) {
        code = stringAt(j, {"err_code"});
    }
    if (auto it = j.find("detail"); it != j.end()) {
        if (it->is_object()) {
            if (message.empty()) {
                message = stringAt(*it, {"message"});
            }
            if (code.empty()) {
                code = stringAt(*it, {"code"});
            }
            if (code.empty()) {
                code = stringAt(*it, {"type"});
            }
        } else if (it->is_string() && message.empty()) {
            message = it->get<std::string>();
        } else if (it->is_array() && !it->empty() && message.empty()) {
            message = stringAt((*it)[0], {"msg"});
            if (code.empty()) {
                code = stringAt((*it)[0], {"type"});
            }
        }
    }
    if (message.empty()) {
        message = stringAt(j, {"message"});
    }
    if (code.empty()) {
        code = stringAt(j, {"category"});
    }
}

bool parseInt(std::string_view s, int& out) {
    const auto* end = s.data() + s.size();
    auto [p, ec] = std::from_chars(s.data(), end, out);
    return ec == std::errc() && p == end;
}

} // namespace

bool equalsIgnoreCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (asciiLower(a[i]) != asciiLower(b[i])) {
            return false;
        }
    }
    return true;
}

std::optional<std::string_view> findHeader(const HttpHeaders& headers, std::string_view name) {
    for (const auto& h : headers) {
        if (equalsIgnoreCase(h.name, name)) {
            return std::string_view(h.value);
        }
    }
    return std::nullopt;
}

// --- URLs -------------------------------------------------------------------------------------------------------

std::optional<UrlParts> parseHttpUrl(std::string_view url) {
    for (const char ch : url) {
        const auto u = static_cast<unsigned char>(ch);
        if (u <= 0x20 || u == 0x7F || ch == '\\') {
            return std::nullopt;
        }
    }
    const auto schemeEnd = url.find("://");
    if (schemeEnd == std::string_view::npos || schemeEnd == 0) {
        return std::nullopt;
    }
    UrlParts parts;
    for (const char ch : url.substr(0, schemeEnd)) {
        parts.scheme.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    if (parts.scheme != "http" && parts.scheme != "https") {
        return std::nullopt;
    }
    std::string_view rest = url.substr(schemeEnd + 3);
    const auto authorityEnd = rest.find_first_of("/?#");
    std::string_view authority = rest.substr(0, authorityEnd);
    parts.path = authorityEnd == std::string_view::npos ? std::string() : std::string(rest.substr(authorityEnd));
    if (const auto at = authority.rfind('@'); at != std::string_view::npos) {
        parts.userinfo = std::string(authority.substr(0, at));
        authority = authority.substr(at + 1);
        if (parts.userinfo.empty()) {
            parts.userinfo = "@"; // "https://@host" still names (empty) credentials: never treat it as clean
        }
    }
    std::string_view host = authority;
    std::string_view port;
    if (authority.starts_with('[')) {
        const auto close = authority.find(']');
        if (close == std::string_view::npos) {
            return std::nullopt;
        }
        host = authority.substr(1, close - 1);
        const std::string_view after = authority.substr(close + 1);
        if (!after.empty()) {
            if (after.front() != ':') {
                return std::nullopt;
            }
            port = after.substr(1);
            if (port.empty()) {
                return std::nullopt;
            }
        }
        for (const char ch : host) {
            if (!std::isxdigit(static_cast<unsigned char>(ch)) && ch != ':' && ch != '.') {
                return std::nullopt;
            }
        }
    } else {
        if (const auto colon = authority.find(':'); colon != std::string_view::npos) {
            host = authority.substr(0, colon);
            port = authority.substr(colon + 1);
            if (port.empty()) {
                return std::nullopt;
            }
        }
        for (const char ch : host) {
            if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '.' && ch != '-') {
                return std::nullopt; // also rejects '%'-encoded and non-ASCII hosts
            }
        }
    }
    if (host.empty()) {
        return std::nullopt;
    }
    for (const char ch : host) {
        parts.host.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    if (!port.empty()) {
        if (port.size() > 5 || !std::all_of(port.begin(), port.end(), [](char ch) { return ch >= '0' && ch <= '9'; })) {
            return std::nullopt;
        }
        const int value = std::stoi(std::string(port));
        if (value < 1 || value > 65535) {
            return std::nullopt;
        }
        parts.port = value;
    }
    return parts;
}

// --- StreamDecoder ----------------------------------------------------------------------------------------------

StreamDecoder::StreamDecoder(BodyMode mode, const HttpStreamHandlers& handlers, StreamLimits limits)
    : m_mode(mode), m_handlers(handlers), m_limits(limits) {}

bool StreamDecoder::overflow(std::string what) {
    m_buffer.clear();
    m_data.clear();
    m_error = AiError(AiErrorCode::PayloadTooLarge, "the response stream was cut off: " + std::move(what));
    return false;
}

template <class Fn>
bool StreamDecoder::call(Fn&& fn) {
    try {
        return fn();
    } catch (const std::exception& e) {
        m_error = AiError(AiErrorCode::ProviderError, std::string("a malformed response broke its handler: ") + e.what());
    } catch (...) {
        m_error = AiError(AiErrorCode::ProviderError, "a malformed response broke its handler");
    }
    return false;
}

bool StreamDecoder::feed(std::span<const std::byte> bytes) {
    return feed(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

bool StreamDecoder::feed(std::string_view text) {
    if (m_error) {
        return false;
    }
    if (m_mode == BodyMode::Raw) {
        if (m_handlers.onData && !text.empty()) {
            return call([&] { return m_handlers.onData(std::as_bytes(std::span(text.data(), text.size()))); });
        }
        return true;
    }
    if (m_mode == BodyMode::Buffered) {
        return true;
    }
    m_total += text.size();
    if (m_total > m_limits.maxTotalBytes) {
        return overflow("more than " + std::to_string(m_limits.maxTotalBytes / (1024 * 1024)) + " MiB");
    }
    std::size_t pos = 0;
    while (pos < text.size()) {
        if (m_lastWasCr && text[pos] == '\n') { // second half of CRLF
            m_lastWasCr = false;
            ++pos;
            continue;
        }
        m_lastWasCr = false;
        const std::size_t eol = text.find_first_of("\r\n", pos);
        const std::size_t lineBytes = (eol == std::string_view::npos ? text.size() : eol) - pos;
        if (m_buffer.size() + lineBytes > m_limits.maxLineBytes) {
            return overflow("a line longer than " + std::to_string(m_limits.maxLineBytes / 1024) + " KiB");
        }
        if (eol == std::string_view::npos) {
            m_buffer.append(text.substr(pos));
            break;
        }
        m_buffer.append(text.substr(pos, eol - pos));
        m_lastWasCr = text[eol] == '\r';
        pos = eol + 1;
        std::string line;
        line.swap(m_buffer);
        if (!processLine(line)) {
            return false;
        }
    }
    return true;
}

bool StreamDecoder::finish() {
    if (m_error) {
        return false;
    }
    if (m_mode == BodyMode::Ndjson && !m_buffer.empty()) {
        std::string line;
        line.swap(m_buffer);
        return processLine(line);
    }
    // SSE: an event not terminated by a blank line is discarded (WHATWG "Server-sent events", end of stream).
    m_buffer.clear();
    return true;
}

bool StreamDecoder::processLine(std::string_view line) {
    if (!m_started) {
        m_started = true;
        if (line.starts_with("\xEF\xBB\xBF")) {
            line.remove_prefix(3);
        }
    }
    if (m_mode == BodyMode::Ndjson) {
        if (trim(line).empty() || !m_handlers.onLine) {
            return true;
        }
        return call([&] { return m_handlers.onLine(line); });
    }
    // SSE
    if (line.empty()) {
        return dispatchEvent();
    }
    if (line.front() == ':') {
        return true; // comment / keep-alive
    }
    std::string_view field = line;
    std::string_view value;
    if (const auto colon = line.find(':'); colon != std::string_view::npos) {
        field = line.substr(0, colon);
        value = line.substr(colon + 1);
        if (!value.empty() && value.front() == ' ') {
            value.remove_prefix(1);
        }
    }
    if (field == "event") {
        m_eventType.assign(value);
    } else if (field == "data") {
        if (m_data.size() + value.size() + 1 > m_limits.maxEventBytes) {
            return overflow("an event larger than " + std::to_string(m_limits.maxEventBytes / (1024 * 1024)) + " MiB");
        }
        m_data.append(value);
        m_data.push_back('\n');
        m_hasData = true;
    } else if (field == "id") {
        if (value.find('\0') == std::string_view::npos) {
            m_lastEventId.assign(value);
        }
    }
    // "retry" and unknown fields are ignored.
    return true;
}

bool StreamDecoder::dispatchEvent() {
    if (!m_hasData) {
        m_eventType.clear();
        return true;
    }
    if (!m_data.empty() && m_data.back() == '\n') {
        m_data.pop_back();
    }
    SseEvent event{std::move(m_eventType), std::move(m_data), m_lastEventId};
    m_eventType.clear();
    m_data.clear();
    m_hasData = false;
    if (!m_handlers.onEvent) {
        return true;
    }
    return call([&] { return m_handlers.onEvent(event); });
}

// --- Retry-After ------------------------------------------------------------------------------------------------

std::optional<std::chrono::milliseconds> parseRetryAfter(std::string_view value,
                                                         std::chrono::system_clock::time_point now) {
    value = trim(value);
    if (value.empty()) {
        return std::nullopt;
    }
    if (std::isdigit(static_cast<unsigned char>(value.front()))) {
        // delay-seconds; fractional values are accepted (some providers send "1.5").
        double seconds = 0.0;
        const auto* end = value.data() + value.size();
        auto [p, ec] = std::from_chars(value.data(), end, seconds);
        if (ec != std::errc() || p != end || !std::isfinite(seconds) || seconds < 0.0 || seconds > 86400.0 * 7) {
            return std::nullopt;
        }
        return std::chrono::milliseconds(static_cast<std::int64_t>(std::ceil(seconds * 1000.0)));
    }
    // IMF-fixdate: "Sun, 06 Nov 1994 08:49:37 GMT"
    static constexpr std::array<std::string_view, 12> kMonths{"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                              "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const auto comma = value.find(',');
    if (comma == std::string_view::npos) {
        return std::nullopt;
    }
    std::string_view rest = trim(value.substr(comma + 1));
    // "06 Nov 1994 08:49:37 GMT"
    if (rest.size() < 24) {
        return std::nullopt;
    }
    int day = 0;
    int year = 0;
    int hh = 0;
    int mm = 0;
    int ss = 0;
    if (!parseInt(rest.substr(0, 2), day) || !parseInt(rest.substr(7, 4), year) || !parseInt(rest.substr(12, 2), hh) ||
        !parseInt(rest.substr(15, 2), mm) || !parseInt(rest.substr(18, 2), ss) || rest.substr(21, 3) != "GMT") {
        return std::nullopt;
    }
    const auto monthIt = std::find(kMonths.begin(), kMonths.end(), rest.substr(3, 3));
    if (monthIt == kMonths.end()) {
        return std::nullopt;
    }
    const std::chrono::year_month_day ymd{std::chrono::year(year),
                                          std::chrono::month(static_cast<unsigned>(monthIt - kMonths.begin() + 1)),
                                          std::chrono::day(static_cast<unsigned>(day))};
    if (!ymd.ok() || hh > 23 || mm > 59 || ss > 60) {
        return std::nullopt;
    }
    const auto when = std::chrono::sys_days(ymd) + std::chrono::hours(hh) + std::chrono::minutes(mm) +
                      std::chrono::seconds(ss);
    const auto delta = std::chrono::duration_cast<std::chrono::milliseconds>(when - now);
    return std::max(delta, std::chrono::milliseconds(0));
}

std::optional<std::chrono::milliseconds> retryAfterFrom(const HttpHeaders& headers,
                                                        std::chrono::system_clock::time_point now) {
    if (auto ms = findHeader(headers, "retry-after-ms")) {
        double value = 0.0;
        const std::string_view v = trim(*ms);
        const auto* end = v.data() + v.size();
        auto [p, ec] = std::from_chars(v.data(), end, value);
        if (ec == std::errc() && p == end && std::isfinite(value) && value >= 0.0) {
            return std::chrono::milliseconds(static_cast<std::int64_t>(std::ceil(value)));
        }
    }
    if (auto ra = findHeader(headers, "retry-after")) {
        return parseRetryAfter(*ra, now);
    }
    return std::nullopt;
}

// --- Error classification ---------------------------------------------------------------------------------------

AiError classifyHttpError(int status, const HttpHeaders& headers, std::string_view body) {
    std::string message;
    std::string code;
    describeBody(body, message, code);

    AiError error;
    error.httpStatus = status;
    error.providerCode = Redactor::global().redact(code);
    const std::string probe = code + " " + message;

    const bool quota = containsAny(probe, {"insufficient_quota", "billing", "credit", "spend_limit", "spend limit",
                                           "payment", "insufficient_credits", "enforced_spend_limit_reached"});
    if (status == 401 || status == 403) {
        error.code = AiErrorCode::Unauthorized;
    } else if (status == 402) {
        error.code = AiErrorCode::QuotaExceeded;
    } else if (status == 413) {
        error.code = AiErrorCode::PayloadTooLarge;
    } else if (status == 408) {
        error.code = AiErrorCode::Network;
    } else if (status == 429) {
        error.code = quota ? AiErrorCode::QuotaExceeded : AiErrorCode::RateLimited;
    } else if (status == 529) {
        error.code = AiErrorCode::RateLimited; // Anthropic "overloaded_error"
    } else if (status == 404) {
        error.code = containsAny(probe, {"model"}) ? AiErrorCode::ModelMissing : AiErrorCode::InvalidRequest;
    } else if (status >= 400 && status < 500) {
        error.code = quota ? AiErrorCode::QuotaExceeded : AiErrorCode::InvalidRequest;
    } else {
        error.code = AiErrorCode::ProviderError;
    }
    if (error.code == AiErrorCode::RateLimited || status >= 500) {
        error.retryAfter = retryAfterFrom(headers, std::chrono::system_clock::now());
    }

    std::string text = "HTTP " + std::to_string(status);
    if (!code.empty()) {
        text += " " + code;
    }
    if (!message.empty()) {
        text += ": " + message;
    }
    if (text.size() > 500) {
        text.resize(500);
    }
    error.message = Redactor::global().redact(text);
    return error;
}

} // namespace wabcut::ai
