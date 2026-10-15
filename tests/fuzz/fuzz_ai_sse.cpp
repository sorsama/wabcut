// Fuzz harness: AI provider response bodies (src/ai StreamDecoder, src/ai_net chat adapters; owner: AI platform).
// Harness only: crashes go to the AI platform engineer through the coordinator. The input is a response body that is
//   - decoded as SSE and as NDJSON by StreamDecoder whole, one byte at a time and in 7-byte pieces: the same events
//     and the same outcome each way;
//   - streamed into each chat adapter (Anthropic Messages, OpenAI Responses, Chat Completions, Gemini Interactions)
//     as a 200 text/event-stream response;
//   - classified as the error body of a 400 and a 429 response.

#include "FuzzSupport.h"

#include "FakeTransport.h"

#include <wabcut/ai/Chat.h>
#include <wabcut/ai/Http.h>
#include <wabcut/ai/ModelRegistry.h>
#include <wabcut/ai/Provider.h>
#include <wabcut/ai_net/Adapters.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace wabcut;
using namespace wabcut::ai;

namespace {

struct Decoded {
    std::vector<SseEvent> events;
    std::vector<std::string> lines;
    bool ok = true;

    bool operator==(const Decoded&) const = default;
};

Decoded decode(BodyMode mode, std::string_view body, std::size_t piece) {
    Decoded out;
    HttpStreamHandlers handlers;
    handlers.onEvent = [&](const SseEvent& e) {
        out.events.push_back(e);
        return true;
    };
    handlers.onLine = [&](std::string_view line) {
        out.lines.emplace_back(line);
        return true;
    };
    StreamDecoder decoder(mode, handlers);
    for (std::size_t pos = 0; out.ok && pos < body.size(); pos += piece) {
        out.ok = decoder.feed(body.substr(pos, piece));
    }
    out.ok = out.ok && decoder.finish();
    return out;
}

void chat(int which, std::string_view body) {
    static const char* const kModels[] = {"claude-sonnet-5-5", "gpt-6.1-sol", "voxtral-mini-latest", "gemini-3.8-flash"};
    const ModelRegistry& registry = ModelRegistry::builtin();
    const ModelSpec* model = registry.model(kModels[which]);
    const ProviderSpec* provider = model ? registry.provider(model->provider) : nullptr;
    WABCUT_FUZZ_CHECK(model && provider, std::string("the built-in registry has no ") + kModels[which]);
    test::FakeTransport transport;
    transport.push(test::sseBody(std::string(body)));
    ProviderContext context{*provider, *model, provider->baseUrl, Secret(std::string("test-key-0123456789")), transport};
    std::unique_ptr<IChatModel> adapter;
    switch (which) {
    case 0:
        adapter = net::makeAnthropicChat(std::move(context));
        break;
    case 1:
        adapter = net::makeOpenAiResponsesChat(std::move(context));
        break;
    case 2:
        adapter = net::makeChatCompletionsChat(std::move(context));
        break;
    default: {
        net::AdapterOptions options;
        options.sleeper = [](std::chrono::milliseconds, std::stop_token) { return true; };
        adapter = net::makeGeminiInteractionsChat(std::move(context), options);
        break;
    }
    }
    ChatRequest request;
    request.messages.push_back(ChatMessage::user("hi"));
    const auto response = adapter->chat(request, [](const ChatEvent&) {}, {});
    (void)response;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view body = fuzz::text(data, size);
    for (const BodyMode mode : {BodyMode::Sse, BodyMode::Ndjson}) {
        const Decoded whole = decode(mode, body, body.size() + 1);
        WABCUT_FUZZ_CHECK(decode(mode, body, 1) == whole, "decoding byte by byte differs from decoding at once");
        WABCUT_FUZZ_CHECK(decode(mode, body, 7) == whole, "decoding in pieces differs from decoding at once");
    }
    for (int which = 0; which < 4; ++which) {
        chat(which, body);
    }
    for (const int status : {400, 429}) {
        const AiError error = classifyHttpError(status, {{"content-type", "application/json"}}, body);
        (void)error;
    }
    return 0;
}
