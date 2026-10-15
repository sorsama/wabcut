#include <wabcut/ai/Redact.h>

#include <algorithm>
#include <array>
#include <cctype>

namespace wabcut::ai {

namespace {

constexpr std::array<std::string_view, 11> kSecretHeaders{
    "authorization", "proxy-authorization", "x-api-key", "xi-api-key",   "x-goog-api-key", "ocp-apim-subscription-key",
    "deepl-auth-key", "api-key",            "cookie",    "set-cookie", "x-goog-upload-url",
};

// Query parameters that carry credentials.
constexpr std::array<std::string_view, 7> kSecretParams{"key", "api_key", "apikey", "access_token", "token", "sig",
                                                      "upload_id"};

bool isTokenChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' || c == '~' || c == '+' ||
           c == '/' || c == '=' || c == ':';
}

char lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

/// Case-insensitive find of `needle` in `hay` from `from`.
std::size_t ifind(std::string_view hay, std::string_view needle, std::size_t from) {
    if (needle.empty() || hay.size() < needle.size()) {
        return std::string_view::npos;
    }
    for (std::size_t i = from; i + needle.size() <= hay.size(); ++i) {
        bool match = true;
        for (std::size_t k = 0; k < needle.size(); ++k) {
            if (lower(hay[i + k]) != lower(needle[k])) {
                match = false;
                break;
            }
        }
        if (match) {
            return i;
        }
    }
    return std::string_view::npos;
}

void replaceAll(std::string& text, std::string_view what, std::string_view with) {
    if (what.empty()) {
        return;
    }
    std::size_t pos = 0;
    while ((pos = text.find(what, pos)) != std::string::npos) {
        text.replace(pos, what.size(), with);
        pos += with.size();
    }
}

// "<header>: value" / "\"<header>\": \"value\"" / "<header>=value": masks the value up to the end of the line,
// a closing quote or a separator.
void maskHeaderValues(std::string& text) {
    for (std::string_view header : kSecretHeaders) {
        std::size_t pos = 0;
        while ((pos = ifind(text, header, pos)) != std::string::npos) {
            const bool boundaryBefore = pos == 0 || !(std::isalnum(static_cast<unsigned char>(text[pos - 1])) ||
                                                       text[pos - 1] == '-' || text[pos - 1] == '_');
            std::size_t i = pos + header.size();
            const bool boundaryAfter = i >= text.size() || !(std::isalnum(static_cast<unsigned char>(text[i])) ||
                                                            text[i] == '-' || text[i] == '_');
            if (!boundaryBefore || !boundaryAfter) {
                pos = i;
                continue;
            }
            if (i < text.size() && text[i] == '"') {
                ++i;
            }
            while (i < text.size() && text[i] == ' ') {
                ++i;
            }
            if (i >= text.size() || (text[i] != ':' && text[i] != '=')) {
                pos = i;
                continue;
            }
            ++i;
            while (i < text.size() && text[i] == ' ') {
                ++i;
            }
            const bool quoted = i < text.size() && text[i] == '"';
            if (quoted) {
                ++i;
            }
            std::size_t end = i;
            while (end < text.size() && text[end] != '\r' && text[end] != '\n' &&
                   !(quoted ? text[end] == '"' : (text[end] == ',' || text[end] == ';' || text[end] == '}'))) {
                if (quoted && text[end] == '\\' && end + 1 < text.size()) {
                    ++end;
                }
                ++end;
            }
            if (end > i && text.compare(i, end - i, kRedacted) != 0) {
                text.replace(i, end - i, kRedacted);
                end = i + kRedacted.size();
            }
            pos = end;
        }
    }
}

// "Bearer <token>", "Token <token>", "Key <token>" with a credential-looking token.
void maskSchemes(std::string& text) {
    for (std::string_view scheme : {std::string_view("bearer "), std::string_view("token "), std::string_view("key ")}) {
        std::size_t pos = 0;
        while ((pos = ifind(text, scheme, pos)) != std::string::npos) {
            const std::size_t start = pos + scheme.size();
            std::size_t end = start;
            while (end < text.size() && isTokenChar(text[end])) {
                ++end;
            }
            if (end - start >= 12 && text.compare(start, end - start, kRedacted) != 0) {
                text.replace(start, end - start, kRedacted);
                end = start + kRedacted.size();
            }
            pos = end;
        }
    }
}

// Well-known key shapes even without context: OpenAI/Anthropic "sk-...", Google "AIza...".
void maskKeyShapes(std::string& text) {
    for (std::string_view prefix : {std::string_view("sk-"), std::string_view("AIza")}) {
        std::size_t pos = 0;
        while ((pos = text.find(prefix, pos)) != std::string::npos) {
            if (pos > 0 && isTokenChar(text[pos - 1]) && text[pos - 1] != ':' && text[pos - 1] != '=' &&
                text[pos - 1] != '/') {
                pos += prefix.size();
                continue;
            }
            std::size_t end = pos + prefix.size();
            while (end < text.size() && isTokenChar(text[end]) && text[end] != ':' && text[end] != '/') {
                ++end;
            }
            if (end - pos >= 20) {
                text.replace(pos, end - pos, kRedacted);
                end = pos + kRedacted.size();
            }
            pos = end;
        }
    }
}

void maskQueryParams(std::string& text) {
    for (std::string_view param : kSecretParams) {
        std::size_t pos = 0;
        while ((pos = ifind(text, param, pos)) != std::string::npos) {
            const bool afterSeparator = pos > 0 && (text[pos - 1] == '?' || text[pos - 1] == '&');
            const std::size_t eq = pos + param.size();
            if (!afterSeparator || eq >= text.size() || text[eq] != '=') {
                pos = eq;
                continue;
            }
            std::size_t end = eq + 1;
            while (end < text.size() && text[end] != '&' && text[end] != '#' && text[end] != '"' && text[end] != ' ' &&
                   text[end] != '\r' && text[end] != '\n') {
                ++end;
            }
            if (end > eq + 1) {
                text.replace(eq + 1, end - eq - 1, kRedacted);
                end = eq + 1 + kRedacted.size();
            }
            pos = end;
        }
    }
}

} // namespace

