#include <wabcut/ai/SpendGuard.h>
#include <wabcut/ai/JsonAccess.h>

#include <algorithm>
#include <cstdio>
#include <set>

namespace wabcut::ai {

namespace {

constexpr std::int64_t kTokensPerImage = 1600;      // ~1092x1092 px; providers bill 1-5k tokens per image
constexpr std::int64_t kTokensPerVideoSecond = 300; // Gemini's documented upper rate per second of video
constexpr std::int64_t kTokensPerMessage = 4;       // role/formatting overhead

std::int64_t textTokens(std::size_t bytes) { return static_cast<std::int64_t>((bytes + 3) / 4); }

std::string monthKey(std::chrono::year_month month) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%04d-%02u", static_cast<int>(month.year()), static_cast<unsigned>(month.month()));
    return buf;
}

std::chrono::year_month monthOf(std::chrono::year_month_day day) { return {day.year(), day.month()}; }

} // namespace

std::int64_t estimateInputTokens(const ChatRequest& request) {
    std::size_t bytes = request.system.size();
    std::int64_t tokens = 0;
    for (const auto& message : request.messages) {
        tokens += kTokensPerMessage;
        for (const auto& part : message.parts) {
            if (const auto* t = std::get_if<TextPart>(&part)) {
                bytes += t->text.size();
            } else if (std::holds_alternative<ImagePart>(part)) {
                tokens += kTokensPerImage;
            } else if (const auto* v = std::get_if<VideoPart>(&part)) {
                if (v->start && v->end && *v->end > *v->start) {
                    const auto seconds = (v->end->value - v->start->value + kTicksPerSecond - 1) / kTicksPerSecond;
                    tokens += seconds * kTokensPerVideoSecond;
                }
            } else if (const auto* c = std::get_if<ToolCall>(&part)) {
                bytes += c->name.size() + c->argumentsText.size();
            } else if (const auto* r = std::get_if<ToolResult>(&part)) {
                bytes += r->content.size();
            } else if (const auto* reasoning = std::get_if<ReasoningPart>(&part)) {
                bytes += reasoning->text.size();
            }
        }
    }
    for (const auto& tool : request.tools) {
        bytes += tool.name.size() + tool.description.size() + tool.parameters.dump().size();
    }
    if (request.responseFormat) {
        bytes += request.responseFormat->schema.dump().size();
    }
    return tokens + textTokens(bytes);
}

CostEstimate estimateChatCost(const ModelSpec& model, const ChatRequest& request, std::chrono::year_month_day date) {
    CostEstimate e;
    e.inputTokens = estimateInputTokens(request);
    e.maxOutputTokens = request.maxOutputTokens;
    e.priced = priceRuleFor(model, e.inputTokens, date) != nullptr;
    Usage ceiling;
    ceiling.inputTokens = e.inputTokens;
    ceiling.outputTokens = request.maxOutputTokens;
    Usage likely = ceiling;
    likely.outputTokens = (request.maxOutputTokens + 3) / 4;
    e.ceiling = costOf(model, ceiling, date);
    e.expected = costOf(model, likely, date);
    return e;
}

CostEstimate estimateTranscriptionCost(const ModelSpec& model, std::int64_t audioMilliseconds,
                                       std::chrono::year_month_day date) {
    CostEstimate e;
    e.audioMilliseconds = audioMilliseconds;
    e.priced = priceRuleFor(model, 0, date) != nullptr;
    Usage usage;
    usage.audioMilliseconds = audioMilliseconds;
    e.expected = e.ceiling = costOf(model, usage, date);
    return e;
}

CostEstimate estimateSpeechCost(const ModelSpec& model, std::int64_t characters, std::chrono::year_month_day date) {
    CostEstimate e;
    const std::int64_t chars = std::max<std::int64_t>(0, characters);
    e.inputTokens = chars / 3 + 1;
    e.priced = priceRuleFor(model, e.inputTokens, date) != nullptr;
    Usage likely;
    likely.inputTokens = e.inputTokens;
    likely.characters = chars;
    likely.audioMilliseconds = std::max<std::int64_t>(2000, chars * 1000 / 14);
    Usage ceiling = likely;
    ceiling.audioMilliseconds = std::max<std::int64_t>(2000, chars * 1000 / 6);
    e.audioMilliseconds = likely.audioMilliseconds;
    e.expected = costOf(model, likely, date);
    e.ceiling = costOf(model, ceiling, date);
    return e;
}

// --- SpendGuard -------------------------------------------------------------------------------------------------

void SpendGuard::setMonthlyCap(std::string_view provider, std::optional<MicroUsd> cap) {
    {
        std::lock_guard lock(m_mutex);
        if (cap) {
            m_caps.insert_or_assign(std::string(provider), std::max<MicroUsd>(0, *cap));
        } else if (auto it = m_caps.find(provider); it != m_caps.end()) {
            m_caps.erase(it);
        }
    }
    notifyChanged();
}

std::optional<MicroUsd> SpendGuard::monthlyCap(std::string_view provider) const {
    std::lock_guard lock(m_mutex);
    auto it = m_caps.find(provider);
    return it == m_caps.end() ? std::nullopt : std::optional<MicroUsd>(it->second);
}

