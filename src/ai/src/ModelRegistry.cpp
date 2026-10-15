#include <wabcut/ai/ModelRegistry.h>
#include <wabcut/ai/JsonAccess.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <set>

namespace wabcut::ai {

namespace detail {
// Generated from src/ai/data/models.json at build time (cmake/EmbedFile.cmake).
extern const unsigned char kModelsJson[];
extern const std::size_t kModelsJsonSize;
} // namespace detail

namespace {

constexpr std::array<std::pair<Capability, std::string_view>, 7> kCapabilityNames{{
    {Capability::Chat, "chat"},
    {Capability::Transcribe, "transcribe"},
    {Capability::Embed, "embed"},
    {Capability::Speak, "speak"},
    {Capability::Translate, "translate"},
    {Capability::GenerateMedia, "generateMedia"},
    {Capability::CloneVoice, "cloneVoice"}, // consent only: not in kAllCapabilities
}};

constexpr std::array<Capability, 6> kAllCapabilities{Capability::Chat,  Capability::Transcribe, Capability::Embed,
                                                     Capability::Speak, Capability::Translate,  Capability::GenerateMedia};

constexpr std::array<std::pair<AuthScheme, std::string_view>, 7> kAuthNames{{
    {AuthScheme::None, "none"},
    {AuthScheme::Bearer, "bearer"},
    {AuthScheme::XApiKey, "x-api-key"},
    {AuthScheme::XiApiKey, "xi-api-key"},
    {AuthScheme::XGoogApiKey, "x-goog-api-key"},
    {AuthScheme::Token, "token"},
    {AuthScheme::RawAuthorization, "raw"},
}};

AiError invalid(const std::string& what) { return AiError(AiErrorCode::InvalidRequest, "models.json: " + what); }

std::string str(const Json& j, const char* key, std::string fallback = {}) {
    auto it = j.find(key);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::move(fallback);
}

bool flag(const Json& j, const char* key, bool fallback) {
    auto it = j.find(key);
    return (it != j.end() && it->is_boolean()) ? it->get<bool>() : fallback;
}

std::int64_t integer(const Json& j, const char* key) {
    auto it = j.find(key);
    return (it != j.end() && it->is_number()) ? it->get<std::int64_t>() : 0;
}

/// USD (a JSON number) -> micro-USD.
std::optional<MicroUsd> usd(const Json& j, const char* key) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_number()) {
        return std::nullopt;
    }
    return static_cast<MicroUsd>(std::llround(it->get<double>() * static_cast<double>(kMicroUsdPerUsd)));
}

std::expected<std::optional<std::chrono::year_month_day>, AiError> date(const Json& j, const char* key,
                                                                         const std::string& where) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) {
        return std::optional<std::chrono::year_month_day>();
    }
    if (!it->is_string()) {
        return std::unexpected(invalid(where + ": '" + key + "' must be a YYYY-MM-DD string"));
    }
    auto d = parseDate(it->get<std::string>());
    if (!d) {
        return std::unexpected(invalid(where + ": bad date in '" + key + "'"));
    }
    return d;
}

std::optional<AuthScheme> authFromString(std::string_view text) {
    for (const auto& [scheme, name] : kAuthNames) {
        if (name == text) {
            return scheme;
        }
    }
    return std::nullopt;
}

