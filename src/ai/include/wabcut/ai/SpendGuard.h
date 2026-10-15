#pragma once

// Client-side spend control (docs/ai.md: "per-provider monthly spend caps"). Before a cloud call the caller asks
// for a pre-flight estimate (from the registry's prices), reserves its ceiling against the provider's monthly cap,
// and settles the reservation with the actual cost from the response's Usage. Reservations make concurrent jobs
// respect the cap too. Months are calendar months in UTC.
//
// Estimates are deliberately conservative: input tokens are estimated from text length (about 4 UTF-8 bytes per
// token, rounded up) plus a fixed cost per image, and the ceiling assumes the full maxOutputTokens.

#include <wabcut/ai/AiError.h>
#include <wabcut/ai/Capability.h>
#include <wabcut/ai/ModelRegistry.h>

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::ai {

struct CostEstimate {
    MicroUsd expected = 0;          ///< likely cost (output assumed at a quarter of the maximum)
    MicroUsd ceiling = 0;           ///< worst case: what gets reserved against the cap
    std::int64_t inputTokens = 0;   ///< estimated
    std::int64_t maxOutputTokens = 0;
    std::int64_t audioMilliseconds = 0;
    bool priced = false;            ///< false when the registry has no price for the model (cost unknown)
};

/// Estimated prompt tokens of a chat request (text, tool specs, response format, images).
std::int64_t estimateInputTokens(const ChatRequest& request);
CostEstimate estimateChatCost(const ModelSpec& model, const ChatRequest& request, std::chrono::year_month_day date);
CostEstimate estimateTranscriptionCost(const ModelSpec& model, std::int64_t audioMilliseconds,
                                       std::chrono::year_month_day date);
/// Text-to-speech (AI-12): `characters` of text. Billed per character (ElevenLabs) or per minute of generated audio
/// (OpenAI, Gemini): the expected audio runs at about 14 characters a second, the ceiling at 6 (slow speech), and
/// at least 2 s.
CostEstimate estimateSpeechCost(const ModelSpec& model, std::int64_t characters, std::chrono::year_month_day date);

class SpendGuard {
public:
    struct Reservation {
        std::uint64_t id = 0;
        std::string provider;
        std::chrono::year_month month{};
        MicroUsd amount = 0;
    };

    struct ProviderSpend {
        std::string provider;
        std::optional<MicroUsd> cap; ///< nullopt = no cap
        MicroUsd spent = 0;          ///< settled this month
        MicroUsd reserved = 0;       ///< in flight
    };

    SpendGuard() = default;
    SpendGuard(const SpendGuard&) = delete;
    SpendGuard& operator=(const SpendGuard&) = delete;

    void setMonthlyCap(std::string_view provider, std::optional<MicroUsd> cap);
    std::optional<MicroUsd> monthlyCap(std::string_view provider) const;

    /// Reserves `ceiling` for a call made on `today`. Error SpendCapExceeded when spent + reserved + ceiling would
    /// exceed the provider's cap for that month; providers without a cap always succeed.
    std::expected<Reservation, AiError> reserve(std::string_view provider, MicroUsd ceiling,
                                                std::chrono::year_month_day today);
    /// Replaces the reservation with the actual cost (which may exceed the reservation; it is recorded anyway).
    void settle(const Reservation& reservation, MicroUsd actual);
    /// Drops a reservation without cost (the call failed before the provider billed anything).
    void release(const Reservation& reservation);
    /// Records a cost that was not reserved (e.g. a key check).
    void record(std::string_view provider, MicroUsd amount, std::chrono::year_month_day today);

    MicroUsd spent(std::string_view provider, std::chrono::year_month month) const;
    MicroUsd reserved(std::string_view provider) const;
    /// Cap, spent and reserved for `month`, for every provider with a cap or spending.
    std::vector<ProviderSpend> summary(std::chrono::year_month month) const;

    /// Caps and monthly totals (reservations are not persisted).
    Json toJson() const;
    std::expected<void, AiError> loadJson(const Json& json);
    /// Called after every change, outside the lock (e.g. to persist toJson()).
    void setOnChanged(std::function<void()> callback);

private:
    void notifyChanged();

    mutable std::mutex m_mutex;
    std::map<std::string, MicroUsd, std::less<>> m_caps;
    // provider -> "YYYY-MM" -> settled micro-USD
    std::map<std::string, std::map<std::string, MicroUsd>, std::less<>> m_spent;
    std::map<std::uint64_t, Reservation> m_reservations;
    std::uint64_t m_nextId = 1;
    std::function<void()> m_onChanged;
};

} // namespace wabcut::ai
