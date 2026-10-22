#pragma once

// Cloud wire adapters (docs/ai.md, "Provider contract"; docs/internal/ai-research.md B.1/B.2). Each one implements a
// capability interface on top of IHttpTransport only: no Qt types, so the recorded-fixture tests run them against a
// fake transport. The Qt network stack that carries them in the app is in Network.h.
//
//   api id                 interface     wire format
//   anthropic-messages     IChatModel    POST /v1/messages, SSE (text, tool_use + input_json_delta, thinking)
//   openai-responses       IChatModel    POST /v1/responses, typed SSE events (store: false, stateless)
//   chat-completions       IChatModel    POST /chat/completions, SSE "data:" chunks + [DONE] (OpenAI-compatible
//                                        servers: Ollama, LM Studio, llama-server, OpenRouter, Groq, Mistral)
//   gemini-interactions    IChatModel    POST /v1beta/interactions, SSE step events; native video via Files API
//   deepgram-listen        ITranscriber  POST /v1/listen (raw audio body), words in seconds
//   elevenlabs-stt         ITranscriber  POST /v1/speech-to-text (multipart), words in seconds
//   assemblyai             ITranscriber  upload -> submit -> poll, words in milliseconds
//   elevenlabs-sound-generation  IMediaGenerator  POST /v1/sound-generation -> MP3 (0.2: sound effects)
//
// Chat adapters always stream (SSE) and only call the event handler when one is given; the returned
// ChatResponse is complete either way.

#include <wabcut/ai/Capability.h>
#include <wabcut/ai/Provider.h>
#include <wabcut/ai/Transport.h>

#include <chrono>
#include <memory>

namespace wabcut::ai::net {

struct AdapterOptions {
    Sleeper sleeper = interruptibleSleep;              ///< waits between polls (tests pass a no-op)
    std::chrono::milliseconds pollInterval{3000};      ///< AssemblyAI transcript status
    std::chrono::milliseconds filePollInterval{2000};  ///< Gemini Files API processing state
    std::chrono::minutes maxPollTime{360};             ///< give up polling after this long
};

/// Registers every adapter above with `registry` under its api id.
// Managed local provider: preparation runs inside chat() on the job thread, never in the factory.
using PrepareLocal = std::function<std::expected<std::pair<std::string, Secret>, AiError>(std::stop_token)>;
std::unique_ptr<IChatModel> makeManagedChat(ProviderContext context, PrepareLocal prepare,
                                          std::function<void(bool)> busy);
std::unique_ptr<IMediaGenerator> makeVeoGenerator(ProviderContext context);
std::unique_ptr<IMediaGenerator> makeRunwayGenerator(ProviderContext context);
/// 0.2: generated sound effects (elevenlabs-sound-generation: POST /v1/sound-generation, the MP3 in the answer).
std::unique_ptr<IMediaGenerator> makeElevenLabsSoundEffects(ProviderContext context);

void registerCloudAdapters(ProviderRegistry& registry, AdapterOptions options = {});

std::unique_ptr<IChatModel> makeAnthropicChat(ProviderContext context);
std::unique_ptr<IChatModel> makeOpenAiResponsesChat(ProviderContext context);
std::unique_ptr<IChatModel> makeChatCompletionsChat(ProviderContext context);
std::unique_ptr<IChatModel> makeGeminiInteractionsChat(ProviderContext context, AdapterOptions options = {});
std::unique_ptr<ITranscriber> makeDeepgramTranscriber(ProviderContext context);
std::unique_ptr<ITranscriber> makeElevenLabsTranscriber(ProviderContext context);
/// Text-to-speech (AI-12): ElevenLabs (with character timestamps), OpenAI /audio/speech, Gemini interactions.
std::unique_ptr<ISpeechSynth> makeElevenLabsSpeech(ProviderContext context);
std::unique_ptr<ISpeechSynth> makeOpenAiSpeech(ProviderContext context);
std::unique_ptr<ISpeechSynth> makeGeminiSpeech(ProviderContext context);
std::unique_ptr<ITranscriber> makeAssemblyAiTranscriber(ProviderContext context, AdapterOptions options = {});

} // namespace wabcut::ai::net