std::expected<ProviderSpec, AiError> parseProvider(const Json& j) {
    if (!j.is_object()) {
        return std::unexpected(invalid("provider entries must be objects"));
    }
    ProviderSpec p;
    p.id = str(j, "id");
    if (p.id.empty()) {
        return std::unexpected(invalid("provider without id"));
    }
    const std::string where = "provider '" + p.id + "'";
    p.name = str(j, "name", p.id);
    p.api = str(j, "api");
    p.baseUrl = str(j, "baseUrl");
    if (p.baseUrl.empty()) {
        return std::unexpected(invalid(where + ": missing baseUrl"));
    }
    const auto auth = authFromString(str(j, "auth", "bearer"));
    if (!auth) {
        return std::unexpected(invalid(where + ": unknown auth '" + str(j, "auth") + "'"));
    }
    p.auth = *auth;
    p.keyRequired = flag(j, "keyRequired", p.auth != AuthScheme::None);
    p.envVar = str(j, "envVar");
    if (auto it = j.find("keyCheck"); it != j.end() && it->is_object()) {
        p.keyCheck.method = str(*it, "method", "GET");
        p.keyCheck.path = str(*it, "path");
    }
    p.local = flag(j, "local", false);
    p.configurableBaseUrl = flag(j, "configurableBaseUrl", p.local);
    if (auto it = j.find("headers"); it != j.end() && it->is_object()) {
        for (const auto& [name, value] : it->items()) {
            if (value.is_string()) {
                p.extraHeaders.push_back({name, value.get<std::string>()});
            }
        }
    }
    p.consoleUrl = str(j, "consoleUrl");
    p.termsUrl = str(j, "termsUrl");
    p.dataPolicy = str(j, "dataPolicy");
    p.verified = flag(j, "verified", false);
    p.source = str(j, "source");
    p.notes = str(j, "notes");
    return p;
}

std::expected<PriceRule, AiError> parsePrice(const Json& j, const std::string& where) {
    if (!j.is_object()) {
        return std::unexpected(invalid(where + ": prices must be objects"));
    }
    PriceRule r;
    auto from = date(j, "from", where);
    if (!from) {
        return std::unexpected(from.error());
    }
    r.effectiveFrom = *from;
    r.aboveInputTokens = integer(j, "aboveInputTokens");
    r.tier = str(j, "tier");
    r.inputPerMTok = usd(j, "inputPerMTok").value_or(0);
    r.outputPerMTok = usd(j, "outputPerMTok").value_or(0);
    r.cacheReadPerMTok = usd(j, "cacheReadPerMTok");
    r.cacheWritePerMTok = usd(j, "cacheWritePerMTok");
    r.perAudioMinute = usd(j, "perAudioMinute").value_or(0);
    r.perThousandChars = usd(j, "perThousandChars").value_or(0);
    r.perVideoSecond = usd(j, "perVideoSecond").value_or(0);
    r.perImage = usd(j, "perImage").value_or(0);
    r.perRequest = usd(j, "perRequest").value_or(0);
    return r;
}

std::vector<std::string> strings(const Json& j, const char* key) {
    std::vector<std::string> out;
    if (auto it = j.find(key); it != j.end() && it->is_array()) {
        for (const auto& v : *it) {
            if (v.is_string()) {
                out.push_back(v.get<std::string>());
            }
        }
    }
    return out;
}

std::expected<GenerationSpec, AiError> parseGeneration(const Json& j, const std::string& where) {
    if (!j.is_object()) {
        return std::unexpected(invalid(where + ": generation must be an object"));
    }
    GenerationSpec g;
    g.video = flag(j, "video", false);
    g.image = flag(j, "image", false);
    g.textInput = flag(j, "textInput", true);
    g.firstFrame = flag(j, "firstFrame", false);
    g.lastFrame = flag(j, "lastFrame", false);
    g.lastFrameAlone = flag(j, "lastFrameAlone", false);
    g.extendVideo = flag(j, "extendVideo", false);
    g.audio = flag(j, "audio", false);
    g.cancel = flag(j, "cancel", false);
    if (auto it = j.find("durations"); it != j.end() && it->is_array()) {
        for (const auto& d : *it) {
            if (!d.is_number_integer() || d.get<int>() <= 0) {
                return std::unexpected(invalid(where + ": durations must be positive whole seconds"));
            }
            g.durations.push_back(d.get<int>());
        }
        std::sort(g.durations.begin(), g.durations.end());
    }
    g.aspectRatios = strings(j, "aspectRatios");
    g.resolutions = strings(j, "resolutions");
    if (auto it = j.find("fps"); it != j.end() && it->is_number()) {
        g.fps = it->get<double>();
    }
    g.maxPromptChars = static_cast<int>(integer(j, "maxPromptChars"));
    if (!g.video && !g.image) {
        return std::unexpected(invalid(where + ": generation makes neither video nor image"));
    }
    return g;
}

