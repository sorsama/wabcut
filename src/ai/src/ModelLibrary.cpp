#include <wabcut/ai/ModelLibrary.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>

namespace wabcut::ai {
namespace detail {
extern const unsigned char kLocalBenchmarksJson[];
extern const std::size_t kLocalBenchmarksJsonSize;
} // namespace detail
} // namespace wabcut::ai

namespace wabcut::ai::library {

namespace {

constexpr std::string_view kHub = "https://huggingface.co";

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string upper(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

bool iequals(std::string_view a, std::string_view b) { return equalsIgnoreCase(a, b); }

bool contains(std::string_view haystack, std::string_view needle) { return haystack.find(needle) != std::string_view::npos; }

bool hasTag(const HubModel& m, std::string_view tag) {
    return std::any_of(m.tags.begin(), m.tags.end(), [&](const std::string& t) { return iequals(t, tag); });
}

bool isHex64(std::string_view s) {
    return s.size() == 64 && std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

bool isHex40(std::string_view s) {
    return s.size() == 40 && std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

/// RFC 3986 unreserved characters stay, everything else is %XX.
std::string percentEncode(std::string_view s) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') {
            out.push_back(ch);
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}

std::string_view namePart(std::string_view repo) {
    const auto slash = repo.find('/');
    return slash == std::string_view::npos ? repo : repo.substr(slash + 1);
}

std::string_view ownerPart(std::string_view repo) {
    const auto slash = repo.find('/');
    return slash == std::string_view::npos ? std::string_view{} : repo.substr(0, slash);
}

std::vector<std::string> stringsOf(const Json* j) {
    std::vector<std::string> out;
    if (!j)
        return out;
    if (j->is_string()) {
        out.push_back(j->get<std::string>());
    } else if (j->is_array()) {
        for (const Json& item : *j) {
            if (item.is_string())
                out.push_back(item.get<std::string>());
        }
    }
    return out;
}

void addUnique(std::vector<std::string>& list, std::string value) {
    if (value.empty())
        return;
    if (std::none_of(list.begin(), list.end(), [&](const std::string& v) { return iequals(v, value); }))
        list.push_back(std::move(value));
}

AiError badJson(std::string what) { return AiError(AiErrorCode::ProviderError, "Hugging Face answered " + std::move(what)); }

/// A number like "1.7" at text[pos...]; advances pos.
std::optional<double> readNumber(std::string_view text, std::size_t& pos) {
    const std::size_t start = pos;
    while (pos < text.size() && (std::isdigit(static_cast<unsigned char>(text[pos])) || text[pos] == '.'))
        ++pos;
    if (pos == start)
        return std::nullopt;
    double value = 0.0;
    const auto [end, ec] = std::from_chars(text.data() + start, text.data() + pos, value);
    if (ec != std::errc() || end != text.data() + pos)
        return std::nullopt;
    return value;
}

/// The size of a parameter token: "8B" -> 8, "1.7B" -> 1.7, "E4B" -> 4, "360M" -> 0.36, "8x7B" -> 56; nullopt otherwise.
std::optional<double> paramToken(std::string_view token) {
    std::size_t pos = 0;
    if (!token.empty() && (token[0] == 'E' || token[0] == 'e'))
        pos = 1; // Gemma's "effective" sizes (E2B, E4B)
    auto first = readNumber(token, pos);
    if (!first)
        return std::nullopt;
    double value = *first;
    if (pos < token.size() && (token[pos] == 'x' || token[pos] == 'X')) {
        ++pos;
        auto second = readNumber(token, pos);
        if (!second)
            return std::nullopt;
        value *= *second;
    }
    if (pos + 1 != token.size())
        return std::nullopt;
    const char unit = token[pos];
    if (unit == 'B' || unit == 'b')
        return value;
    if (unit == 'M' || unit == 'm')
        return value / 1000.0;
    return std::nullopt;
}

std::string metricKey(std::string_view text) {
    std::string out;
    for (const char c : text) {
        if (std::isalnum(static_cast<unsigned char>(c)))
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

/// Model-index metric names onto the table's ids.
std::string metricIdFor(std::string_view name) {
    const std::string k = metricKey(name);
    if (k.starts_with("mmlupro"))
        return "mmlu_pro";
    if (k.starts_with("gpqa"))
        return "gpqa_diamond";
    if (k.starts_with("ifeval"))
        return "ifeval";
    if (k.starts_with("livecodebench"))
        return "livecodebench";
    if (k.starts_with("aime2025") || k == "aime25")
        return "aime25";
    return {};
}

} // namespace

// ---- Enums ----------------------------------------------------------------------------------------------------------

std::string_view toString(Task task) {
    switch (task) {
    case Task::Chat: return "chat";
    case Task::Coding: return "coding";
    case Task::Reasoning: return "reasoning";
    case Task::Vision: return "vision";
    case Task::Transcription: return "transcription";
    case Task::Translation: return "translation";
    }
    return "chat";
}

std::optional<Task> taskFromString(std::string_view text) {
    for (const Task t : {Task::Chat, Task::Coding, Task::Reasoning, Task::Vision, Task::Transcription, Task::Translation}) {
        if (toString(t) == text)
            return t;
    }
    return std::nullopt;
}

std::string_view toString(SortKey key) {
    switch (key) {
    case SortKey::Recommended: return "recommended";
    case SortKey::Benchmark: return "benchmark";
    case SortKey::Downloads: return "downloads";
    case SortKey::Likes: return "likes";
    case SortKey::Trending: return "trending";
    case SortKey::Newest: return "newest";
    case SortKey::Updated: return "updated";
    case SortKey::Smallest: return "smallest";
    }
    return "recommended";
}

std::optional<SortKey> sortKeyFromString(std::string_view text) {
    for (const SortKey k : {SortKey::Recommended, SortKey::Benchmark, SortKey::Downloads, SortKey::Likes, SortKey::Trending,
                            SortKey::Newest, SortKey::Updated, SortKey::Smallest}) {
        if (toString(k) == text)
            return k;
    }
    return std::nullopt;
}

std::string_view toString(Publisher publisher) {
    switch (publisher) {
    case Publisher::Official: return "official";
    case Publisher::Quantizer: return "quantizer";
    case Publisher::Community: return "community";
    }
    return "community";
}

// ---- Hub ------------------------------------------------------------------------------------------------------------

bool isValidRepoId(std::string_view repo) {
    const auto slash = repo.find('/');
    if (slash == std::string_view::npos || repo.find('/', slash + 1) != std::string_view::npos)
        return false;
    for (const std::string_view part : {repo.substr(0, slash), repo.substr(slash + 1)}) {
        if (part.empty() || part.size() > 96 || part == "." || part == ".." || part.back() == '.' || part.front() == '-')
            return false;
        for (const char c : part) {
            if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.'))
                return false;
        }
    }
    return true;
}

bool isSafeRepoPath(std::string_view path) {
    if (path.empty() || path.size() > 300 || path.front() == '/' || path.back() == '/')
        return false;
    for (const char c : path) {
        if (static_cast<unsigned char>(c) < 0x20 || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
            c == '>' || c == '|' || c == 0x7f)
            return false;
    }
    std::size_t start = 0;
    while (true) {
        const std::size_t slash = path.find('/', start);
        const std::string_view part = path.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
        if (part.empty() || part == "." || part == ".." || part.back() == '.' || part.back() == ' ')
            return false;
        if (slash == std::string_view::npos)
            return true;
        start = slash + 1;
    }
}

std::string hubSearchUrl(const HubQuery& query) {
    std::string sort = "downloads";
    switch (query.sort) {
    case SortKey::Likes: sort = "likes"; break;
    case SortKey::Trending: sort = "trendingScore"; break;
    case SortKey::Newest: sort = "createdAt"; break;
    case SortKey::Updated: sort = "lastModified"; break;
    default: break;
    }
    std::string url = std::string(kHub) + "/api/models?filter=gguf";
    if (!query.search.empty())
        url += "&search=" + percentEncode(query.search.substr(0, 120));
    if (!query.author.empty() && isValidRepoId(query.author + "/x"))
        url += "&author=" + percentEncode(query.author);
    url += "&sort=" + sort + "&direction=-1&limit=" + std::to_string(std::clamp(query.limit, 1, 100));
    if (query.minParamsB > 0.0 || query.maxParamsB > 0.0) {
        std::string range;
        if (query.minParamsB > 0.0)
            range += "min:" + std::to_string(static_cast<long long>(std::floor(query.minParamsB))) + "B";
        if (query.maxParamsB > 0.0)
            range += std::string(range.empty() ? "" : ",") + "max:" + std::to_string(static_cast<long long>(std::ceil(query.maxParamsB))) + "B";
        url += "&num_parameters=" + percentEncode(range);
    }
    for (const char* field : {"author", "downloads", "likes", "trendingScore", "lastModified", "createdAt", "tags",
                              "pipeline_tag", "gated", "cardData", "gguf", "baseModels"}) {
        url += "&expand%5B%5D=";
        url += field;
    }
    return url;
}

std::string hubModelUrl(std::string_view repo) { return std::string(kHub) + "/api/models/" + std::string(repo) + "?blobs=true"; }

std::string hubTreeUrl(std::string_view repo, std::string_view revision) {
    return std::string(kHub) + "/api/models/" + std::string(repo) + "/tree/" + percentEncode(revision) + "?recursive=true";
}

std::string hubPageUrl(std::string_view repo) { return std::string(kHub) + "/" + std::string(repo); }

std::string hubFileUrl(std::string_view repo, std::string_view revision, std::string_view path) {
    std::string url = std::string(kHub) + "/" + std::string(repo) + "/resolve/" + percentEncode(revision) + "/";
    std::size_t start = 0;
    while (true) {
        const std::size_t slash = path.find('/', start);
        url += percentEncode(path.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start));
        if (slash == std::string_view::npos)
            break;
        url += '/';
        start = slash + 1;
    }
    return url;
}

std::optional<std::string> nextPageUrl(const HttpHeaders& headers) {
    const auto link = findHeader(headers, "link");
    if (!link)
        return std::nullopt;
    std::string_view rest = *link;
    while (!rest.empty()) {
        const auto open = rest.find('<');
        const auto close = rest.find('>', open == std::string_view::npos ? 0 : open);
        if (open == std::string_view::npos || close == std::string_view::npos)
            return std::nullopt;
        const std::string_view url = rest.substr(open + 1, close - open - 1);
        const auto comma = rest.find(',', close);
        const std::string_view params = rest.substr(close + 1, comma == std::string_view::npos ? std::string_view::npos : comma - close - 1);
        if (contains(lower(params), "rel=\"next\"") || contains(lower(params), "rel=next")) {
            const auto parts = parseHttpUrl(url);
            if (parts && parts->scheme == "https" && parts->host == "huggingface.co" && !parts->port &&
                parts->path.starts_with("/api/models?"))
                return std::string(url);
            return std::nullopt;
        }
        if (comma == std::string_view::npos)
            break;
        rest = rest.substr(comma + 1);
    }
    return std::nullopt;
}

double paramsFromName(std::string_view name, double* activeParamsB) {
    double total = 0.0;
    double active = 0.0;
    std::size_t start = 0;
    const std::string_view base = namePart(name);
    while (start <= base.size()) {
        std::size_t end = base.find_first_of("-_ ", start);
        if (end == std::string_view::npos)
            end = base.size();
        const std::string_view token = base.substr(start, end - start);
        if (!token.empty()) {
            if ((token[0] == 'A' || token[0] == 'a') && token.size() > 1 && std::isdigit(static_cast<unsigned char>(token[1]))) {
                if (auto v = paramToken(token.substr(1)); v && total > 0.0)
                    active = *v;
            } else if (auto v = paramToken(token); v && total == 0.0) {
                total = *v;
            }
        }
        start = end + 1;
    }
    if (activeParamsB)
        *activeParamsB = active;
    return total;
}

std::string normalizeLicense(std::string_view hubLicense) {
    static const std::map<std::string, std::string, std::less<>> kSpdx{
        {"apache-2.0", "Apache-2.0"}, {"mit", "MIT"},     {"bsd-2-clause", "BSD-2-Clause"}, {"bsd-3-clause", "BSD-3-Clause"},
        {"isc", "ISC"},               {"zlib", "Zlib"},   {"cc-by-4.0", "CC-BY-4.0"},       {"bsd", "BSD-3-Clause"},
    };
    const std::string key = lower(hubLicense);
    if (const auto it = kSpdx.find(key); it != kSpdx.end())
        return it->second;
    for (const auto& [id, spdx] : kSpdx) {
        if (iequals(spdx, hubLicense))
            return spdx;
    }
    return key;
}

std::string licenseDisplayName(std::string_view license, std::string_view cardName) {
    static const std::map<std::string, std::string, std::less<>> kNames{
        {"Apache-2.0", "Apache License 2.0"},
        {"MIT", "MIT License"},
        {"BSD-2-Clause", "BSD 2-Clause License"},
        {"BSD-3-Clause", "BSD 3-Clause License"},
        {"ISC", "ISC License"},
        {"Zlib", "zlib License"},
        {"CC-BY-4.0", "Creative Commons Attribution 4.0"},
        {"llama2", "Llama 2 Community License"},
        {"llama3", "Llama 3 Community License"},
        {"llama3.1", "Llama 3.1 Community License"},
        {"llama3.2", "Llama 3.2 Community License"},
        {"llama3.3", "Llama 3.3 Community License"},
        {"llama4", "Llama 4 Community License"},
        {"gemma", "Gemma Terms of Use"},
        {"openrail", "OpenRAIL License"},
        {"creativeml-openrail-m", "CreativeML OpenRAIL-M"},
        {"cc-by-nc-4.0", "Creative Commons Attribution-NonCommercial 4.0 (non-commercial)"},
        {"cc-by-nc-sa-4.0", "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 (non-commercial)"},
        {"cc-by-sa-4.0", "Creative Commons Attribution-ShareAlike 4.0"},
        {"gpl-3.0", "GNU GPL v3"},
        {"agpl-3.0", "GNU AGPL v3"},
        {"openmdw-1.1", "OpenMDW License 1.1"},
    };
    if (const auto it = kNames.find(license); it != kNames.end())
        return it->second;
    if (!cardName.empty())
        return std::string(cardName);
    if (license.empty() || license == "unknown")
        return "Licence not stated";
    if (license == "other")
        return "Custom licence";
    return std::string(license);
}

bool isPermissive(std::string_view license) { return isAllowedModelLicense(license); }

namespace {

void readCommon(const Json& j, HubModel& m) {
    m.id = jsonStringOr(j, "id", jsonStringOr(j, "modelId"));
    m.author = jsonStringOr(j, "author", std::string(ownerPart(m.id)));
    if (const auto sha = jsonString(j, "sha"); sha && isHex40(*sha))
        m.revision = *sha;
    m.downloads = std::max<std::int64_t>(0, jsonIntOr(j, "downloads", 0));
    m.likes = std::max<std::int64_t>(0, jsonIntOr(j, "likes", 0));
    m.trending = jsonNumberOr(j, "trendingScore", 0.0);
    m.createdAt = jsonStringOr(j, "createdAt");
    m.lastModified = jsonStringOr(j, "lastModified");
    m.tags = stringsOf(jsonMember(j, "tags"));
    m.pipelineTag = jsonStringOr(j, "pipeline_tag");
    if (const Json* gated = jsonMember(j, "gated")) {
        m.gated = (gated->is_boolean() && gated->get<bool>()) || (gated->is_string() && !gated->get<std::string>().empty() &&
                                                                  gated->get<std::string>() != "false");
    }

    std::string license;
    std::string cardLicenseName;
    std::string relation;
    if (const Json* card = jsonObject(j, "cardData")) {
        license = jsonStringOr(*card, "license");
        if (license.empty()) {
            if (const auto list = stringsOf(jsonMember(*card, "license")); !list.empty())
                license = list.front();
        }
        cardLicenseName = jsonStringOr(*card, "license_name");
        for (std::string& lang : stringsOf(jsonMember(*card, "language"))) {
            if (lang.size() >= 2 && lang.size() <= 3 && std::all_of(lang.begin(), lang.end(), [](char c) { return std::isalpha(static_cast<unsigned char>(c)); }))
                addUnique(m.languages, lower(lang));
        }
        for (std::string& base : stringsOf(jsonMember(*card, "base_model")))
            addUnique(m.baseModels, std::move(base));
        relation = jsonStringOr(*card, "base_model_relation");
        if (m.pipelineTag.empty())
            m.pipelineTag = jsonStringOr(*card, "pipeline_tag");
    }
    if (const Json* base = jsonObject(j, "baseModels")) {
        if (relation.empty())
            relation = jsonStringOr(*base, "relation");
        if (const Json* models = jsonArray(*base, "models")) {
            for (const Json& item : *models) {
                if (item.is_object())
                    addUnique(m.baseModels, jsonStringOr(item, "id"));
            }
        }
    }
    for (const std::string& tag : m.tags) {
        if (license.empty() && tag.starts_with("license:"))
            license = tag.substr(8);
        if (tag.starts_with("base_model:")) {
            std::string rest = tag.substr(11);
            const auto colon = rest.find(':');
            if (colon != std::string::npos) {
                if (relation.empty())
                    relation = rest.substr(0, colon);
                rest = rest.substr(colon + 1);
            }
            addUnique(m.baseModels, std::move(rest));
        }
    }
    std::erase_if(m.baseModels, [](const std::string& b) { return !isValidRepoId(b); });
    m.baseRelation = lower(relation);
    m.license = normalizeLicense(license);
    m.licenseName = licenseDisplayName(m.license, cardLicenseName);

    if (const Json* gguf = jsonObject(j, "gguf")) {
        if (const auto total = jsonNumber(*gguf, "total"); total && *total > 0.0)
            m.paramsB = *total / 1e9;
        m.architecture = jsonStringOr(*gguf, "architecture");
        m.contextLength = std::max<std::int64_t>(0, jsonIntOr(*gguf, "context_length", 0));
        if (const auto size = jsonInt(*gguf, "totalFileSize"); size && *size > 0)
            m.ggufFileSize = static_cast<std::uint64_t>(*size);
    }
    double active = 0.0;
    const double named = paramsFromName(m.id, &active);
    if (m.paramsB <= 0.0)
        m.paramsB = named;
    m.activeParamsB = active;

    if (const Json* index = jsonArray(j, "model-index")) {
        for (const Json& entry : *index) {
            const Json* results = entry.is_object() ? jsonArray(entry, "results") : nullptr;
            if (!results)
                continue;
            for (const Json& result : *results) {
                const Json* metrics = result.is_object() ? jsonArray(result, "metrics") : nullptr;
                if (!metrics)
                    continue;
                std::string dataset;
                if (const Json* ds = jsonObject(result, "dataset"))
                    dataset = jsonStringOr(*ds, "name", jsonStringOr(*ds, "type"));
                for (const Json& metric : *metrics) {
                    if (!metric.is_object())
                        continue;
                    const auto value = jsonNumber(metric, "value");
                    if (!value)
                        continue;
                    EvalResult r;
                    r.dataset = dataset;
                    r.metric = jsonStringOr(metric, "name", jsonStringOr(metric, "type"));
                    r.value = *value <= 1.0 ? *value * 100.0 : *value; // fractions or percent
                    if (r.value >= 0.0 && r.value <= 100.0)
                        m.evals.push_back(std::move(r));
                }
            }
        }
    }
}

std::optional<HubFile> fileOf(std::string path, const Json* size, const Json* lfs, std::string_view shaKey) {
    if (!isSafeRepoPath(path))
        return std::nullopt;
    HubFile f;
    f.path = std::move(path);
    if (const auto s = asInt(size); s && *s > 0)
        f.size = static_cast<std::uint64_t>(*s);
    if (lfs && lfs->is_object()) {
        if (const auto sha = jsonString(*lfs, shaKey); sha && isHex64(*sha))
            f.sha256 = *sha;
        if (const auto s = jsonInt(*lfs, "size"); s && *s > 0)
            f.size = static_cast<std::uint64_t>(*s);
    }
    return f;
}

} // namespace

std::expected<std::vector<HubModel>, AiError> parseHubSearch(std::string_view json) {
    const Json j = Json::parse(json, nullptr, false);
    if (!j.is_array())
        return std::unexpected(badJson("something other than a model list"));
    std::vector<HubModel> out;
    for (const Json& item : j) {
        if (!item.is_object())
            continue;
        HubModel m;
        readCommon(item, m);
        if (isValidRepoId(m.id))
            out.push_back(std::move(m));
    }
    return out;
}

std::expected<HubModel, AiError> parseHubModel(std::string_view json) {
    const Json j = Json::parse(json, nullptr, false);
    if (!j.is_object())
        return std::unexpected(badJson("something other than a model"));
    HubModel m;
    readCommon(j, m);
    if (!isValidRepoId(m.id))
        return std::unexpected(badJson("a model without a valid id"));
    if (const Json* siblings = jsonArray(j, "siblings")) {
        for (const Json& s : *siblings) {
            if (!s.is_object())
                continue;
            if (auto f = fileOf(jsonStringOr(s, "rfilename"), jsonMember(s, "size"), jsonMember(s, "lfs"), "sha256"))
                m.files.push_back(std::move(*f));
        }
    }
    return m;
}

std::expected<std::vector<HubFile>, AiError> parseHubTree(std::string_view json) {
    const Json j = Json::parse(json, nullptr, false);
    if (!j.is_array())
        return std::unexpected(badJson("something other than a file list"));
    std::vector<HubFile> out;
    for (const Json& e : j) {
        if (!e.is_object() || jsonStringOr(e, "type") != "file")
            continue;
        if (auto f = fileOf(jsonStringOr(e, "path"), jsonMember(e, "size"), jsonMember(e, "lfs"), "oid"))
            out.push_back(std::move(*f));
    }
    return out;
}

// ---- Variants -------------------------------------------------------------------------------------------------------

std::string quantOf(std::string_view fileName) {
    const auto slash = fileName.rfind('/');
    std::string base(fileName.substr(slash == std::string_view::npos ? 0 : slash + 1));
    if (base.size() > 5 && iequals(std::string_view(base).substr(base.size() - 5), ".gguf"))
        base.resize(base.size() - 5);
    static const std::regex kShard("-[0-9]{5}-of-[0-9]{5}$", std::regex::icase);
    base = std::regex_replace(base, kShard, "");
    static const std::regex kQuant(
        "(?:^|[-_.])((?:UD-)?(?:I?Q[1-8](?:_[0-9A-Z]{1,3}){0,3}|BF16|F16|F32|FP16|MXFP4(?:_MOE)?|NVFP4|TQ[12]_0))(?=$|[-_.])",
        std::regex::icase);
    std::string found;
    for (auto it = std::sregex_iterator(base.begin(), base.end(), kQuant); it != std::sregex_iterator(); ++it)
        found = (*it)[1].str();
    return upper(found);
}

double bitsOf(std::string_view quantText) {
    std::string q = upper(quantText);
    if (q.starts_with("UD-"))
        q = q.substr(3);
    if (q == "F32")
        return 32.0;
    if (q == "F16" || q == "BF16" || q == "FP16")
        return 16.0;
    if (q.starts_with("MXFP4") || q == "NVFP4")
        return 4.25;
    if (q == "TQ1_0")
        return 1.7;
    if (q == "TQ2_0")
        return 2.1;
    const bool importance = q.starts_with("IQ");
    const std::size_t digit = importance ? 2 : (q.starts_with("Q") ? 1 : std::string::npos);
    if (digit == std::string::npos || digit >= q.size() || !std::isdigit(static_cast<unsigned char>(q[digit])))
        return 0.0;
    switch (q[digit]) {
    case '1': return 1.75;
    case '2': return 2.7;
    case '3': return 3.6;
    case '4': return q.find("_K") != std::string::npos ? 4.8 : 4.5;
    case '5': return 5.6;
    case '6': return 6.6;
    case '8': return 8.5;
    default: return 0.0;
    }
}

namespace {

bool helperFile(std::string_view path) {
    const std::string l = lower(path);
    const auto slash = l.rfind('/');
    const std::string_view file = std::string_view(l).substr(slash == std::string::npos ? 0 : slash + 1);
    if (file.starts_with("mmproj") || contains(file, "imatrix") || file.starts_with("mtp-") || file.starts_with("mtp_"))
        return true;
    return l.starts_with("mtp/") || contains(l, "/mtp/");
}

bool isGguf(std::string_view path) {
    return path.size() > 5 && iequals(path.substr(path.size() - 5), ".gguf");
}

} // namespace

std::vector<Variant> variantsOf(std::span<const HubFile> files) {
    static const std::regex kShard("^(.*)-([0-9]{5})-of-([0-9]{5})\\.gguf$", std::regex::icase);
    struct Group {
        std::map<int, HubFile> shards;
        int count = 1;
        std::string key;
    };
    std::map<std::string, Group> groups;
    for (const HubFile& f : files) {
        if (!isGguf(f.path) || helperFile(f.path) || !isHex64(f.sha256) || f.size == 0)
            continue;
        std::smatch m;
        if (std::regex_match(f.path, m, kShard)) {
            Group& g = groups[lower(m[1].str())];
            g.key = m[1].str();
            g.count = std::stoi(m[3].str());
            g.shards[std::stoi(m[2].str())] = f;
        } else {
            Group& g = groups[lower(f.path)];
            g.key = f.path.substr(0, f.path.size() - 5);
            g.shards[1] = f;
        }
    }
    std::vector<Variant> out;
    for (auto& [key, g] : groups) {
        if (g.count < 1 || g.count > 256 || static_cast<int>(g.shards.size()) != g.count || g.shards.begin()->first != 1)
            continue; // an incomplete split file
        Variant v;
        for (auto& [index, f] : g.shards) {
            v.bytes += f.size;
            v.files.push_back(f);
        }
        v.quant = quantOf(v.files.front().path);
        if (v.quant.empty()) {
            const auto slash = g.key.rfind('/');
            v.quant = g.key.substr(slash == std::string::npos ? 0 : slash + 1);
        }
        v.bits = bitsOf(v.quant);
        out.push_back(std::move(v));
    }
    // Two files of one quantization (a directory copy) keep the smaller name apart: disambiguate by path.
    std::stable_sort(out.begin(), out.end(), [](const Variant& a, const Variant& b) { return a.bytes < b.bytes; });
    std::map<std::string, int> seen;
    for (Variant& v : out) {
        if (++seen[v.quant] > 1) {
            const std::string& path = v.files.front().path;
            const auto slash = path.rfind('/');
            v.quant += " (" + (slash == std::string::npos ? path : path.substr(0, slash)) + ")";
        }
    }
    return out;
}

std::vector<HubFile> projectorsOf(std::span<const HubFile> files) {
    std::vector<HubFile> out;
    for (const HubFile& f : files) {
        const auto slash = f.path.rfind('/');
        const std::string name = lower(std::string_view(f.path).substr(slash == std::string::npos ? 0 : slash + 1));
        if (isGguf(f.path) && name.starts_with("mmproj") && isHex64(f.sha256) && f.size > 0)
            out.push_back(f);
    }
    return out;
}

const Variant* defaultVariant(std::span<const Variant> variants, std::string_view preferred) {
    if (variants.empty())
        return nullptr;
    auto byQuant = [&](std::string_view q) -> const Variant* {
        for (const Variant& v : variants) {
            if (iequals(v.quant, q))
                return &v;
        }
        return nullptr;
    };
    if (!preferred.empty()) {
        if (const Variant* v = byQuant(preferred))
            return v;
    }
    for (const std::string_view q : {"Q4_K_M", "UD-Q4_K_XL", "Q4_K_XL", "Q4_K_S", "UD-Q4_K_M", "IQ4_XS", "IQ4_NL", "Q4_0",
                                     "MXFP4", "MXFP4_MOE", "Q5_K_M", "UD-Q5_K_XL"}) {
        if (const Variant* v = byQuant(q))
            return v;
    }
    const Variant* best = nullptr;
    double bestDistance = 0.0;
    for (const Variant& v : variants) {
        if (v.bits <= 0.0)
            continue;
        const double d = std::abs(v.bits - 4.8);
        if (!best || d < bestDistance) {
            best = &v;
            bestDistance = d;
        }
    }
    return best ? best : &variants.front();
}

const HubFile* defaultProjector(std::span<const HubFile> projectors) {
    for (const std::string_view precision : {"F16", "BF16", "F32"}) {
        for (const HubFile& p : projectors) {
            if (quantOf(p.path) == precision)
                return &p;
        }
    }
    return projectors.empty() ? nullptr : &projectors.front();
}

// ---- Benchmarks -----------------------------------------------------------------------------------------------------

const Metric* BenchmarkTable::metric(std::string_view id) const {
    for (const Metric& m : metrics) {
        if (m.id == id)
            return &m;
    }
    return nullptr;
}

const BenchmarkFamily* BenchmarkTable::family(std::string_view repoId) const {
    for (const BenchmarkFamily& f : families) {
        if (iequals(f.id, repoId))
            return &f;
        for (const std::string& alias : f.aliases) {
            if (iequals(alias, repoId))
                return &f;
        }
    }
    return nullptr;
}

std::expected<BenchmarkTable, AiError> parseBenchmarks(std::string_view json) {
    const Json j = Json::parse(json, nullptr, false);
    auto fail = [](std::string what) { return std::unexpected(AiError(AiErrorCode::ProviderError, "benchmark table: " + std::move(what))); };
    if (!j.is_object() || jsonIntOr(j, "schemaVersion", 0) != 1)
        return fail("not a schemaVersion 1 document");
    BenchmarkTable table;
    table.updated = jsonStringOr(j, "updated");
    if (const Json* metrics = jsonArray(j, "metrics")) {
        for (const Json& m : *metrics) {
            Metric metric;
            metric.id = m.is_object() ? jsonStringOr(m, "id") : std::string();
            if (metric.id.empty() || table.metric(metric.id))
                return fail("a metric without an id, or twice");
            metric.name = jsonStringOr(m, "name", metric.id);
            metric.description = jsonStringOr(m, "description");
            metric.core = jsonBoolOr(m, "core", false);
            table.metrics.push_back(std::move(metric));
        }
    }
    const Json* families = jsonArray(j, "families");
    if (!families)
        return fail("no families");
    for (const Json& f : *families) {
        if (!f.is_object())
            return fail("a family that is not an object");
        BenchmarkFamily family;
        family.id = jsonStringOr(f, "id");
        if (!isValidRepoId(family.id) || table.family(family.id))
            return fail("family '" + family.id + "' has an invalid or repeated id");
        family.name = jsonStringOr(f, "name", std::string(namePart(family.id)));
        family.publisher = jsonStringOr(f, "publisher", std::string(ownerPart(family.id)));
        family.paramsB = jsonNumberOr(f, "paramsB", 0.0);
        family.activeParamsB = jsonNumberOr(f, "activeParamsB", 0.0);
        family.license = normalizeLicense(jsonStringOr(f, "license"));
        family.vision = jsonBoolOr(f, "vision", false);
        family.tasks = stringsOf(jsonMember(f, "tasks"));
        family.languages = stringsOf(jsonMember(f, "languages"));
        family.aliases = stringsOf(jsonMember(f, "aliases"));
        family.source = jsonStringOr(f, "source");
        family.released = jsonStringOr(f, "released");
        family.checked = jsonStringOr(f, "checked", table.updated);
        family.note = jsonStringOr(f, "note");
        if (!family.source.starts_with("https://"))
            return fail("family '" + family.id + "' has no https source");
        for (const std::string& task : family.tasks) {
            if (!taskFromString(task))
                return fail("family '" + family.id + "' has an unknown task '" + task + "'");
        }
        if (const Json* scores = jsonObject(f, "scores")) {
            for (const auto& [metricId, value] : scores->items()) {
                if (!table.metric(metricId))
                    return fail("family '" + family.id + "' scores an unknown metric '" + metricId + "'");
                Score s;
                s.metric = metricId;
                std::optional<double> v = value.is_object() ? jsonNumber(value, "value") : asNumber(&value);
                if (!v || !std::isfinite(*v) || *v < 0.0 || *v > 100.0)
                    return fail("family '" + family.id + "' has an invalid " + metricId + " score");
                s.value = *v;
                if (value.is_object()) {
                    s.mode = jsonStringOr(value, "mode");
                    s.note = jsonStringOr(value, "note");
                    s.source = jsonStringOr(value, "source");
                    if (!s.source.empty() && !s.source.starts_with("https://"))
                        return fail("family '" + family.id + "' has a " + metricId + " score without an https source");
                }
                family.scores.push_back(std::move(s));
            }
        }
        // In the table's metric order (the counted ones first), not the JSON object's alphabetical one.
        auto rank = [&](const Score& s) {
            for (std::size_t k = 0; k < table.metrics.size(); ++k) {
                if (table.metrics[k].id == s.metric)
                    return k;
            }
            return table.metrics.size();
        };
        std::stable_sort(family.scores.begin(), family.scores.end(), [&](const Score& a, const Score& b) { return rank(a) < rank(b); });
        table.families.push_back(std::move(family));
    }
    return table;
}

const BenchmarkTable& builtinBenchmarks() {
    static const BenchmarkTable table = [] {
        const std::string_view json(reinterpret_cast<const char*>(detail::kLocalBenchmarksJson), detail::kLocalBenchmarksJsonSize);
        auto parsed = parseBenchmarks(json);
        return parsed ? std::move(*parsed) : BenchmarkTable{};
    }();
    return table;
}

namespace {

/// A repository name without GGUF and quantization noise: "Qwen3-8B-GGUF" -> "qwen3-8b",
/// "Qwen3-8B-Q4_K_M-GGUF" -> "qwen3-8b".
std::string bareName(std::string_view repo) {
    std::string name = lower(namePart(repo));
    bool changed = true;
    while (changed) {
        changed = false;
        for (const std::string_view suffix : {"-gguf", "_gguf", ".gguf", "-ggml", "-imatrix", "-i1"}) {
            if (name.size() > suffix.size() && name.ends_with(suffix)) {
                name.resize(name.size() - suffix.size());
                changed = true;
            }
        }
        const std::string q = lower(quantOf(name + ".gguf"));
        if (!q.empty() && name.size() > q.size() + 1 && name.ends_with(q)) {
            name.resize(name.size() - q.size());
            while (!name.empty() && (name.back() == '-' || name.back() == '_' || name.back() == '.'))
                name.pop_back();
            changed = true;
        }
    }
    return name;
}

} // namespace

const BenchmarkFamily* familyForRepo(std::string_view repo, const BenchmarkTable& table) {
    if (const BenchmarkFamily* f = table.family(repo))
        return f;
    const std::string bare = bareName(repo);
    if (bare.empty())
        return nullptr;
    for (const BenchmarkFamily& f : table.families) {
        if (lower(namePart(f.id)) == bare)
            return &f;
        for (const std::string& alias : f.aliases) {
            if (lower(namePart(alias)) == bare)
                return &f;
        }
    }
    return nullptr;
}

const BenchmarkFamily* matchFamily(const HubModel& model, const BenchmarkTable& table) {
    if (const BenchmarkFamily* f = table.family(model.id))
        return f; // the base model itself
    if (!model.baseModels.empty()) {
        if (!model.baseRelation.empty() && model.baseRelation != "quantized")
            return nullptr; // a fine-tune, merge or adapter: the base model's scores do not apply
        for (const std::string& base : model.baseModels) {
            if (const BenchmarkFamily* f = familyForRepo(base, table))
                return f;
        }
        return nullptr;
    }
    return familyForRepo(model.id, table);
}

std::optional<double> compositeScore(const BenchmarkFamily& family, const BenchmarkTable& table) {
    double sum = 0.0;
    int n = 0;
    for (const Score& s : family.scores) {
        if (const Metric* m = table.metric(s.metric); m && m->core) {
            sum += s.value;
            ++n;
        }
    }
    if (n == 0)
        return std::nullopt;
    return sum / n;
}

std::optional<double> compositeScore(std::span<const EvalResult> evals, const BenchmarkTable& table) {
    std::map<std::string, double> best;
    for (const EvalResult& e : evals) {
        std::string id = metricIdFor(e.metric);
        if (id.empty())
            id = metricIdFor(e.dataset);
        const Metric* m = id.empty() ? nullptr : table.metric(id);
        if (!m || !m->core)
            continue;
        best[id] = std::max(best[id], e.value);
    }
    if (best.empty())
        return std::nullopt;
    double sum = 0.0;
    for (const auto& [id, v] : best)
        sum += v;
    return sum / static_cast<double>(best.size());
}

// ---- Entries ----------------------------------------------------------------------------------------------------------

bool isKnownQuantizer(std::string_view author) {
    static constexpr std::array<std::string_view, 10> kKnown{"unsloth",      "bartowski",   "lmstudio-community", "ggml-org",
                                                             "mradermacher", "MaziyarPanahi", "QuantFactory",     "TheBloke",
                                                             "second-state", "NexaAI"};
    return std::any_of(kKnown.begin(), kKnown.end(), [&](std::string_view k) { return iequals(k, author); });
}

bool isRunnableLlm(const HubModel& model) {
    const std::string lname = lower(model.id);
    if (!hasTag(model, "gguf") && !contains(lname, "gguf"))
        return false;
    static constexpr std::array<std::string_view, 24> kNotChat{
        "text-to-speech",     "automatic-speech-recognition", "text-to-image",     "image-to-image",
        "feature-extraction", "sentence-similarity",          "text-to-video",     "image-to-video",
        "text-to-audio",      "audio-to-audio",               "object-detection",  "image-segmentation",
        "depth-estimation",   "image-classification",         "audio-classification", "zero-shot-image-classification",
        "text-ranking",       "image-text-to-video",          "video-classification", "mask-generation",
        "translation",        "audio-text-to-text",           "text-to-3d",        "image-to-3d"};
    for (const std::string_view p : kNotChat) {
        if (iequals(model.pipelineTag, p) || hasTag(model, p))
            return false;
    }
    const std::string pipeline = lower(model.pipelineTag);
    if (pipeline == "text-generation" || pipeline == "image-text-to-text" || pipeline == "any-to-any")
        return true;
    if (!pipeline.empty())
        return false;
    return hasTag(model, "conversational") || hasTag(model, "text-generation") || contains(lname, "instruct") ||
           contains(lname, "-chat") || contains(lname, "-it-") || lname.ends_with("-it");
}

std::vector<Task> tasksOf(const HubModel& model, const BenchmarkFamily* family) {
    std::vector<Task> tasks{Task::Chat};
    const std::string lname = lower(namePart(model.id));
    auto familyHas = [&](std::string_view t) {
        return family && std::find(family->tasks.begin(), family->tasks.end(), t) != family->tasks.end();
    };
    auto add = [&](Task t) {
        if (std::find(tasks.begin(), tasks.end(), t) == tasks.end())
            tasks.push_back(t);
    };
    if (familyHas("coding") || contains(lname, "coder") || contains(lname, "code") || contains(lname, "devstral") ||
        hasTag(model, "code") || hasTag(model, "coding"))
        add(Task::Coding);
    if (familyHas("reasoning") || contains(lname, "thinking") || contains(lname, "reason") || contains(lname, "-r1") ||
        contains(lname, "qwq") || hasTag(model, "reasoning") || hasTag(model, "thinking"))
        add(Task::Reasoning);
    const std::string pipeline = lower(model.pipelineTag);
    if ((family && family->vision) || familyHas("vision") || pipeline == "image-text-to-text" || pipeline == "any-to-any" ||
        hasTag(model, "vision") || hasTag(model, "multimodal") || hasTag(model, "image-text-to-text") ||
        !projectorsOf(model.files).empty())
        add(Task::Vision);
    if (familyHas("translation") || hasTag(model, "translation") || contains(lname, "-mt") || contains(lname, "translat"))
        add(Task::Translation);
    return tasks;
}

Entry makeEntry(HubModel model, const BenchmarkTable& table) {
    Entry e;
    e.family = matchFamily(model, table);
    e.tasks = tasksOf(model, e.family);
    e.vision = std::find(e.tasks.begin(), e.tasks.end(), Task::Vision) != e.tasks.end();
    if (e.family) {
        e.score = compositeScore(*e.family, table);
    }
    if (!e.score) {
        e.score = compositeScore(model.evals, table);
        e.ownEvals = e.score.has_value();
    }
    const std::string_view author = model.author.empty() ? ownerPart(model.id) : std::string_view(model.author);
    bool official = e.family && iequals(ownerPart(e.family->id), author);
    for (const std::string& base : model.baseModels)
        official = official || iequals(ownerPart(base), author);
    e.publisher = official ? Publisher::Official : isKnownQuantizer(author) ? Publisher::Quantizer : Publisher::Community;
    if (model.license.empty() && e.family) {
        model.license = e.family->license; // the base model's licence when the repository does not state one
        model.licenseName = licenseDisplayName(model.license);
    }
    e.permissive = isPermissive(model.license);
    const std::vector<Variant> variants = variantsOf(model.files);
    for (const Variant& v : variants)
        e.quants.push_back(v.quant);
    if (const Variant* v = defaultVariant(variants)) {
        e.sizeBytes = v->bytes;
        e.quant = v->quant;
    } else {
        if (model.paramsB <= 0.0 && e.family)
            model.paramsB = e.family->paramsB;
        e.quant = quantOf(model.id + ".gguf");
        if (!e.quant.empty() && model.ggufFileSize > 0) {
            e.sizeBytes = model.ggufFileSize; // a repository of one quantization
        } else if (model.paramsB > 0.0) {
            e.sizeBytes = static_cast<std::uint64_t>(model.paramsB * 1e9 * (e.quant.empty() ? 4.8 : bitsOf(e.quant)) / 8.0);
            e.sizeEstimated = true;
        } else {
            e.sizeBytes = model.ggufFileSize;
            e.sizeEstimated = e.sizeBytes > 0;
        }
    }
    if (model.paramsB <= 0.0 && e.family)
        model.paramsB = e.family->paramsB;
    e.languages = model.languages;
    if (e.family) {
        for (const std::string& lang : e.family->languages)
            addUnique(e.languages, lang);
    }
    e.hub = std::move(model);
    return e;
}

namespace {

std::uint64_t estimatedSize(const Entry& e) {
    if (e.sizeBytes > 0)
        return e.sizeBytes;
    if (e.hub.paramsB > 0.0)
        return static_cast<std::uint64_t>(e.hub.paramsB * 1e9 * 4.8 / 8.0);
    return 0;
}

bool inBand(double value, std::string_view band, std::initializer_list<std::pair<std::string_view, std::pair<double, double>>> bands) {
    for (const auto& [name, range] : bands) {
        if (name == band)
            return value >= range.first && value < range.second;
    }
    return true;
}

} // namespace

bool matches(const Entry& e, const Filter& f) {
    if (f.installedOnly && !e.installed)
        return false;
    if (f.task && std::find(e.tasks.begin(), e.tasks.end(), *f.task) == e.tasks.end())
        return false;
    if (f.fitsOnly && e.fit == Fit::No)
        return false;
    if (f.size != "any") {
        const std::uint64_t bytes = estimatedSize(e);
        if (bytes == 0)
            return false;
        const double gb = static_cast<double>(bytes) / 1e9;
        if (!inBand(gb, f.size, {{"small", {0.0, 3.0}}, {"medium", {3.0, 8.0}}, {"large", {8.0, 20.0}}, {"huge", {20.0, 1e12}}}))
            return false;
    }
    if (f.params != "any") {
        if (e.hub.paramsB <= 0.0)
            return false;
        if (!inBand(e.hub.paramsB, f.params,
                    {{"tiny", {0.0, 3.0}}, {"small", {3.0, 9.0}}, {"medium", {9.0, 20.0}}, {"large", {20.0, 40.0}}, {"huge", {40.0, 1e9}}}))
            return false;
    }
    if (!f.quant.empty()) {
        if (!e.quants.empty()) {
            if (std::none_of(e.quants.begin(), e.quants.end(), [&](const std::string& q) { return iequals(q, f.quant); }))
                return false;
        } else if (!e.quant.empty() && !iequals(e.quant, f.quant)) {
            return false;
        }
    }
    if (f.permissiveOnly && !e.permissive)
        return false;
    if (!f.language.empty() &&
        std::none_of(e.languages.begin(), e.languages.end(), [&](const std::string& l) { return iequals(l, f.language) || l == "multilingual"; }))
        return false;
    if (f.publisher == "official" && e.publisher != Publisher::Official)
        return false;
    if (f.publisher == "trusted" && e.publisher == Publisher::Community)
        return false;
    return true;
}

void sortEntries(std::vector<Entry>& entries, SortKey key) {
    auto scoreOf = [](const Entry& e) { return e.score.value_or(-1.0); };
    auto fitRank = [](const Entry& e) {
        switch (e.fit) {
        case Fit::Gpu: return 3;
        case Fit::Cpu: return 2;
        case Fit::Unknown: return 1;
        case Fit::No: return 0;
        }
        return 1;
    };
    std::stable_sort(entries.begin(), entries.end(), [&](const Entry& a, const Entry& b) {
        switch (key) {
        case SortKey::Recommended:
            if (fitRank(a) != fitRank(b))
                return fitRank(a) > fitRank(b);
            if (a.curated != b.curated)
                return a.curated;
            if (scoreOf(a) != scoreOf(b))
                return scoreOf(a) > scoreOf(b);
            return a.hub.downloads > b.hub.downloads;
        case SortKey::Benchmark:
            if (scoreOf(a) != scoreOf(b))
                return scoreOf(a) > scoreOf(b);
            return a.hub.downloads > b.hub.downloads;
        case SortKey::Downloads: return a.hub.downloads > b.hub.downloads;
        case SortKey::Likes: return a.hub.likes > b.hub.likes;
        case SortKey::Trending: return a.hub.trending > b.hub.trending;
        case SortKey::Newest: return a.hub.createdAt > b.hub.createdAt;
        case SortKey::Updated: return a.hub.lastModified > b.hub.lastModified;
        case SortKey::Smallest: {
            const std::uint64_t sa = estimatedSize(a);
            const std::uint64_t sb = estimatedSize(b);
            if ((sa == 0) != (sb == 0))
                return sb == 0;
            return sa < sb;
        }
        }
        return false;
    });
}

// ---- Installed library ------------------------------------------------------------------------------------------------

std::string libraryModelId(std::string_view repo, std::string_view quant) {
    return "hf:" + std::string(repo) + ":" + std::string(quant);
}

std::string storeNameFor(std::string_view repo, std::string_view path) {
    if (!isValidRepoId(repo) || !isSafeRepoPath(path))
        return {};
    return "library/" + std::string(repo) + "/" + std::string(path);
}

std::expected<InstalledModel, AiError> planInstall(const HubModel& model, const Variant& variant, const HubFile* projector,
                                                   bool licenseAccepted, const BenchmarkTable& table) {
    auto invalid = [](std::string what) { return std::unexpected(AiError(AiErrorCode::InvalidRequest, std::move(what))); };
    if (!isValidRepoId(model.id))
        return invalid("not a Hugging Face model id");
    if (!isHex40(model.revision))
        return invalid("the model's revision is unknown");
    if (variant.files.empty())
        return invalid("no file to download");
    const std::string license = model.license;
    const bool allowList = license == "MIT" || license == "Apache-2.0";
    if (!isPermissive(license) && !licenseAccepted)
        return invalid("accept the model's licence (" + model.licenseName + ") first");
    InstalledModel out;
    out.repo = model.id;
    out.revision = model.revision;
    out.quant = variant.quant;
    out.id = libraryModelId(model.id, variant.quant);
    const BenchmarkFamily* family = matchFamily(model, table);
    std::string bare(namePart(model.id));
    for (const std::string_view suffix : {"-GGUF", "-gguf", "_GGUF", "-Gguf"}) {
        if (bare.size() > suffix.size() && bare.ends_with(suffix))
            bare.resize(bare.size() - suffix.size());
    }
    out.name = family ? family->name : bare;
    out.publisher = model.author.empty() ? std::string(ownerPart(model.id)) : model.author;
    out.license = license;
    out.licenseName = model.licenseName;
    out.licenseAccepted = !allowList; // outside MIT / Apache-2.0: on the allow-list, or accepted just now
    out.paramsB = model.paramsB > 0.0 ? model.paramsB : (family ? family->paramsB : 0.0);
    out.contextLength = model.contextLength;
    out.family = family ? family->id : std::string();
    for (const Task t : tasksOf(model, family)) {
        if (t != Task::Vision)
            out.tasks.emplace_back(toString(t));
    }
    auto file = [&](const HubFile& f) -> std::expected<InstalledFile, AiError> {
        if (!isHex64(f.sha256) || f.size == 0)
            return std::unexpected(AiError(AiErrorCode::InvalidRequest, f.path + " has no SHA-256 to verify it"));
        InstalledFile i;
        i.storeName = storeNameFor(model.id, f.path);
        if (i.storeName.empty())
            return std::unexpected(AiError(AiErrorCode::InvalidRequest, "unsafe file name " + f.path));
        i.url = hubFileUrl(model.id, model.revision, f.path);
        i.size = f.size;
        i.sha256 = f.sha256;
        return i;
    };
    for (const HubFile& f : variant.files) {
        auto i = file(f);
        if (!i)
            return std::unexpected(i.error());
        out.files.push_back(std::move(*i));
    }
    if (projector) {
        auto i = file(*projector);
        if (!i)
            return std::unexpected(i.error());
        out.projector = std::move(*i);
        out.vision = true;
        out.tasks.emplace_back(toString(Task::Vision));
    }
    out.installedAt = formatDate(todayUtc());
    return out;
}

std::vector<ModelInfo> modelInfosOf(const InstalledModel& m) {
    std::vector<ModelInfo> out;
    if (m.files.empty())
        return out;
    const std::string card = m.name + " (" + m.repo + ", revision " + m.revision.substr(0, 12) + "), " + m.licenseName +
                             ". Downloaded from Hugging Face; runs on this computer through llama-server: nothing you ask "
                             "the assistant leaves it. " + hubPageUrl(m.repo);
    auto info = [&](std::string id, const InstalledFile& f, std::string description) {
        ModelInfo i;
        i.id = std::move(id);
        i.kind = ModelKind::Llm;
        i.fileName = f.storeName;
        i.url = f.url;
        i.size = f.size;
        i.sha256 = f.sha256;
        i.license = m.license;
        i.licenseCard = card;
        i.description = std::move(description);
        i.licenseAccepted = m.licenseAccepted;
        i.paramsB = m.paramsB;
        return i;
    };
    ModelInfo main = info(m.id, m.files.front(), m.name + " " + m.quant);
    for (std::size_t k = 1; k < m.files.size(); ++k) {
        main.parts.push_back(m.id + "#" + std::to_string(k + 1));
    }
    if (m.projector)
        main.projector = m.id + "#mmproj";
    out.push_back(main);
    for (std::size_t k = 1; k < m.files.size(); ++k)
        out.push_back(info(m.id + "#" + std::to_string(k + 1), m.files[k], m.name + " " + m.quant + ", part " + std::to_string(k + 1)));
    if (m.projector)
        out.push_back(info(m.id + "#mmproj", *m.projector, m.name + " vision projector"));
    return out;
}

ModelSpec registrySpecOf(const InstalledModel& m) {
    ModelSpec spec;
    spec.id = m.id;
    spec.provider = "local-llm";
    spec.name = m.name + " " + m.quant + " (local)";
    spec.api = "managed-llama";
    spec.endpoint = "/chat/completions";
    spec.caps.capabilities = {Capability::Chat};
    spec.caps.vision = m.projector.has_value();
    // Tool calls as one grammar-constrained answer per step (llama-server's JSON schema): any chat template works.
    spec.caps.toolsOnChat = true;
    spec.caps.constrainedTools = true;
    spec.caps.structuredOutput = true;
    spec.contextTokens = m.context > 0 ? m.context : 8192;
    spec.maxOutputTokens = 2048;
    spec.prices = {PriceRule{}};
    spec.license = m.license;
    spec.verified = false;
    spec.source = hubPageUrl(m.repo);
    spec.notes = "Downloaded from the local model library (" + m.repo + ", " + m.quant + ")";
    return spec;
}

std::uint64_t bytesOf(const InstalledModel& m) {
    std::uint64_t total = 0;
    for (const InstalledFile& f : m.files)
        total += f.size;
    if (m.projector)
        total += m.projector->size;
    return total;
}

namespace {

Json fileJson(const InstalledFile& f) {
    return Json{{"storeName", f.storeName}, {"url", f.url}, {"size", f.size}, {"sha256", f.sha256}};
}

std::expected<InstalledFile, AiError> fileFromJson(const Json& j, std::string_view repo) {
    InstalledFile f;
    if (!j.is_object())
        return std::unexpected(AiError(AiErrorCode::InvalidRequest, "a file entry is not an object"));
    f.storeName = jsonStringOr(j, "storeName");
    f.url = jsonStringOr(j, "url");
    f.sha256 = jsonStringOr(j, "sha256");
    const auto size = jsonInt(j, "size");
    const std::string prefix = "library/" + std::string(repo) + "/";
    if (!f.storeName.starts_with(prefix) || !isSafeRepoPath(f.storeName.substr(prefix.size())))
        return std::unexpected(AiError(AiErrorCode::InvalidRequest, "a file outside the model's folder"));
    if (!isHex64(f.sha256) || !size || *size <= 0)
        return std::unexpected(AiError(AiErrorCode::InvalidRequest, "a file without size or SHA-256"));
    const std::string urlPrefix = std::string(kHub) + "/" + std::string(repo) + "/resolve/";
    if (!f.url.starts_with(urlPrefix) || !parseHttpUrl(f.url))
        return std::unexpected(AiError(AiErrorCode::InvalidRequest, "a file that does not come from the model's repository"));
    f.size = static_cast<std::uint64_t>(*size);
    return f;
}

} // namespace

Json toJson(const InstalledModel& m) {
    Json files = Json::array();
    for (const InstalledFile& f : m.files)
        files.push_back(fileJson(f));
    Json j{{"id", m.id},
           {"repo", m.repo},
           {"revision", m.revision},
           {"quant", m.quant},
           {"name", m.name},
           {"publisher", m.publisher},
           {"license", m.license},
           {"licenseName", m.licenseName},
           {"licenseAccepted", m.licenseAccepted},
           {"paramsB", m.paramsB},
           {"contextLength", m.contextLength},
           {"vision", m.vision},
           {"tasks", m.tasks},
           {"family", m.family},
           {"files", std::move(files)},
           {"installedAt", m.installedAt},
           {"context", m.context},
           {"gpuLayers", m.gpuLayers},
           {"threads", m.threads}};
    if (m.projector)
        j["projector"] = fileJson(*m.projector);
    return j;
}

std::expected<InstalledModel, AiError> installedFromJson(const Json& j) {
    auto invalid = [](std::string what) { return std::unexpected(AiError(AiErrorCode::InvalidRequest, std::move(what))); };
    if (!j.is_object())
        return invalid("not an object");
    InstalledModel m;
    m.repo = jsonStringOr(j, "repo");
    m.quant = jsonStringOr(j, "quant");
    m.id = jsonStringOr(j, "id");
    m.revision = jsonStringOr(j, "revision");
    if (!isValidRepoId(m.repo) || m.quant.empty() || m.quant.size() > 120 || m.id != libraryModelId(m.repo, m.quant) ||
        !isHex40(m.revision))
        return invalid("entry '" + m.id + "' has an invalid id, repository or revision");
    m.name = jsonStringOr(j, "name", m.repo);
    m.publisher = jsonStringOr(j, "publisher");
    m.license = jsonStringOr(j, "license");
    m.licenseName = jsonStringOr(j, "licenseName", licenseDisplayName(m.license));
    m.licenseAccepted = jsonBoolOr(j, "licenseAccepted", false);
    if (!isPermissive(m.license) && !m.licenseAccepted)
        return invalid("entry '" + m.id + "' has a licence nobody accepted");
    m.paramsB = std::clamp(jsonNumberOr(j, "paramsB", 0.0), 0.0, 10000.0);
    m.contextLength = std::max<std::int64_t>(0, jsonIntOr(j, "contextLength", 0));
    m.vision = jsonBoolOr(j, "vision", false);
    m.tasks = stringsOf(jsonMember(j, "tasks"));
    std::erase_if(m.tasks, [](const std::string& t) { return !taskFromString(t); });
    m.family = jsonStringOr(j, "family");
    m.installedAt = jsonStringOr(j, "installedAt");
    m.context = static_cast<int>(std::clamp<std::int64_t>(jsonIntOr(j, "context", 0), 0, 131072));
    m.gpuLayers = static_cast<int>(std::clamp<std::int64_t>(jsonIntOr(j, "gpuLayers", -1), -1, 999));
    m.threads = static_cast<int>(std::clamp<std::int64_t>(jsonIntOr(j, "threads", 0), 0, 256));
    const Json* files = jsonArray(j, "files");
    if (!files || files->empty() || files->size() > 256)
        return invalid("entry '" + m.id + "' has no files");
    for (const Json& f : *files) {
        auto file = fileFromJson(f, m.repo);
        if (!file)
            return invalid("entry '" + m.id + "': " + file.error().message);
        m.files.push_back(std::move(*file));
    }
    if (const Json* p = jsonMember(j, "projector"); p && !p->is_null()) {
        auto file = fileFromJson(*p, m.repo);
        if (!file)
            return invalid("entry '" + m.id + "': " + file.error().message);
        m.projector = std::move(*file);
        m.vision = true;
    }
    return m;
}

LibraryIndex::LibraryIndex(std::filesystem::path storeRoot) : m_root(std::move(storeRoot)) {}

std::filesystem::path LibraryIndex::fileFor(const std::filesystem::path& storeRoot) { return storeRoot / "library" / "index.json"; }

std::expected<void, Error> LibraryIndex::load() {
    m_models.clear();
    const std::filesystem::path file = fileFor(m_root);
    std::error_code ec;
    if (!std::filesystem::exists(file, ec))
        return {};
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return std::unexpected(Error{ErrorCode::IoError, "cannot read the model library index"});
    std::stringstream text;
    text << in.rdbuf();
    const Json j = Json::parse(text.str(), nullptr, false);
    if (!j.is_object() || jsonIntOr(j, "schemaVersion", 0) != 1)
        return std::unexpected(Error{ErrorCode::InvalidArgument, "the model library index is damaged"});
    if (const Json* models = jsonArray(j, "models")) {
        for (const Json& item : *models) {
            auto m = installedFromJson(item);
            if (m && !find(m->id))
                m_models.push_back(std::move(*m));
        }
    }
    return {};
}

std::expected<void, Error> LibraryIndex::save() const {
    const std::filesystem::path file = fileFor(m_root);
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    if (ec)
        return std::unexpected(Error{ErrorCode::IoError, "cannot create the model library folder: " + ec.message()});
    Json models = Json::array();
    for (const InstalledModel& m : m_models)
        models.push_back(toJson(m));
    const Json doc{{"schemaVersion", 1}, {"models", std::move(models)}};
    std::filesystem::path temp = file;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out << doc.dump(2) << '\n';
        if (!out)
            return std::unexpected(Error{ErrorCode::IoError, "cannot write the model library index"});
    }
    std::filesystem::rename(temp, file, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        return std::unexpected(Error{ErrorCode::IoError, "cannot write the model library index"});
    }
    return {};
}

const InstalledModel* LibraryIndex::find(std::string_view id) const {
    for (const InstalledModel& m : m_models) {
        if (m.id == id)
            return &m;
    }
    return nullptr;
}

InstalledModel* LibraryIndex::find(std::string_view id) {
    for (InstalledModel& m : m_models) {
        if (m.id == id)
            return &m;
    }
    return nullptr;
}

void LibraryIndex::upsert(InstalledModel model) {
    if (InstalledModel* existing = find(model.id)) {
        *existing = std::move(model);
        return;
    }
    m_models.push_back(std::move(model));
}

bool LibraryIndex::remove(std::string_view id) {
    const auto before = m_models.size();
    std::erase_if(m_models, [&](const InstalledModel& m) { return m.id == id; });
    return m_models.size() != before;
}

std::vector<std::string> LibraryIndex::exclusiveFiles(std::string_view id) const {
    const InstalledModel* model = find(id);
    if (!model)
        return {};
    std::set<std::string> others;
    for (const InstalledModel& m : m_models) {
        if (m.id == id)
            continue;
        for (const InstalledFile& f : m.files)
            others.insert(f.storeName);
        if (m.projector)
            others.insert(m.projector->storeName);
    }
    std::vector<std::string> out;
    for (const InstalledFile& f : model->files) {
        if (!others.contains(f.storeName))
            out.push_back(f.storeName);
    }
    if (model->projector && !others.contains(model->projector->storeName))
        out.push_back(model->projector->storeName);
    return out;
}

} // namespace wabcut::ai::library
