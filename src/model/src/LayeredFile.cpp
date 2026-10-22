#include <wabcut/model/LayeredFile.h>

#include <wabcut/model/MediaLocator.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <system_error>

namespace wabcut {

namespace {

constexpr std::string_view kLayers = "?layers=";
constexpr std::string_view kSize = "&size=";

std::string lowered(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

} // namespace

bool isLayeredFileExtension(std::string_view extension) {
    const std::string e = lowered(extension);
    return e == ".psd" || e == ".psb";
}

std::string layeredFilePath(const LayeredFileSpec& spec) {
    if (spec.layers.size() > kMaxLayeredFileLayers || spec.file.empty() ||
        !isLayeredFileExtension(pathToUtf8(spec.file.extension()))) {
        return {};
    }
    std::string out = pathToUtf8(spec.file) + std::string(kLayers);
    if (spec.layers.empty()) {
        out += "all";
    }
    for (std::size_t i = 0; i < spec.layers.size(); ++i) {
        if (spec.layers[i] < 0 || spec.layers[i] >= static_cast<int>(kMaxLayeredFileLayers)) {
            return {};
        }
        if (i > 0) {
            out += ',';
        }
        out += std::to_string(spec.layers[i]);
    }
    out += kSize;
    out += spec.documentSize ? "document" : "layer";
    return out;
}

std::optional<LayeredFileSpec> parseLayeredFilePath(std::string_view utf8) {
    const std::size_t query = utf8.rfind(kLayers);
    if (query == std::string_view::npos || query == 0 || utf8.size() > 64 * 1024) {
        return std::nullopt;
    }
    LayeredFileSpec spec;
    spec.file = pathFromUtf8(std::string(utf8.substr(0, query)));
    if (!isLayeredFileExtension(pathToUtf8(spec.file.extension()))) {
        return std::nullopt;
    }
    const std::string_view rest = utf8.substr(query + kLayers.size());
    const std::size_t sizeAt = rest.find(kSize);
    if (sizeAt == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string_view size = rest.substr(sizeAt + kSize.size());
    if (size == "document") {
        spec.documentSize = true;
    } else if (size == "layer") {
        spec.documentSize = false;
    } else {
        return std::nullopt;
    }
    std::string_view list = rest.substr(0, sizeAt);
    if (list == "all") {
        return spec;
    }
    while (!list.empty()) {
        if (spec.layers.size() >= kMaxLayeredFileLayers) {
            return std::nullopt;
        }
        const std::size_t comma = list.find(',');
        const std::string_view item = list.substr(0, comma);
        int value = -1;
        const auto [end, ec] = std::from_chars(item.data(), item.data() + item.size(), value);
        if (item.empty() || item.size() > 5 || ec != std::errc{} || end != item.data() + item.size() || value < 0 ||
            value >= static_cast<int>(kMaxLayeredFileLayers)) {
            return std::nullopt;
        }
        spec.layers.push_back(value);
        list = comma == std::string_view::npos ? std::string_view{} : list.substr(comma + 1);
        if (comma != std::string_view::npos && list.empty()) {
            return std::nullopt;
        }
    }
    if (spec.layers.empty()) {
        return std::nullopt;
    }
    return spec;
}

} // namespace wabcut