std::expected<ModelSpec, AiError> parseModel(const Json& j, const std::vector<ProviderSpec>& providers) {
    if (!j.is_object()) {
        return std::unexpected(invalid("model entries must be objects"));
    }
    ModelSpec m;
    m.id = str(j, "id");
    m.provider = str(j, "provider");
    if (m.id.empty()) {
        return std::unexpected(invalid("model without id"));
    }
    const std::string where = "model '" + m.id + "'";
    const auto provider = std::find_if(providers.begin(), providers.end(), [&](const ProviderSpec& p) { return p.id == m.provider; });
    if (provider == providers.end()) {
        return std::unexpected(invalid(where + ": unknown provider '" + m.provider + "'"));
    }
    m.name = str(j, "name", m.id);
    m.api = str(j, "api", provider->api);
    if (m.api.empty()) {
        return std::unexpected(invalid(where + ": no api"));
    }
    m.endpoint = str(j, "endpoint");
    if (auto it = j.find("aliases"); it != j.end() && it->is_array()) {
        for (const auto& a : *it) {
            if (a.is_string()) {
                m.aliases.push_back(a.get<std::string>());
            }
        }
    }
    const auto caps = j.find("capabilities");
    if (caps == j.end() || !caps->is_object()) {
        return std::unexpected(invalid(where + ": missing capabilities"));
    }
    for (const auto& [key, value] : caps->items()) {
        if (auto c = capabilityFromString(key)) {
            if (!value.is_boolean()) {
                return std::unexpected(invalid(where + ": capability '" + key + "' must be true/false"));
            }
            if (value.get<bool>()) {
                m.caps.capabilities.push_back(*c);
            }
            continue;
        }
        static constexpr std::array<std::string_view, 12> kFlags{
            "vision",    "toolsOnChat", "toolsOnResponses", "strictSchema", "structuredOutput", "forcedToolChoice",
            "temperature", "streaming", "videoInput",       "wordTimestamps", "diarization", "constrainedTools"};
        if (std::find(kFlags.begin(), kFlags.end(), key) == kFlags.end()) {
            return std::unexpected(invalid(where + ": unknown capability '" + key + "'"));
        }
    }
    if (m.caps.capabilities.empty()) {
        return std::unexpected(invalid(where + ": no capability"));
    }
    m.caps.vision = flag(*caps, "vision", false);
    m.caps.toolsOnChat = flag(*caps, "toolsOnChat", false);
    m.caps.toolsOnResponses = flag(*caps, "toolsOnResponses", false);
    m.caps.strictSchema = flag(*caps, "strictSchema", false);
    m.caps.structuredOutput = flag(*caps, "structuredOutput", false);
    m.caps.forcedToolChoice = flag(*caps, "forcedToolChoice", true);
    m.caps.constrainedTools = flag(*caps, "constrainedTools", false);
    m.caps.temperature = flag(*caps, "temperature", true);
    m.caps.streaming = flag(*caps, "streaming", true);
    m.caps.wordTimestamps = flag(*caps, "wordTimestamps", false);
    m.caps.diarization = flag(*caps, "diarization", false);
    const std::string video = str(*caps, "videoInput", "none");
    if (video == "none") {
        m.caps.videoInput = VideoInput::None;
    } else if (video == "frames") {
        m.caps.videoInput = VideoInput::Frames;
    } else if (video == "native") {
        m.caps.videoInput = VideoInput::Native;
    } else {
        return std::unexpected(invalid(where + ": videoInput must be none, frames or native"));
    }
    m.contextTokens = integer(j, "contextTokens");
    m.maxOutputTokens = integer(j, "maxOutputTokens");
    m.maxUploadBytes = integer(j, "maxUploadBytes");
    m.maxAudioSeconds = integer(j, "maxAudioSeconds");
    if (auto it = j.find("generation"); it != j.end()) {
        auto generation = parseGeneration(*it, where);
        if (!generation) {
            return std::unexpected(generation.error());
        }
        m.generation = std::move(*generation);
    } else if (m.caps.has(Capability::GenerateMedia)) {
        return std::unexpected(invalid(where + ": a generateMedia model needs a 'generation' block"));
    }
    if (auto it = j.find("prices"); it != j.end()) {
        if (!it->is_array()) {
            return std::unexpected(invalid(where + ": prices must be an array"));
        }
        for (const auto& p : *it) {
            auto rule = parsePrice(p, where);
            if (!rule) {
                return std::unexpected(rule.error());
            }
            m.prices.push_back(*rule);
        }
    }
    auto deprecation = date(j, "deprecation", where);
    if (!deprecation) {
        return std::unexpected(deprecation.error());
    }
    m.deprecation = *deprecation;
    auto retirement = date(j, "retirement", where);
    if (!retirement) {
        return std::unexpected(retirement.error());
    }
    m.retirement = *retirement;
    m.license = str(j, "license");
    m.verified = flag(j, "verified", false);
    m.source = str(j, "source");
    m.notes = str(j, "notes");
    return m;
}

