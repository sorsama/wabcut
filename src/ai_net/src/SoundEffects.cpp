// Generated sound effects (0.2, docs/ai.md "Generative media"; Premiere's Generative Media), checked against the
// official docs on 2026-10-16 (elevenlabs.io/docs/api-reference/text-to-sound-effects/convert):
//   ElevenLabs  POST /v1/sound-generation?output_format=mp3_44100_128, xi-api-key, {"text", "model_id":
//               "eleven_text_to_sound_v2", "duration_seconds" (0.5..30, optional: else the model picks the length),
//               "prompt_influence" (0..1, default 0.3), "loop"} -> the MP3 file. $0.12 per minute of sound
//               (elevenlabs.io/pricing/api).
// The answer is the file itself: create() makes the request and keeps the bytes, poll() reports them done with the length
// that came back (the MP3's frames counted: GenerationStatus::usage), fetch() writes them. The spend ledger is then
// settled at that length, never more than the approved estimate. Never retried: a network ambiguity must not bill a
// sound twice.

#include "AdapterUtil.h"

#include <wabcut/ai/Generation.h>
#include <wabcut/ai/JsonAccess.h>
#include <wabcut/ai_net/Adapters.h>
#include <wabcut/model/MediaLocator.h>

#include <atomic>
#include <cmath>
#include <fstream>
#include <map>
#include <mutex>
#include <random>

