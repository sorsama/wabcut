// Helpers shared by the adapters added in round 07 (AdapterUtil.h, "r07"): checked downloads of generated media,
// file signatures, conditioning images, paid requests and audio measurements.

#include "AdapterUtil.h"

#include <wabcut/ai/JsonAccess.h>
#include <wabcut/ai/Redirects.h>
#include <wabcut/model/MediaLocator.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <fstream>
#include <random>

namespace wabcut::ai::net {

namespace {

AiError invalid(std::string text) { return {AiErrorCode::InvalidRequest, std::move(text)}; }

std::string lowerAscii(std::string text) {
    for (char& c : text) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

/// A unique stem in `directory` for a new output file.
std::filesystem::path newStem(const std::filesystem::path& directory) {
    static std::atomic<unsigned> serial{0};
    return directory / ("generated-" + std::to_string(std::random_device{}()) + "-" + std::to_string(++serial));
}

std::string_view kindName(GeneratedMediaKind kind) {
    switch (kind) {
    case GeneratedMediaKind::Image:
        return "a PNG, JPEG or WebP image";
    case GeneratedMediaKind::Video:
        return "an MP4 video";
    case GeneratedMediaKind::SoundEffect:
        return "an MP3, WAV, FLAC, OGG or M4A sound";
    }
    return "the expected media";
}

/// The length of MP3 audio (MPEG-1, -2 or -2.5 Layer III) in milliseconds, by walking its frames after an ID3v2 tag.
std::int64_t mp3Milliseconds(std::span<const std::uint8_t> b) {
    const auto at8 = [&](std::size_t i) { return static_cast<unsigned>(b[i]); };
    std::size_t at = 0;
    if (b.size() >= 10 && b[0] == 'I' && b[1] == 'D' && b[2] == '3') {
        const std::size_t size = (at8(6) & 0x7F) << 21 | (at8(7) & 0x7F) << 14 | (at8(8) & 0x7F) << 7 | (at8(9) & 0x7F);
        at = 10 + size + ((at8(5) & 0x10) != 0 ? 10 : 0);
    }
    constexpr int kMpeg1Kbps[16] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
    constexpr int kMpeg2Kbps[16] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
    constexpr int kMpeg1Rates[3] = {44100, 48000, 32000};
    double seconds = 0.0;
    while (at + 4 <= b.size()) {
        const unsigned h1 = at8(at + 1), h2 = at8(at + 2);
        const unsigned version = (h1 >> 3) & 3;
        const unsigned layer = (h1 >> 1) & 3;
        const unsigned bitrateIndex = h2 >> 4, rateIndex = (h2 >> 2) & 3, padding = (h2 >> 1) & 1;
        if (at8(at) != 0xFF || (h1 & 0xE0) != 0xE0 || version == 1 || layer != 1 || bitrateIndex == 0 || bitrateIndex == 15 ||
            rateIndex == 3) {
            ++at;
            continue;
        }
        const bool mpeg1 = version == 3;
        const int rate = kMpeg1Rates[rateIndex] / (mpeg1 ? 1 : version == 2 ? 2 : 4);
        const int kbps = (mpeg1 ? kMpeg1Kbps : kMpeg2Kbps)[bitrateIndex];
        const std::size_t length = static_cast<std::size_t>((mpeg1 ? 144 : 72) * kbps * 1000 / rate) + padding;
        if (length < 4 || at + length > b.size()) {
            break;
        }
        seconds += (mpeg1 ? 1152.0 : 576.0) / rate;
        at += length;
    }
    return std::llround(seconds * 1000.0);
}

std::optional<std::int64_t> wavMilliseconds(std::span<const std::uint8_t> wav) {
    const auto u32 = [&](std::size_t at) {
        return static_cast<std::uint32_t>(wav[at]) | (static_cast<std::uint32_t>(wav[at + 1]) << 8) |
               (static_cast<std::uint32_t>(wav[at + 2]) << 16) | (static_cast<std::uint32_t>(wav[at + 3]) << 24);
    };
    if (wav.size() < 12 || std::string_view(reinterpret_cast<const char*>(wav.data()), 4) != "RIFF" ||
        std::string_view(reinterpret_cast<const char*>(wav.data()) + 8, 4) != "WAVE") {
        return std::nullopt;
    }
    std::uint32_t byteRate = 0;
    std::size_t at = 12;
    while (at + 8 <= wav.size()) {
        const std::string_view id(reinterpret_cast<const char*>(wav.data()) + at, 4);
        std::uint64_t size = u32(at + 4);
        if (id == "fmt " && at + 8 + 16 <= wav.size()) {
            byteRate = u32(at + 8 + 8);
        } else if (id == "data") {
            const std::uint64_t available = wav.size() - (at + 8);
            if (size == 0 || size == 0xFFFFFFFFu || size > available) {
                size = available;
            }
            if (byteRate == 0) {
                return std::nullopt;
            }
            return static_cast<std::int64_t>(size * 1000 / byteRate);
        }
        at += 8 + static_cast<std::size_t>(size) + (size & 1);
    }
    return std::nullopt;
}

} // namespace

bool hostMatches(std::string_view host, std::initializer_list<std::string_view> patterns) {
    for (const std::string_view pattern : patterns) {
        if (pattern.starts_with("*.")) {
            const std::string_view suffix = pattern.substr(1); // ".example.com"
            if (host.size() > suffix.size() && host.ends_with(suffix)) {
                const std::string_view label = host.substr(0, host.size() - suffix.size());
                if (!label.empty() && label.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-.") == std::string_view::npos &&
                    !label.starts_with('.') && !label.ends_with('.')) {
                    return true;
                }
            }
        } else if (host == pattern) {
            return true;
        }
    }
    return false;
}

std::optional<std::string> generatedFileExtension(std::string contentType, std::string_view magic, GeneratedMediaKind kind) {
    contentType = lowerAscii(std::move(contentType));
    if (const auto semicolon = contentType.find(';'); semicolon != std::string::npos) {
        contentType.resize(semicolon);
    }
    while (!contentType.empty() && contentType.back() == ' ') {
        contentType.pop_back();
    }
    const bool generic = contentType.empty() || contentType == "application/octet-stream" || contentType == "binary/octet-stream";
    const auto typed = [&](std::initializer_list<std::string_view> types) {
        return generic || std::ranges::any_of(types, [&](std::string_view t) { return contentType == t; });
    };
    const auto u8 = [&](std::size_t i) { return i < magic.size() ? static_cast<unsigned char>(magic[i]) : 0u; };
    switch (kind) {
    case GeneratedMediaKind::Image:
        if (magic.starts_with(std::string_view("\x89PNG\r\n\x1a\n", 8)) && typed({"image/png"})) {
            return ".png";
        }
        if (magic.starts_with(std::string_view("\xff\xd8\xff", 3)) && typed({"image/jpeg", "image/jpg"})) {
            return ".jpg";
        }
        if (magic.size() >= 12 && magic.substr(0, 4) == "RIFF" && magic.substr(8, 4) == "WEBP" && typed({"image/webp"})) {
            return ".webp";
        }
        return std::nullopt;
    case GeneratedMediaKind::Video:
        if (magic.size() >= 8 && magic.substr(4, 4) == "ftyp" && typed({"video/mp4", "video/quicktime", "application/mp4"})) {
            return ".mp4";
        }
        return std::nullopt;
    case GeneratedMediaKind::SoundEffect:
        if (magic.size() >= 12 && magic.substr(0, 4) == "RIFF" && magic.substr(8, 4) == "WAVE" &&
            typed({"audio/wav", "audio/x-wav", "audio/wave", "audio/vnd.wave"})) {
            return ".wav";
        }
        if ((magic.starts_with("ID3") || (u8(0) == 0xFF && (u8(1) & 0xE0) == 0xE0)) && typed({"audio/mpeg", "audio/mp3"})) {
            return ".mp3";
        }
        if (magic.starts_with("fLaC") && typed({"audio/flac", "audio/x-flac"})) {
            return ".flac";
        }
        if (magic.starts_with("OggS") && typed({"audio/ogg", "audio/opus", "application/ogg"})) {
            return ".ogg";
        }
        if (magic.size() >= 8 && magic.substr(4, 4) == "ftyp" && typed({"audio/mp4", "audio/m4a", "audio/x-m4a", "audio/aac"})) {
            return ".m4a";
        }
        return std::nullopt;
    }
    return std::nullopt;
}

namespace {

/// The download itself, once the URL's origin is approved: redirects within `allow` only, at most 512 MiB, a file of
/// `kind` (its signature checked) renamed into place.
std::expected<std::filesystem::path, AiError> fetchGenerated(IHttpTransport& transport, const std::string& url, HttpHeaders headers,
                                                             const std::filesystem::path& directory, GeneratedMediaKind kind,
                                                             const RedirectAllowList& allow, std::stop_token stop) {
    HttpRequest http;
    http.method = "GET";
    http.url = url;
    http.headers = std::move(headers);
    http.responseMode = BodyMode::Raw;
    http.timeout = std::chrono::minutes(30);
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) {
        return std::unexpected(invalid("cannot create generation directory"));
    }
    const auto stem = newStem(directory);
    auto partial = stem; // appended to the path: stem.string() throws for names outside the code page
    partial += ".part";
    std::ofstream file(partial, std::ios::binary | std::ios::trunc);
    if (!file) {
        return std::unexpected(invalid("cannot write generated asset"));
    }
    std::uint64_t count = 0;
    std::string contentType;
    std::string magic;
    HttpStreamHandlers handlers;
    handlers.onHead = [&](const HttpResponseHead& head) {
        if (auto type = findHeader(head.headers, "content-type")) {
            contentType = std::string(*type);
        }
        return true;
    };
    handlers.onData = [&](std::span<const std::byte> data) {
        count += data.size();
        if (count > 512ULL * 1024 * 1024) {
            return false;
        }
        if (magic.size() < 16) {
            magic.append(reinterpret_cast<const char*>(data.data()), std::min<std::size_t>(16 - magic.size(), data.size()));
        }
        file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        return static_cast<bool>(file);
    };
    // Redirects only on the download host itself: a cross-host hop fails closed and credentials never travel with it.
    auto response = sendFollowingRedirects(transport, http, handlers, stop, allow);
    file.close();
    if (!response || !response->ok() || count == 0 || !file) {
        std::filesystem::remove(partial, ec);
        return std::unexpected(!response ? response.error() : !response->ok() ? httpError(*response) : invalid("empty or incomplete generated asset"));
    }
    const auto extension = generatedFileExtension(contentType, magic, kind);
    if (!extension) {
        std::filesystem::remove(partial, ec);
        return std::unexpected(invalid("the provider returned something that is not " + std::string(kindName(kind))));
    }
    auto path = stem;
    path += *extension;
    std::filesystem::rename(partial, path, ec);
    if (ec) {
        std::filesystem::remove(partial, ec);
        return std::unexpected(invalid("cannot finalize generated asset"));
    }
    return path;
}

int portOf(const UrlParts& url) { return url.port ? *url.port : (url.scheme == "https" ? 443 : 80); }

} // namespace

std::expected<std::filesystem::path, AiError> downloadGenerated(IHttpTransport& transport, const std::string& url, HttpHeaders headers,
                                                                const std::filesystem::path& directory,
                                                                std::initializer_list<std::string_view> hosts, GeneratedMediaKind kind,
                                                                std::stop_token stop) {
    const auto parts = parseHttpUrl(url);
    if (!parts || parts->scheme != "https" || !parts->userinfo.empty() || (parts->port && *parts->port != 443) ||
        !hostMatches(parts->host, hosts)) {
        return std::unexpected(invalid("generation returned an unapproved download host"));
    }
    return fetchGenerated(transport, url, std::move(headers), directory, kind, RedirectAllowList::sameHost(), stop);
}

std::expected<std::filesystem::path, AiError> downloadFromProvider(IHttpTransport& transport, const std::string& url, HttpHeaders headers,
                                                                   const std::filesystem::path& directory, std::string_view baseUrl,
                                                                   GeneratedMediaKind kind, std::stop_token stop) {
    const auto base = parseHttpUrl(baseUrl);
    const auto parts = parseHttpUrl(url);
    if (!base || !parts || !parts->userinfo.empty() || parts->scheme != base->scheme || parts->host != base->host ||
        portOf(*parts) != portOf(*base)) {
        return std::unexpected(invalid("generation returned an unapproved download host"));
    }
    if (parts->scheme != "https" && !isLoopbackUrl(url)) { // plain http only from a server on this computer
        return std::unexpected(invalid("generation returned an unapproved download host"));
    }
    RedirectAllowList allow = RedirectAllowList::sameHost();
    allow.httpsOnly = parts->scheme == "https"; // a loopback server may redirect on itself, never anywhere else
    return fetchGenerated(transport, url, std::move(headers), directory, kind, allow, stop);
}

std::expected<std::filesystem::path, AiError> writeGenerated(std::span<const std::uint8_t> bytes, std::string_view contentType,
                                                             const std::filesystem::path& directory, GeneratedMediaKind kind) {
    if (bytes.empty() || bytes.size() > 512ULL * 1024 * 1024) {
        return std::unexpected(invalid("empty or oversized generated asset"));
    }
    const std::string_view magic(reinterpret_cast<const char*>(bytes.data()), std::min<std::size_t>(16, bytes.size()));
    const auto extension = generatedFileExtension(std::string(contentType), magic, kind);
    if (!extension) {
        return std::unexpected(invalid("the provider returned something that is not " + std::string(kindName(kind))));
    }
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) {
        return std::unexpected(invalid("cannot create generation directory"));
    }
    auto path = newStem(directory);
    path += *extension;
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    file.close();
    if (!file) {
        std::filesystem::remove(path, ec);
        return std::unexpected(invalid("cannot write generated asset"));
    }
    return path;
}