Json dateJson(const std::optional<std::chrono::year_month_day>& d) { return d ? Json(formatDate(*d)) : Json(nullptr); }

double toUsd(MicroUsd v) { return static_cast<double>(v) / static_cast<double>(kMicroUsdPerUsd); }

/// ceil(a * b / divisor) for non-negative values, without overflow for realistic inputs.
MicroUsd scaledCeil(std::int64_t amount, MicroUsd price, std::int64_t divisor) {
    if (amount <= 0 || price <= 0) {
        return 0;
    }
    const auto product = static_cast<long double>(amount) * static_cast<long double>(price);
    return static_cast<MicroUsd>(std::ceil(product / static_cast<long double>(divisor)));
}

} // namespace

std::string_view toString(Capability capability) {
    for (const auto& [c, name] : kCapabilityNames) {
        if (c == capability) {
            return name;
        }
    }
    return "chat";
}

std::optional<Capability> capabilityFromString(std::string_view text) {
    for (const auto& [c, name] : kCapabilityNames) {
        if (name == text) {
            return c;
        }
    }
    return std::nullopt;
}

std::span<const Capability> allCapabilities() { return kAllCapabilities; }

std::string_view toString(VideoInput input) {
    switch (input) {
    case VideoInput::None:
        return "none";
    case VideoInput::Frames:
        return "frames";
    case VideoInput::Native:
        return "native";
    }
    return "none";
}

std::string_view toString(AuthScheme scheme) {
    for (const auto& [s, name] : kAuthNames) {
        if (s == scheme) {
            return name;
        }
    }
    return "none";
}

void applyAuth(HttpHeaders& headers, AuthScheme scheme, std::string_view key) {
    if (key.empty()) {
        return;
    }
    switch (scheme) {
    case AuthScheme::None:
        return;
    case AuthScheme::Bearer:
        headers.push_back({"Authorization", "Bearer " + std::string(key)});
        return;
    case AuthScheme::XApiKey:
        headers.push_back({"x-api-key", std::string(key)});
        return;
    case AuthScheme::XiApiKey:
        headers.push_back({"xi-api-key", std::string(key)});
        return;
    case AuthScheme::XGoogApiKey:
        headers.push_back({"x-goog-api-key", std::string(key)});
        return;
    case AuthScheme::Token:
        headers.push_back({"Authorization", "Token " + std::string(key)});
        return;
    case AuthScheme::RawAuthorization:
        headers.push_back({"Authorization", std::string(key)});
        return;
    }
}

std::string formatUsd(MicroUsd amount) {
    const bool negative = amount < 0;
    const std::uint64_t value = negative ? static_cast<std::uint64_t>(-(amount + 1)) + 1 : static_cast<std::uint64_t>(amount);
    const std::uint64_t whole = value / kMicroUsdPerUsd;
    std::string fraction = std::to_string(value % kMicroUsdPerUsd);
    fraction.insert(0, 6 - fraction.size(), '0');
    while (fraction.size() > 2 && fraction.back() == '0') {
        fraction.pop_back();
    }
    return std::string(negative ? "-$" : "$") + std::to_string(whole) + "." + fraction;
}