bool isSecretHeader(std::string_view name) {
    return std::any_of(kSecretHeaders.begin(), kSecretHeaders.end(),
                       [&](std::string_view h) { return equalsIgnoreCase(h, name); });
}

Redactor& Redactor::global() {
    static Redactor instance;
    return instance;
}

void Redactor::addSecret(std::string_view value) {
    if (value.size() < 8) {
        return;
    }
    std::lock_guard lock(m_mutex);
    if (std::find(m_secrets.begin(), m_secrets.end(), value) == m_secrets.end()) {
        m_secrets.emplace_back(value);
        // Longest first, so a key that contains another registered key is masked whole.
        std::sort(m_secrets.begin(), m_secrets.end(),
                  [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
    }
}

void Redactor::removeSecret(std::string_view value) {
    std::lock_guard lock(m_mutex);
    auto it = std::find(m_secrets.begin(), m_secrets.end(), value);
    if (it != m_secrets.end()) {
        std::fill(it->begin(), it->end(), '\0');
        m_secrets.erase(it);
    }
}

void Redactor::clearSecrets() {
    std::lock_guard lock(m_mutex);
    for (auto& s : m_secrets) {
        std::fill(s.begin(), s.end(), '\0');
    }
    m_secrets.clear();
}

std::string Redactor::redact(std::string_view text) const {
    std::string out(text);
    {
        std::lock_guard lock(m_mutex);
        for (const auto& secret : m_secrets) {
            replaceAll(out, secret, kRedacted);
        }
    }
    maskHeaderValues(out);
    maskSchemes(out);
    maskQueryParams(out);
    maskKeyShapes(out);
    return out;
}

std::string Redactor::redactSecrets(std::string_view text, std::string_view replacement) const {
    std::string out(text);
    std::lock_guard lock(m_mutex);
    for (const auto& secret : m_secrets) {
        replaceAll(out, secret, replacement);
    }
    return out;
}

std::string Redactor::redactUrl(std::string_view url) const { return redact(url); }

HttpHeaders Redactor::redactHeaders(const HttpHeaders& headers) const {
    HttpHeaders out;
    out.reserve(headers.size());
    for (const auto& h : headers) {
        out.push_back({h.name, isSecretHeader(h.name) ? std::string(kRedacted) : redact(h.value)});
    }
    return out;
}

} // namespace wabcut::ai