std::expected<ImagePart, AiError> loadConditioningImage(const std::filesystem::path& path, std::size_t maxBytes) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size == 0 || size > maxBytes) {
        return std::unexpected(invalid("the start image must be 1 byte to " + std::to_string(maxBytes / (1024 * 1024)) + " MiB"));
    }
    const auto ext = lowerAscii(pathToUtf8(path.extension()));
    if (ext != ".png" && ext != ".jpg" && ext != ".jpeg") {
        return std::unexpected(invalid("the start image must be a PNG or JPEG file"));
    }
    ImagePart image;
    image.mediaType = ext == ".png" ? "image/png" : "image/jpeg";
    image.bytes.resize(static_cast<std::size_t>(size));
    std::ifstream file(path, std::ios::binary);
    file.read(reinterpret_cast<char*>(image.bytes.data()), static_cast<std::streamsize>(size));
    if (!file) {
        return std::unexpected(invalid("cannot read the start image"));
    }
    return image;
}

bool safePathId(std::string_view id, bool slash) {
    return !id.empty() && id.size() < 512 && id.find("..") == std::string_view::npos && id.find("//") == std::string_view::npos &&
           std::all_of(id.begin(), id.end(), [slash](unsigned char c) {
               return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                      (slash && (c == '/' || c == '.'));
           });
}