bool ModelCapabilities::has(Capability capability) const {
    return std::find(capabilities.begin(), capabilities.end(), capability) != capabilities.end();
}

const PriceRule* priceRuleFor(const ModelSpec& model, std::int64_t inputTokens, std::chrono::year_month_day date,
                              std::string_view tier) {
    // A media tier with rules of its own uses only those; otherwise the untiered rules. Rules of other tiers never
    // apply. Then: the latest effectiveFrom <= date (rules without a date count as the oldest), then the highest
    // long-context tier exceeded.
    const bool tiered = !tier.empty() && std::any_of(model.prices.begin(), model.prices.end(),
                                                     [&](const PriceRule& r) { return r.tier == tier; });
    const std::string_view wanted = tiered ? tier : std::string_view{};
    std::optional<std::chrono::year_month_day> era;
    bool haveEra = false;
    for (const auto& r : model.prices) {
        if (r.tier != wanted || (r.effectiveFrom && *r.effectiveFrom > date)) {
            continue;
        }
        if (!haveEra || (r.effectiveFrom && (!era || *r.effectiveFrom > *era))) {
            era = r.effectiveFrom;
            haveEra = true;
        }
    }
    if (!haveEra) {
        return nullptr;
    }
    const PriceRule* best = nullptr;
    for (const auto& r : model.prices) {
        if (r.tier != wanted || r.effectiveFrom != era) {
            continue;
        }
        if (r.aboveInputTokens > 0 && inputTokens <= r.aboveInputTokens) {
            continue;
        }
        if (!best || r.aboveInputTokens > best->aboveInputTokens) {
            best = &r;
        }
    }
    return best;
}

MicroUsd costOf(const ModelSpec& model, const Usage& usage, std::chrono::year_month_day date) {
    const PriceRule* rule = priceRuleFor(model, usage.inputTokens, date, usage.mediaTier);
    if (!rule) {
        return 0;
    }
    const std::int64_t cached = std::max<std::int64_t>(0, usage.cacheReadTokens);
    const std::int64_t written = std::max<std::int64_t>(0, usage.cacheWriteTokens);
    const std::int64_t fresh = std::max<std::int64_t>(0, usage.inputTokens - cached - written);
    MicroUsd total = 0;
    total += scaledCeil(fresh, rule->inputPerMTok, 1'000'000);
    total += scaledCeil(cached, rule->cacheReadPerMTok.value_or(rule->inputPerMTok), 1'000'000);
    total += scaledCeil(written, rule->cacheWritePerMTok.value_or(rule->inputPerMTok), 1'000'000);
    total += scaledCeil(usage.outputTokens, rule->outputPerMTok, 1'000'000);
    total += scaledCeil(usage.audioMilliseconds, rule->perAudioMinute, 60'000);
    total += scaledCeil(usage.characters, rule->perThousandChars, 1'000);
    total += scaledCeil(usage.videoMilliseconds, rule->perVideoSecond, 1'000);
    total += std::max<std::int64_t>(0, usage.images) * rule->perImage;
    total += rule->perRequest;
    return total;
}

bool isDeprecated(const ModelSpec& model, std::chrono::year_month_day today) {
    return model.deprecation && *model.deprecation <= today;
}

std::chrono::year_month_day todayUtc() {
    return std::chrono::year_month_day(std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now()));
}

std::optional<std::chrono::year_month_day> parseDate(std::string_view text) {
    if (text.size() != 10 || text[4] != '-' || text[7] != '-') {
        return std::nullopt;
    }
    int y = 0;
    unsigned m = 0;
    unsigned d = 0;
    auto num = [](std::string_view s, auto& out) {
        auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
        return ec == std::errc() && p == s.data() + s.size();
    };
    if (!num(text.substr(0, 4), y) || !num(text.substr(5, 2), m) || !num(text.substr(8, 2), d)) {
        return std::nullopt;
    }
    const std::chrono::year_month_day ymd{std::chrono::year(y), std::chrono::month(m), std::chrono::day(d)};
    if (!ymd.ok()) {
        return std::nullopt;
    }
    return ymd;
}

std::string formatDate(std::chrono::year_month_day date) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%04d-%02u-%02u", static_cast<int>(date.year()), static_cast<unsigned>(date.month()),
                  static_cast<unsigned>(date.day()));
    return buf;
}