namespace wabcut::ai::net {

namespace {

AiError invalid(std::string text) { return {AiErrorCode::InvalidRequest, std::move(text)}; }

/// An MP3 file's first bytes: an ID3 tag or an MPEG audio frame sync.
bool looksLikeMp3(std::string_view body) {
    if (body.size() < 4) {
        return false;
    }
    const auto b0 = static_cast<unsigned char>(body[0]), b1 = static_cast<unsigned char>(body[1]);
    return body.starts_with("ID3") || (b0 == 0xFF && (b1 & 0xE0) == 0xE0);
}

/// The length of MP3 audio (MPEG-1, -2 or -2.5 Layer III) in milliseconds, by walking its frames after an ID3v2 tag:
/// each frame's samples over its sample rate. Bytes between frames are skipped (resync); an incomplete last frame does
/// not count. 0 when no frame is found.
std::int64_t mp3Milliseconds(std::string_view b) {
    const auto at8 = [&](std::size_t i) { return static_cast<unsigned>(static_cast<unsigned char>(b[i])); };
    std::size_t at = 0;
    if (b.size() >= 10 && b.starts_with("ID3")) {
        const std::size_t size = (at8(6) & 0x7F) << 21 | (at8(7) & 0x7F) << 14 | (at8(8) & 0x7F) << 7 | (at8(9) & 0x7F);
        at = 10 + size + ((at8(5) & 0x10) != 0 ? 10 : 0); // a footer follows when flagged
    }
    constexpr int kMpeg1Kbps[16] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
    constexpr int kMpeg2Kbps[16] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
    constexpr int kMpeg1Rates[3] = {44100, 48000, 32000};
    double seconds = 0.0;
    while (at + 4 <= b.size()) {
        const unsigned h1 = at8(at + 1), h2 = at8(at + 2);
        const unsigned version = (h1 >> 3) & 3; // 0 MPEG-2.5, 1 reserved, 2 MPEG-2, 3 MPEG-1
        const unsigned layer = (h1 >> 1) & 3;   // 1 = Layer III
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

class ElevenLabsSoundEffects final : public IMediaGenerator {
public:
    explicit ElevenLabsSoundEffects(ProviderContext c)
        : m_provider(c.provider), m_model(c.model), m_base(std::move(c.baseUrl)), m_key(std::move(c.key)), m_http(c.transport) {}

    const ModelSpec& spec() const override { return m_model; }

    std::expected<GenerationTicket, AiError> create(const MediaGenerationRequest& r, std::stop_token stop) override {
        if (r.kind != GeneratedMediaKind::SoundEffect) {
            return std::unexpected(AiError(AiErrorCode::Unsupported, m_model.id + " makes sound effects only"));
        }
        if (auto valid = checkGenerationRequest(m_model, r); !valid) {
            return std::unexpected(valid.error());
        }
        Json body{{"text", r.prompt}, {"model_id", m_model.id}};
        if (r.duration && r.duration->value > 0) {
            body["duration_seconds"] = std::round(soundEffectSeconds(m_model, r) * 100.0) / 100.0;
        }
        if (r.promptInfluence) {
            body["prompt_influence"] = *r.promptInfluence;
        }
        if (r.loop) {
            body["loop"] = true;
        }
        const ProviderContext c{m_provider, m_model, m_base, m_key.clone(), m_http};
        HttpRequest http = jsonRequest(c, joinUrl(m_base, "/v1/sound-generation") + "?output_format=mp3_44100_128", body, BodyMode::Buffered);
        http.retrySafe = false;
        http.timeout = std::chrono::seconds(180);
        http.idleTimeout = std::chrono::seconds(90);
        auto response = m_http.send(http, {}, stop);
        if (!response) {
            return std::unexpected(response.error());
        }
        if (!response->ok()) {
            return std::unexpected(httpError(*response));
        }
        if (response->body.size() > 64u * 1024 * 1024) {
            return std::unexpected(AiError(AiErrorCode::ProviderError, "ElevenLabs sent more than a sound effect can be"));
        }
        if (!looksLikeMp3(response->body)) {
            return std::unexpected(invalid("ElevenLabs returned something that is not an MP3 file"));
        }
        GenerationTicket ticket{m_provider.id, m_model.id, "sfx-" + std::to_string(++m_serial), {}, {}, {}};
        std::lock_guard lock(m_mutex);
        m_done[ticket.id] = std::move(response->body);
        return ticket;
    }

    std::expected<GenerationStatus, AiError> poll(const GenerationTicket& ticket, std::stop_token) override {
        GenerationStatus status;
        std::lock_guard lock(m_mutex);
        const auto it = m_done.find(ticket.id);
        if (it == m_done.end() || ticket.provider != m_provider.id || ticket.model != m_model.id) {
            return std::unexpected(invalid("invalid generation ticket"));
        }
        status.state = GenerationState::Succeeded;
        status.progress = 1.0f;
        status.inlineOutputs.push_back(std::vector<std::uint8_t>(it->second.begin(), it->second.end()));
        status.inlineTypes.push_back("audio/mpeg");
        status.usage.audioMilliseconds = mp3Milliseconds(it->second); // what came back: the spend ledger is settled at it
        return status;
    }

    std::expected<std::vector<std::filesystem::path>, AiError> fetch(const GenerationStatus& status, const std::filesystem::path& directory,
                                                                    std::stop_token) override {
        if (status.state != GenerationState::Succeeded || status.inlineOutputs.size() != 1) {
            return std::unexpected(malformedEvent("generation has no completed output"));
        }
        std::error_code ec;
        std::filesystem::create_directories(directory, ec);
        if (ec) {
            return std::unexpected(invalid("cannot create generation directory"));
        }
        auto path = directory / ("sound-effect-" + std::to_string(std::random_device{}()));
        path += ".mp3";
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        const std::vector<std::uint8_t>& bytes = status.inlineOutputs.front();
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        file.close();
        if (!file) {
            std::filesystem::remove(path, ec);
            return std::unexpected(invalid("cannot write the generated sound effect"));
        }
        return std::vector<std::filesystem::path>{path};
    }

private:
    ProviderSpec m_provider;
    ModelSpec m_model;
    std::string m_base;
    Secret m_key;
    IHttpTransport& m_http;
    std::mutex m_mutex;
    std::map<std::string, std::string> m_done;
    std::atomic<int> m_serial{0};
};

} // namespace

std::unique_ptr<IMediaGenerator> makeElevenLabsSoundEffects(ProviderContext context) {
    return std::make_unique<ElevenLabsSoundEffects>(std::move(context));
}

} // namespace wabcut::ai::net
