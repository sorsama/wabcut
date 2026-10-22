#pragma once

// The model registry as data (docs/ai.md, "Model registry as data"). Providers and models are described in
// models.json, which ships inside wabcut_ai (src/ai/data/models.json) and can be replaced by a newer, signed file
// later. Users can add their own OpenAI-compatible endpoints at runtime. Nothing in Wabcut is hard-wired to one
// vendor: adapters are chosen by the `api` string of a provider/model (ProviderRegistry, Provider.h).
//
// Every entry carries `verified` and `source`: whether its id, endpoint and prices were checked against the
// vendor's documentation, and where. unverifiedEntries() lists the rest.
//
// Money is integer micro-USD (1 USD = 1'000'000), so spend caps and estimates never round differently.

#include <wabcut/ai/AiError.h>
#include <wabcut/ai/Chat.h>
#include <wabcut/ai/Http.h>

#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::ai {

/// What a provider/model can do. Matches the capability interfaces in Capability.h.
enum class Capability {
    Chat,          ///< IChatModel (text, tools, structured output, images/video per the flags below)
    Transcribe,    ///< ITranscriber
    Embed,         ///< IEmbedder
    Speak,         ///< ISpeechSynth
    Translate,     ///< ITranslator
    GenerateMedia, ///< IMediaGenerator
    /// Consent only, never a model capability (allCapabilities() and models.json leave it out): uploading recordings of
    /// a person to make a voice from them (ISpeechSynth::cloneVoice). "Always allow" for Speak does not cover it
    /// (review wave 5, S5-P2-9).
    CloneVoice,
};

std::string_view toString(Capability capability);
std::optional<Capability> capabilityFromString(std::string_view text);
/// Every capability, in declaration order.
std::span<const Capability> allCapabilities();

enum class VideoInput {
    None,   ///< no video at all
    Frames, ///< send sampled frames as images
    Native, ///< the API accepts a video file (VideoPart)
};

std::string_view toString(VideoInput input);

/// How the API key is sent.
enum class AuthScheme {
    None,             ///< local servers
    Bearer,           ///< Authorization: Bearer <key>
    XApiKey,          ///< x-api-key: <key> (Anthropic)
    XiApiKey,         ///< xi-api-key: <key> (ElevenLabs)
    XGoogApiKey,      ///< x-goog-api-key: <key> (Gemini)
    Token,            ///< Authorization: Token <key> (Deepgram)
    RawAuthorization, ///< Authorization: <key> (AssemblyAI)
};

std::string_view toString(AuthScheme scheme);
/// Adds the auth header for `scheme` (nothing for None or an empty key).
void applyAuth(HttpHeaders& headers, AuthScheme scheme, std::string_view key);

/// Integer micro-USD.
using MicroUsd = std::int64_t;
inline constexpr MicroUsd kMicroUsdPerUsd = 1'000'000;

/// "$1.23", "$0.0043" (at least 2 decimals, up to 6, trailing zeros trimmed).
std::string formatUsd(MicroUsd amount);

/// One price rule of a model, in micro-USD per unit. A model has one or more rules; costOf() picks the rule with
/// the latest `effectiveFrom` not after the date, then the highest `aboveInputTokens` the prompt exceeds.
struct PriceRule {
    std::optional<std::chrono::year_month_day> effectiveFrom; ///< e.g. a published price change
    std::int64_t aboveInputTokens = 0; ///< applies to prompts longer than this (long-context tiers)
    /// Generated media: the output tier this rule prices ("720p", "1080p", "4k", "720p+audio", "1024x1024", ...);
    /// empty = every tier without a rule of its own.
    std::string tier;

    MicroUsd inputPerMTok = 0;     ///< per 1M input tokens (uncached)
    MicroUsd outputPerMTok = 0;    ///< per 1M output tokens (reasoning included)
    std::optional<MicroUsd> cacheReadPerMTok;  ///< default: inputPerMTok
    std::optional<MicroUsd> cacheWritePerMTok; ///< default: inputPerMTok
    MicroUsd perAudioMinute = 0;   ///< transcription
    MicroUsd perThousandChars = 0; ///< speech synthesis, translation
    MicroUsd perVideoSecond = 0;   ///< generated video
    MicroUsd perImage = 0;         ///< generated image
    MicroUsd perRequest = 0;