// --- ModelRegistry ----------------------------------------------------------------------------------------------

std::expected<ModelRegistry, AiError> ModelRegistry::fromJson(std::string_view json) {
    const auto j = Json::parse(json, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        return std::unexpected(invalid("not a JSON object"));
    }
    if (jsonIntOr(j, "schemaVersion", 0) != 1) {
        return std::unexpected(invalid("unsupported schemaVersion"));
    }
    ModelRegistry registry;
    registry.m_updated = str(j, "updated");
    const auto providers = j.find("providers");
    const auto models = j.find("models");
    if (providers == j.end() || !providers->is_array() || models == j.end() || !models->is_array()) {
        return std::unexpected(invalid("'providers' and 'models' arrays are required"));
    }
    std::set<std::string> ids;
    for (const auto& p : *providers) {
        auto spec = parseProvider(p);
        if (!spec) {
            return std::unexpected(spec.error());
        }
        if (!ids.insert(spec->id).second) {
            return std::unexpected(invalid("duplicate provider '" + spec->id + "'"));
        }
        registry.m_builtinProviderIds.push_back(spec->id);
        registry.m_providers.push_back(std::move(*spec));
    }
    std::set<std::string> modelIds;
    for (const auto& m : *models) {
        auto spec = parseModel(m, registry.m_providers);
        if (!spec) {
            return std::unexpected(spec.error());
        }
        if (!modelIds.insert(spec->id).second) {
            return std::unexpected(invalid("duplicate model '" + spec->id + "'"));
        }
        for (const auto& alias : spec->aliases) {
            if (!modelIds.insert(alias).second) {
                return std::unexpected(invalid("alias '" + alias + "' of model '" + spec->id + "' is taken"));
            }
        }
        registry.m_models.push_back(std::move(*spec));
    }
    return registry;
}

std::string_view ModelRegistry::builtinJson() {
    return {reinterpret_cast<const char*>(detail::kModelsJson), detail::kModelsJsonSize};
}

const ModelRegistry& ModelRegistry::builtin() {
    static const ModelRegistry registry = [] {
        auto parsed = fromJson(builtinJson());
        return parsed ? std::move(*parsed) : ModelRegistry(); // the tests guarantee the shipped file parses
    }();
    return registry;
}

const ProviderSpec* ModelRegistry::provider(std::string_view id) const {
    for (const auto& p : m_providers) {
        if (p.id == id) {
            return &p;
        }
    }
    return nullptr;
}

const ModelSpec* ModelRegistry::model(std::string_view idOrAlias) const {
    for (const auto& m : m_models) {
        if (m.id == idOrAlias || std::find(m.aliases.begin(), m.aliases.end(), idOrAlias) != m.aliases.end()) {
            return &m;
        }
    }
    return nullptr;
}

std::vector<const ModelSpec*> ModelRegistry::modelsOf(std::string_view providerId) const {
    std::vector<const ModelSpec*> out;
    for (const auto& m : m_models) {
        if (m.provider == providerId) {
            out.push_back(&m);
        }
    }
    return out;
}

std::vector<const ModelSpec*> ModelRegistry::modelsWith(Capability capability) const {
    std::vector<const ModelSpec*> out;
    for (const auto& m : m_models) {
        if (m.caps.has(capability)) {
            out.push_back(&m);
        }
    }
    return out;
}

std::expected<const ModelSpec*, AiError> ModelRegistry::usableModel(std::string_view idOrAlias,
                                                                    std::chrono::year_month_day today) const {
    const ModelSpec* m = model(idOrAlias);
    if (!m) {
        return std::unexpected(AiError(AiErrorCode::ModelMissing, "unknown model '" + std::string(idOrAlias) + "'"));
    }
    if (isDeprecated(*m, today)) {
        return std::unexpected(
            AiError(AiErrorCode::Deprecated, "model '" + m->id + "' is deprecated since " + formatDate(*m->deprecation)));
    }
    return m;
}