std::expected<SpendGuard::Reservation, AiError> SpendGuard::reserve(std::string_view provider, MicroUsd ceiling,
                                                                    std::chrono::year_month_day today) {
    std::lock_guard lock(m_mutex);
    const auto month = monthOf(today);
    if (auto cap = m_caps.find(provider); cap != m_caps.end()) {
        MicroUsd committed = 0;
        if (auto p = m_spent.find(provider); p != m_spent.end()) {
            if (auto m = p->second.find(monthKey(month)); m != p->second.end()) {
                committed += m->second;
            }
        }
        for (const auto& [id, r] : m_reservations) {
            if (r.provider == provider && r.month == month) {
                committed += r.amount;
            }
        }
        if (committed + std::max<MicroUsd>(0, ceiling) > cap->second) {
            AiError error(AiErrorCode::SpendCapExceeded,
                          "this call could cost up to " + formatUsd(ceiling) + "; " + formatUsd(committed) + " of the " +
                              formatUsd(cap->second) + " monthly cap for " + std::string(provider) + " is used");
            return std::unexpected(std::move(error));
        }
    }
    Reservation r{m_nextId++, std::string(provider), month, std::max<MicroUsd>(0, ceiling)};
    m_reservations.emplace(r.id, r);
    return r;
}

void SpendGuard::settle(const Reservation& reservation, MicroUsd actual) {
    {
        std::lock_guard lock(m_mutex);
        m_reservations.erase(reservation.id);
        m_spent[reservation.provider][monthKey(reservation.month)] += std::max<MicroUsd>(0, actual);
    }
    notifyChanged();
}

void SpendGuard::release(const Reservation& reservation) {
    std::lock_guard lock(m_mutex);
    m_reservations.erase(reservation.id);
}

void SpendGuard::record(std::string_view provider, MicroUsd amount, std::chrono::year_month_day today) {
    {
        std::lock_guard lock(m_mutex);
        m_spent[std::string(provider)][monthKey(monthOf(today))] += std::max<MicroUsd>(0, amount);
    }
    notifyChanged();
}

MicroUsd SpendGuard::spent(std::string_view provider, std::chrono::year_month month) const {
    std::lock_guard lock(m_mutex);
    auto p = m_spent.find(provider);
    if (p == m_spent.end()) {
        return 0;
    }
    auto m = p->second.find(monthKey(month));
    return m == p->second.end() ? 0 : m->second;
}

MicroUsd SpendGuard::reserved(std::string_view provider) const {
    std::lock_guard lock(m_mutex);
    MicroUsd total = 0;
    for (const auto& [id, r] : m_reservations) {
        if (r.provider == provider) {
            total += r.amount;
        }
    }
    return total;
}

std::vector<SpendGuard::ProviderSpend> SpendGuard::summary(std::chrono::year_month month) const {
    std::lock_guard lock(m_mutex);
    std::set<std::string, std::less<>> providers;
    for (const auto& [p, cap] : m_caps) {
        providers.insert(p);
    }
    for (const auto& [p, months] : m_spent) {
        if (months.contains(monthKey(month))) {
            providers.insert(p);
        }
    }
    std::vector<ProviderSpend> out;
    for (const auto& p : providers) {
        ProviderSpend s;
        s.provider = p;
        if (auto c = m_caps.find(p); c != m_caps.end()) {
            s.cap = c->second;
        }
        if (auto sp = m_spent.find(p); sp != m_spent.end()) {
            if (auto m = sp->second.find(monthKey(month)); m != sp->second.end()) {
                s.spent = m->second;
            }
        }
        for (const auto& [id, r] : m_reservations) {
            if (r.provider == p && r.month == month) {
                s.reserved += r.amount;
            }
        }
        out.push_back(std::move(s));
    }
    return out;
}

Json SpendGuard::toJson() const {
    std::lock_guard lock(m_mutex);
    Json caps = Json::object();
    for (const auto& [p, cap] : m_caps) {
        caps[p] = cap;
    }
    Json spent = Json::object();
    for (const auto& [p, months] : m_spent) {
        Json m = Json::object();
        for (const auto& [month, amount] : months) {
            m[month] = amount;
        }
        spent[p] = std::move(m);
    }
    return {{"version", 1}, {"capsMicroUsd", caps}, {"spentMicroUsd", spent}};
}

std::expected<void, AiError> SpendGuard::loadJson(const Json& json) {
    if (!json.is_object() || jsonIntOr(json, "version", 0) != 1) {
        return std::unexpected(AiError(AiErrorCode::InvalidRequest, "spend data: unsupported format"));
    }
    std::map<std::string, MicroUsd, std::less<>> caps;
    std::map<std::string, std::map<std::string, MicroUsd>, std::less<>> spent;
    if (auto it = json.find("capsMicroUsd"); it != json.end() && it->is_object()) {
        for (const auto& [p, v] : it->items()) {
            if (v.is_number_integer()) {
                caps[p] = v.get<MicroUsd>();
            }
        }
    }
    if (auto it = json.find("spentMicroUsd"); it != json.end() && it->is_object()) {
        for (const auto& [p, months] : it->items()) {
            if (!months.is_object()) {
                continue;
            }
            for (const auto& [month, v] : months.items()) {
                if (v.is_number_integer()) {
                    spent[p][month] = v.get<MicroUsd>();
                }
            }
        }
    }
    {
        std::lock_guard lock(m_mutex);
        m_caps = std::move(caps);
        m_spent = std::move(spent);
    }
    return {};
}

void SpendGuard::setOnChanged(std::function<void()> callback) {
    std::lock_guard lock(m_mutex);
    m_onChanged = std::move(callback);
}

void SpendGuard::notifyChanged() {
    std::function<void()> callback;
    {
        std::lock_guard lock(m_mutex);
        callback = m_onChanged;
    }
    if (callback) {
        callback();
    }
}

} // namespace wabcut::ai