    bool operator==(const PriceRule&) const = default;
};

struct ModelCapabilities {
    std::vector<Capability> capabilities;
    bool vision = false;           ///< image input on chat
    bool toolsOnChat = false;      ///< tool calling on the provider's chat endpoint
    bool toolsOnResponses = false; ///< tool calling on OpenAI's Responses API
    bool strictSchema = false;     ///< schema-guaranteed tool arguments / structured output
    bool structuredOutput = false; ///< JSON-schema constrained final answer
    bool forcedToolChoice = true;  ///< accepts ToolChoiceMode::Required/Named (some models reject it)
    /// AI-15: tool calls as one JSON-schema constrained answer per step (llama-server's grammar), not the native
    /// tools field, so a small local model always emits a well-formed call (wabcut/ai/agent/ConstrainedTools.h).
    bool constrainedTools = false;
    bool temperature = true;       ///< accepts a custom temperature (some models reject anything but 1.0)
    bool streaming = true;
    VideoInput videoInput = VideoInput::None;
    bool wordTimestamps = false;   ///< transcription
    bool diarization = false;      ///< transcription

    bool has(Capability capability) const;
    bool operator==(const ModelCapabilities&) const = default;
};

/// What a media-generation model accepts (models with the generateMedia capability; models.json "generation").
struct GenerationSpec {
    bool video = false;          ///< makes video clips
    bool image = false;          ///< makes still images
    bool textInput = true;       ///< from a prompt alone (text-to-video / text-to-image)
    bool firstFrame = false;     ///< from a start image (image-to-video)
    bool lastFrame = false;      ///< to an end image, together with a start image
    bool lastFrameAlone = false; ///< to an end image without a start image
    bool extendVideo = false;    ///< continues an input clip
    bool audio = false;          ///< can generate sound with the video
    bool soundEffect = false;    ///< 0.2: makes sound effects from a prompt (GeneratedMediaKind::SoundEffect)
    bool loop = false;           ///< sound effects: can make a seamless loop
    double minSeconds = 0.0;     ///< sound effects: the shortest and longest length one can ask for
    double maxSeconds = 0.0;
    bool cancel = false;         ///< the API can cancel a running job
    std::vector<int> durations;  ///< allowed lengths in seconds (video), ascending
    std::vector<std::string> aspectRatios;
    std::vector<std::string> resolutions; ///< video resolutions ("720p") or image sizes ("1024x1024", "2K")
    double fps = 0.0;            ///< frame rate of the output video; 0 = not documented
    int maxPromptChars = 0;      ///< 0 = no documented limit

    bool operator==(const GenerationSpec&) const = default;
};

struct KeyCheck {
    std::string method = "GET";
    std::string path; ///< relative to the provider's base URL, e.g. "/v1/models?limit=1"

    bool operator==(const KeyCheck&) const = default;
};

struct ProviderSpec {
    std::string id;           ///< stable, e.g. "anthropic"; also the secret-store entry name
    std::string name;         ///< display name
    std::string api;          ///< default wire format of its models, e.g. "anthropic-messages"
    std::string baseUrl;      ///< "https://api.anthropic.com"
    AuthScheme auth = AuthScheme::Bearer;
    bool keyRequired = true;
    std::string envVar;       ///< environment override, e.g. "WABCUT_ANTHROPIC_API_KEY"
    KeyCheck keyCheck;        ///< a cheap authenticated call used to validate a key on save
    bool local = false;       ///< runs on this machine (Ollama, LM Studio, llama-server): no upload, no cost
    bool configurableBaseUrl = false;
    HttpHeaders extraHeaders; ///< sent with every request (e.g. OpenRouter's X-Title)
    std::string consoleUrl;   ///< where users create a key
    std::string termsUrl;     ///< data use / retention terms, linked from the consent prompt
    std::string dataPolicy;   ///< one-line summary for the consent prompt
    bool verified = false;
    std::string source;       ///< documentation the entry was checked against
    std::string notes;

    bool operator==(const ProviderSpec&) const = default;
};