std::expected<void, AiError> ModelRegistry::addCustomProvider(ProviderSpec provider, std::vector<ModelSpec> models) {
    if (provider.id.empty() || provider.baseUrl.empty()) {
        return std::unexpected(AiError(AiErrorCode::InvalidRequest, "a custom provider needs an id and a base URL"));
    }
    if (std::find(m_builtinProviderIds.begin(), m_builtinProviderIds.end(), provider.id) != m_builtinProviderIds.end()) {
        return std::unexpected(AiError(AiErrorCode::InvalidRequest, "'" + provider.id + "' is a built-in provider"));
    }
    for (auto& m : models) {
        if (m.provider.empty()) {
            m.provider = provider.id;
        }
        if (m.provider != provider.id || m.id.empty()) {
            return std::unexpected(AiError(AiErrorCode::InvalidRequest, "custom models must belong to the provider"));
        }
        if (m.api.empty()) {
            m.api = provider.api;
        }
    }
    for (const auto& m : models) {
        const ModelSpec* existing = model(m.id);
        if (existing && existing->provider != provider.id) {
            return std::unexpected(AiError(AiErrorCode::InvalidRequest, "model id '" + m.id + "' is taken"));
        }
    }
    std::erase_if(m_models, [&](const ModelSpec& m) { return m.provider == provider.id; });
    std::erase_if(m_providers, [&](const ProviderSpec& p) { return p.id == provider.id; });
    m_providers.push_back(std::move(provider));
    for (auto& m : models) {
        m_models.push_back(std::move(m));
    }
    return {};
}

std::expected<void, AiError> ModelRegistry::addModel(ModelSpec model) {
    const ProviderSpec* owner = provider(model.provider);
    if (!owner) {
        return std::unexpected(AiError(AiErrorCode::InvalidRequest, "unknown provider '" + model.provider + "'"));
    }
    if (model.id.empty() || this->model(model.id)) {
        return std::unexpected(AiError(AiErrorCode::InvalidRequest, "model id '" + model.id + "' is empty or taken"));
    }
    if (model.api.empty()) {
        model.api = owner->api;
    }
    if (model.name.empty()) {
        model.name = model.id;
    }
    if (model.caps.capabilities.empty()) {
        model.caps.capabilities.push_back(Capability::Chat);
    }
    m_userModelIds.push_back(model.id);
    m_models.push_back(std::move(model));
    return {};
}

bool ModelRegistry::removeUserModel(std::string_view id) {
    auto it = std::find(m_userModelIds.begin(), m_userModelIds.end(), id);
    if (it == m_userModelIds.end()) {
        return false;
    }
    m_userModelIds.erase(it);
    std::erase_if(m_models, [&](const ModelSpec& m) { return m.id == id; });
    return true;
}

bool ModelRegistry::isUserModel(std::string_view id) const {
    return std::find(m_userModelIds.begin(), m_userModelIds.end(), id) != m_userModelIds.end();
}

std::vector<std::string> ModelRegistry::unverifiedEntries() const {
    std::vector<std::string> out;
    for (const auto& p : m_providers) {
        if (!p.verified) {
            out.push_back("provider:" + p.id);
        }
    }
    for (const auto& m : m_models) {
        if (!m.verified) {
            out.push_back("model:" + m.id);
        }
    }
    return out;
}