HttpRequest paidSubmit(HttpRequest request, std::chrono::seconds timeout) {
    request.retrySafe = false;
    request.timeout = timeout;
    request.idleTimeout = std::min<std::chrono::milliseconds>(timeout, std::chrono::seconds(120));
    return request;
}

std::expected<Json, AiError> sendForJson(IHttpTransport& transport, const HttpRequest& request, std::string_view what, std::stop_token stop) {
    auto response = transport.send(request, {}, stop);
    if (!response) {
        return std::unexpected(response.error());
    }
    if (!response->ok()) {
        return std::unexpected(httpError(*response));
    }
    return parseJsonBody(response->body, what);
}

std::expected<Json, AiError> getJson(IHttpTransport& transport, const ProviderContext& context, std::string url, std::string_view what,
                                     std::stop_token stop) {
    HttpRequest http;
    http.method = "GET";
    http.url = std::move(url);
    http.headers = providerHeaders(context);
    http.timeout = std::chrono::seconds(60);
    return sendForJson(transport, http, what, stop);
}

std::int64_t codePointCount(std::string_view text) {
    return static_cast<std::int64_t>(
        std::count_if(text.begin(), text.end(), [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
}

std::pair<int, int> sizeForAspect(std::string_view aspectRatio, int longSide, int multiple) {
    int w = 1, h = 1;
    if (const auto colon = aspectRatio.find(':'); colon != std::string_view::npos) {
        try {
            w = std::max(1, std::stoi(std::string(aspectRatio.substr(0, colon))));
            h = std::max(1, std::stoi(std::string(aspectRatio.substr(colon + 1))));
        } catch (const std::exception&) {
            w = h = 1;
        }
    }
    const auto snap = [&](double v) { return std::max(multiple, static_cast<int>(std::lround(v / multiple)) * multiple); };
    if (w >= h) {
        return {snap(longSide), snap(static_cast<double>(longSide) * h / w)};
    }
    return {snap(static_cast<double>(longSide) * w / h), snap(longSide)};
}

std::string sniffAudioType(std::span<const std::uint8_t> bytes) {
    const std::string_view magic(reinterpret_cast<const char*>(bytes.data()), std::min<std::size_t>(16, bytes.size()));
    const auto extension = generatedFileExtension({}, magic, GeneratedMediaKind::SoundEffect);
    if (!extension) {
        return {};
    }
    if (*extension == ".wav") {
        return "audio/wav";
    }
    if (*extension == ".mp3") {
        return "audio/mpeg";
    }
    if (*extension == ".flac") {
        return "audio/flac";
    }
    if (*extension == ".ogg") {
        return "audio/ogg";
    }
    return "audio/mp4";
}

std::optional<std::int64_t> audioMilliseconds(std::span<const std::uint8_t> bytes) {
    if (auto wav = wavMilliseconds(bytes)) {
        return wav;
    }
    const std::int64_t mp3 = mp3Milliseconds(bytes);
    return mp3 > 0 ? std::optional<std::int64_t>(mp3) : std::nullopt;
}

} // namespace wabcut::ai::net