struct ModelSpec {
    std::string id;           ///< the provider's model id, e.g. "claude-sonnet-5-5"
    std::string provider;     ///< ProviderSpec::id
    std::string name;         ///< display name
    std::string api;          ///< wire format; defaults to the provider's
    std::string endpoint;     ///< path below the base URL (or a full URL), e.g. "/v1/messages"
    std::vector<std::string> aliases;
    ModelCapabilities caps;
    std::int64_t contextTokens = 0;
    std::int64_t maxOutputTokens = 0;
    std::int64_t maxUploadBytes = 0;     ///< 0 = no documented limit
    std::int64_t maxAudioSeconds = 0;    ///< 0 = no documented limit
    GenerationSpec generation;           ///< media generation models
    std::vector<PriceRule> prices;
    std::optional<std::chrono::year_month_day> deprecation; ///< from this date on the model is Deprecated
    std::optional<std::chrono::year_month_day> retirement;  ///< announced shutdown (or "not before" date)
    std::string license;      ///< "proprietary", or the SPDX id of open weights
    bool verified = false;
    std::string source;
    std::string notes;

    bool operator==(const ModelSpec&) const = default;
};

/// The price rule that applies to a call with `inputTokens` prompt tokens on `date`; nullptr without prices. With a
/// `tier` (generated media), a rule of that tier wins over untiered rules; rules of other tiers never apply.
const PriceRule* priceRuleFor(const ModelSpec& model, std::int64_t inputTokens, std::chrono::year_month_day date,
                              std::string_view tier = {});

/// Cost of `usage` on `model` at `date`, rounded up to whole micro-USD. 0 for models without prices.
MicroUsd costOf(const ModelSpec& model, const Usage& usage, std::chrono::year_month_day date);

bool isDeprecated(const ModelSpec& model, std::chrono::year_month_day today);

/// Today's date in UTC.
std::chrono::year_month_day todayUtc();
/// "2026-10-11"; nullopt when malformed.
std::optional<std::chrono::year_month_day> parseDate(std::string_view text);
std::string formatDate(std::chrono::year_month_day date);

class ModelRegistry {
public:
    /// Parses a registry document (schemaVersion 1). Unknown fields are ignored so newer files load; missing
    /// required fields, unknown capabilities, duplicate ids or models of unknown providers are InvalidRequest
    /// errors naming the entry.
    static std::expected<ModelRegistry, AiError> fromJson(std::string_view json);
    /// The models.json compiled into the library.
    static std::string_view builtinJson();
    /// The parsed built-in registry (parsed once; the shipped file is validated by the tests).
    static const ModelRegistry& builtin();

    const std::vector<ProviderSpec>& providers() const { return m_providers; }
    const std::vector<ModelSpec>& models() const { return m_models; }
    std::string_view updated() const { return m_updated; }

    const ProviderSpec* provider(std::string_view id) const;
    /// By id or alias.
    const ModelSpec* model(std::string_view idOrAlias) const;
    std::vector<const ModelSpec*> modelsOf(std::string_view providerId) const;
    std::vector<const ModelSpec*> modelsWith(Capability capability) const;

    /// The model if it exists and is not deprecated on `today`. Errors: ModelMissing, Deprecated.
    std::expected<const ModelSpec*, AiError> usableModel(std::string_view idOrAlias,
                                                         std::chrono::year_month_day today) const;

    /// Adds (or replaces) a user-defined provider with its models, e.g. an OpenAI-compatible server. Errors:
    /// InvalidRequest (empty id, a built-in provider id, models of another provider).
    std::expected<void, AiError> addCustomProvider(ProviderSpec provider, std::vector<ModelSpec> models);

    /// Adds a user-defined model to an existing provider, e.g. "qwen3.5:9b" on Ollama or any OpenRouter model;
    /// capabilities and api default to a text chat model on the provider's api. InvalidRequest when the provider
    /// is unknown or the id is taken.
    std::expected<void, AiError> addModel(ModelSpec model);
    /// Removes a model added with addModel() (built-in models stay). False when there was none.
    bool removeUserModel(std::string_view id);
    bool isUserModel(std::string_view id) const;

    /// "provider:<id>" and "model:<id>" for every entry with verified == false.
    std::vector<std::string> unverifiedEntries() const;

    Json toJson() const;

private:
    std::vector<ProviderSpec> m_providers;
    std::vector<ModelSpec> m_models;
    std::vector<std::string> m_builtinProviderIds;
    std::vector<std::string> m_userModelIds;
    std::string m_updated;
};

} // namespace wabcut::ai