Json ModelRegistry::toJson() const {
    Json providers = Json::array();
    for (const auto& p : m_providers) {
        Json headers = Json::object();
        for (const auto& h : p.extraHeaders) {
            headers[h.name] = h.value;
        }
        providers.push_back({{"id", p.id},
                             {"name", p.name},
                             {"api", p.api},
                             {"baseUrl", p.baseUrl},
                             {"auth", toString(p.auth)},
                             {"keyRequired", p.keyRequired},
                             {"envVar", p.envVar},
                             {"keyCheck", {{"method", p.keyCheck.method}, {"path", p.keyCheck.path}}},
                             {"local", p.local},
                             {"configurableBaseUrl", p.configurableBaseUrl},
                             {"headers", headers},
                             {"consoleUrl", p.consoleUrl},
                             {"termsUrl", p.termsUrl},
                             {"dataPolicy", p.dataPolicy},
                             {"verified", p.verified},
                             {"source", p.source},
                             {"notes", p.notes}});
    }
    Json models = Json::array();
    for (const auto& m : m_models) {
        Json caps = Json::object();
        for (Capability c : kAllCapabilities) {
            caps[std::string(toString(c))] = m.caps.has(c);
        }
        caps["vision"] = m.caps.vision;
        caps["toolsOnChat"] = m.caps.toolsOnChat;
        caps["toolsOnResponses"] = m.caps.toolsOnResponses;
        caps["strictSchema"] = m.caps.strictSchema;
        caps["structuredOutput"] = m.caps.structuredOutput;
        caps["forcedToolChoice"] = m.caps.forcedToolChoice;
        caps["constrainedTools"] = m.caps.constrainedTools;
        caps["temperature"] = m.caps.temperature;
        caps["streaming"] = m.caps.streaming;
        caps["videoInput"] = toString(m.caps.videoInput);
        caps["wordTimestamps"] = m.caps.wordTimestamps;
        caps["diarization"] = m.caps.diarization;
        Json prices = Json::array();
        for (const auto& r : m.prices) {
            Json p = {{"from", dateJson(r.effectiveFrom)}, {"aboveInputTokens", r.aboveInputTokens},
                      {"inputPerMTok", toUsd(r.inputPerMTok)}, {"outputPerMTok", toUsd(r.outputPerMTok)},
                      {"perAudioMinute", toUsd(r.perAudioMinute)}, {"perThousandChars", toUsd(r.perThousandChars)},
                      {"perVideoSecond", toUsd(r.perVideoSecond)}, {"perImage", toUsd(r.perImage)},
                      {"perRequest", toUsd(r.perRequest)}};
            if (!r.tier.empty()) {
                p["tier"] = r.tier;
            }
            if (r.cacheReadPerMTok) {
                p["cacheReadPerMTok"] = toUsd(*r.cacheReadPerMTok);
            }
            if (r.cacheWritePerMTok) {
                p["cacheWritePerMTok"] = toUsd(*r.cacheWritePerMTok);
            }
            prices.push_back(std::move(p));
        }
        Json generation;
        if (m.caps.has(Capability::GenerateMedia)) {
            const GenerationSpec& g = m.generation;
            generation = {{"video", g.video},           {"image", g.image},
                          {"textInput", g.textInput},   {"firstFrame", g.firstFrame},
                          {"lastFrame", g.lastFrame},   {"lastFrameAlone", g.lastFrameAlone},
                          {"extendVideo", g.extendVideo}, {"audio", g.audio},
                          {"cancel", g.cancel},         {"durations", g.durations},
                          {"aspectRatios", g.aspectRatios}, {"resolutions", g.resolutions},
                          {"fps", g.fps},               {"maxPromptChars", g.maxPromptChars}};
        }
        models.push_back({{"id", m.id},
                          {"provider", m.provider},
                          {"name", m.name},
                          {"api", m.api},
                          {"endpoint", m.endpoint},
                          {"aliases", m.aliases},
                          {"capabilities", caps},
                          {"contextTokens", m.contextTokens},
                          {"maxOutputTokens", m.maxOutputTokens},
                          {"maxUploadBytes", m.maxUploadBytes},
                          {"maxAudioSeconds", m.maxAudioSeconds},
                          {"prices", prices},
                          {"deprecation", dateJson(m.deprecation)},
                          {"retirement", dateJson(m.retirement)},
                          {"license", m.license},
                          {"verified", m.verified},
                          {"source", m.source},
                          {"notes", m.notes}});
        if (!generation.is_null()) {
            models.back()["generation"] = std::move(generation);
        }
    }
    return {{"schemaVersion", 1}, {"updated", m_updated}, {"providers", providers}, {"models", models}};
}

} // namespace wabcut::ai
