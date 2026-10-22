#include <wabcut/ai/Generation.h>

#include <wabcut/ai/Sha256.h>
#include <wabcut/model/MediaLocator.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <system_error>

namespace wabcut::ai {

namespace {

bool contains(const std::vector<std::string>& list, std::string_view value) {
    return std::find(list.begin(), list.end(), value) != list.end();
}

bool hasTier(const ModelSpec& model, std::string_view tier) {
    return std::any_of(model.prices.begin(), model.prices.end(), [&](const PriceRule& r) { return r.tier == tier; });
}

std::size_t utf8Length(std::string_view text) {
    return static_cast<std::size_t>(std::count_if(text.begin(), text.end(), [](char c) {
        return (static_cast<unsigned char>(c) & 0xC0) != 0x80;
    }));
}

AiError invalid(std::string what) { return AiError(AiErrorCode::InvalidRequest, std::move(what)); }

} // namespace

std::string_view toString(GeneratedMediaKind kind) {
    switch (kind) {
    case GeneratedMediaKind::Video:
        return "video";
    case GeneratedMediaKind::Image:
        return "image";
    case GeneratedMediaKind::SoundEffect:
        return "audio";
    }
    return "video";
}

double soundEffectSeconds(const ModelSpec& model, const MediaGenerationRequest& request) {
    const GenerationSpec& g = model.generation;
    if (!request.duration || request.duration->value <= 0) {
        return g.maxSeconds; // the model picks the length: the estimate assumes the longest
    }
    const double asked = static_cast<double>(request.duration->value) / static_cast<double>(kTicksPerSecond);
    return std::clamp(asked, g.minSeconds, g.maxSeconds);
}

std::string_view toString(GenerationState state) {
    switch (state) {
    case GenerationState::Queued:
        return "queued";
    case GenerationState::Running:
        return "running";
    case GenerationState::Succeeded:
        return "succeeded";
    case GenerationState::Failed:
        return "failed";
    case GenerationState::Cancelled:
        return "cancelled";
    }
    return "queued";
}

int generatedSeconds(const GenerationSpec& spec, std::optional<Ticks> wanted) {
    if (spec.durations.empty()) {
        return 0;
    }
    if (!wanted || wanted->value <= 0) {
        return spec.durations.front();
    }
    const double seconds = static_cast<double>(wanted->value) / static_cast<double>(kTicksPerSecond);
    for (const int d : spec.durations) {
        if (d + 1e-9 >= seconds) {
            return d;
        }
    }
    return spec.durations.back();
}

std::string generationTier(const ModelSpec& model, const MediaGenerationRequest& request) {
    std::string tier = request.resolution;
    if (request.kind == GeneratedMediaKind::Video && request.audio && hasTier(model, tier + "+audio")) {
        tier += "+audio";
    }
    return tier;
}

std::expected<void, AiError> checkGenerationRequest(const ModelSpec& model, const MediaGenerationRequest& request) {
    if (!model.caps.has(Capability::GenerateMedia)) {
        return std::unexpected(AiError(AiErrorCode::Unsupported, model.id + " does not generate media"));
    }
    const GenerationSpec& g = model.generation;
    const bool video = request.kind == GeneratedMediaKind::Video;
    if (request.kind == GeneratedMediaKind::SoundEffect) {
        // 0.2: a sound effect from a prompt alone.
        if (!g.soundEffect) {
            return std::unexpected(AiError(AiErrorCode::Unsupported, model.id + " does not generate sound effects"));
        }
        if (request.prompt.empty()) {
            return std::unexpected(invalid("describe the sound"));
        }
        if (!request.firstFrame.empty() || !request.lastFrame.empty() || !request.inputClip.empty()) {
            return std::unexpected(invalid("a sound effect is made from a prompt alone"));
        }
        if (g.maxPromptChars > 0 && utf8Length(request.prompt) > static_cast<std::size_t>(g.maxPromptChars)) {
            return std::unexpected(invalid("the prompt is longer than " + std::to_string(g.maxPromptChars) + " characters"));
        }
        if (request.duration) {
            const double seconds = static_cast<double>(request.duration->value) / static_cast<double>(kTicksPerSecond);
            if (seconds < g.minSeconds - 1e-9 || seconds > g.maxSeconds + 1e-9) {
                return std::unexpected(invalid(std::format("a sound effect lasts {:g} to {:g} seconds", g.minSeconds, g.maxSeconds)));
            }
        }
        if (request.loop && !g.loop) {
            return std::unexpected(AiError(AiErrorCode::Unsupported, model.id + " cannot make a loop"));
        }
        if (request.promptInfluence && !(*request.promptInfluence >= 0.0 && *request.promptInfluence <= 1.0)) {
            return std::unexpected(invalid("prompt influence lies between 0 and 1"));
        }
        if (request.count != 1) {
            return std::unexpected(invalid("one sound effect per request"));
        }
        if (!request.extra.empty()) {
            return std::unexpected(invalid("unpriced provider options are not supported"));
        }
        return {};
    }
    if ((video && !g.video) || (!video && !g.image)) {
        return std::unexpected(
            AiError(AiErrorCode::Unsupported, model.id + " does not generate " + std::string(toString(request.kind))));
    }
    const bool hasFirst = !request.firstFrame.empty();
    const bool hasLast = !request.lastFrame.empty();
    if (request.prompt.empty() && !hasFirst && !hasLast) {
        return std::unexpected(invalid("a prompt is needed"));
    }
    if (!hasFirst && !hasLast && request.inputClip.empty() && !g.textInput) {
        return std::unexpected(AiError(AiErrorCode::Unsupported, model.id + " needs a start image"));
    }
    if (hasFirst && !g.firstFrame) {
        return std::unexpected(AiError(AiErrorCode::Unsupported, model.id + " cannot start from an image"));
    }
    if (hasLast && !(hasFirst ? g.lastFrame : g.lastFrameAlone)) {
        return std::unexpected(AiError(AiErrorCode::Unsupported,
                                       model.id + (hasFirst ? " cannot end on an image" : " cannot end on an image without a start image")));
    }
    if (!request.inputClip.empty() && !g.extendVideo) {
        return std::unexpected(AiError(AiErrorCode::Unsupported, model.id + " cannot continue a clip"));
    }
    for (const auto* file : {&request.firstFrame, &request.lastFrame, &request.inputClip}) {
        std::error_code ec;
        if (!file->empty() && !std::filesystem::is_regular_file(*file, ec)) {
            return std::unexpected(invalid("no such file: " + pathToUtf8(file->filename())));
        }
    }
    if (!request.aspectRatio.empty() && !g.aspectRatios.empty() && !contains(g.aspectRatios, request.aspectRatio)) {
        return std::unexpected(invalid(model.id + " does not make " + request.aspectRatio + " output"));
    }
    if (!request.resolution.empty() && !g.resolutions.empty() && !contains(g.resolutions, request.resolution)) {
        return std::unexpected(invalid(model.id + " does not make " + request.resolution + " output"));
    }
    if (request.audio && video && !g.audio) {
        return std::unexpected(AiError(AiErrorCode::Unsupported, model.id + " does not generate sound"));
    }
    if (g.maxPromptChars > 0 && utf8Length(request.prompt) > static_cast<std::size_t>(g.maxPromptChars)) {
        return std::unexpected(invalid("the prompt is longer than " + std::to_string(g.maxPromptChars) + " characters"));
    }
    if (!request.extra.empty()) {
        return std::unexpected(invalid("unpriced provider options are not supported"));
    }
    if (video && request.duration && !g.durations.empty() &&
        request.duration->value > Ticks::fromSeconds(g.durations.back()).value) {
        return std::unexpected(invalid("duration exceeds the model limit"));
    }
    if (request.count < 1 || request.count > 4) {
        return std::unexpected(invalid("between 1 and 4 outputs per request"));
    }
    if (video && request.count != 1) {
        return std::unexpected(invalid("one video per request"));
    }
    if (request.duration && request.duration->value <= 0) {
        return std::unexpected(invalid("the duration must be positive"));
    }
    return {};
}

Usage generationUsage(const ModelSpec& model, const MediaGenerationRequest& request) {
    Usage u;
    u.mediaTier = generationTier(model, request);
    if (request.kind == GeneratedMediaKind::SoundEffect) {
        // Billed per minute of sound: the length asked for, or the longest when the model picks it.
        u.audioMilliseconds = static_cast<std::int64_t>(std::ceil(soundEffectSeconds(model, request) * 1000.0));
        return u;
    }
    if (request.kind == GeneratedMediaKind::Video) {
        const int seconds = generatedSeconds(model.generation, request.duration);
        std::int64_t ms = static_cast<std::int64_t>(seconds) * 1000;
        if (seconds == 0 && request.duration) { // no documented durations: bill what was asked for
            ms = (request.duration->value * 1000 + kTicksPerSecond - 1) / kTicksPerSecond;
        }
        u.videoMilliseconds = ms * std::max(1, request.count);
    } else {
        u.images = std::max(1, request.count);
    }
    return u;
}

CostEstimate estimateGenerationCost(const ModelSpec& model, const MediaGenerationRequest& request,
                                    std::chrono::year_month_day date) {
    CostEstimate e;
    const Usage usage = generationUsage(model, request);
    e.priced = priceRuleFor(model, 0, date, usage.mediaTier) != nullptr;
    e.expected = e.ceiling = costOf(model, usage, date);
    return e;
}

std::string promptHash(std::string_view prompt) { return Sha256::hex(prompt); }

} // namespace wabcut::ai
