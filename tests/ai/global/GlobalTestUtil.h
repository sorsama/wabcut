#pragma once

// Fakes for the AI-12 tests (caption translation, dubbing, narration): a dictionary translator, a word-list line
// breaker for Thai, a scripted voice provider and the audio side of a dub without FFmpeg. Nothing here touches the
// network or a model.

#include <wabcut/ai/Capability.h>
#include <wabcut/ai/ModelRegistry.h>
#include <wabcut/ai/dub/Pipeline.h>
#include <wabcut/model/CaptionText.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace wabcut::ai::global::test {

/// Translates whole texts (and the pieces between bleeps) from a dictionary; unknown text comes back as "<lang>:text".
/// `dropBleeps`: answers a text with a bleep marker without it (a model that lost a marker).
class DictionaryTranslator final : public ITranslator {
public:
    explicit DictionaryTranslator(std::map<std::string, std::string> words, bool dropBleeps = false)
        : m_words(std::move(words)), m_dropBleeps(dropBleeps) {}

    std::expected<TranslationResult, AiError> translate(const TranslationRequest& request, std::stop_token stop) override {
        if (stop.stop_requested()) {
            return std::unexpected(AiError(AiErrorCode::Cancelled, "cancelled"));
        }
        std::lock_guard lock(m_mutex);
        m_requests.push_back(request);
        TranslationResult result;
        for (const TranslationSegment& s : request.segments) {
            std::string out;
            if (const auto it = m_words.find(s.text); it != m_words.end()) {
                out = it->second;
            } else {
                out = request.targetLanguage + ":" + s.text;
            }
            if (m_dropBleeps) {
                for (auto at = out.find("[bleep]"); at != std::string::npos; at = out.find("[bleep]")) {
                    out.erase(at, 7);
                }
            }
            result.segments.push_back(TranslationSegment{s.id, out});
            result.usage.characters += static_cast<std::int64_t>(s.text.size());
        }
        return result;
    }

    std::vector<TranslationRequest> requests() const {
        std::lock_guard lock(m_mutex);
        return m_requests;
    }
    /// Every text that was sent, in order.
    std::vector<std::string> sent() const {
        std::vector<std::string> out;
        for (const TranslationRequest& r : requests()) {
            for (const TranslationSegment& s : r.segments) {
                out.push_back(s.text);
            }
        }
        return out;
    }
    void stopDroppingBleeps() { m_dropBleeps = false; }

private:
    std::map<std::string, std::string> m_words;
    bool m_dropBleeps = false;
    mutable std::mutex m_mutex;
    std::vector<TranslationRequest> m_requests;
};

/// Lends a translator the test keeps (a host's opener hands out a new one per language and drops it after the run).
class SharedTranslator final : public ITranslator {
public:
    explicit SharedTranslator(std::shared_ptr<ITranslator> translator) : m_translator(std::move(translator)) {}
    std::expected<TranslationResult, AiError> translate(const TranslationRequest& request, std::stop_token stop) override {
        return m_translator->translate(request, stop);
    }

private:
    std::shared_ptr<ITranslator> m_translator;
};

/// Breaks between the words of a list (longest match first), as a dictionary-based Thai breaker does.
class WordListBreaker final : public LineBreaker {
public:
    explicit WordListBreaker(std::vector<std::string> words) : m_words(std::move(words)) {
        std::sort(m_words.begin(), m_words.end(), [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
    }
    std::vector<std::size_t> breakOpportunities(std::string_view text, std::string_view language) const override {
        std::vector<std::size_t> out = simpleLineBreaker().breakOpportunities(text, language);
        std::size_t i = 0;
        while (i < text.size()) {
            if (text[i] == ' ') {
                ++i;
                continue;
            }
            std::size_t matched = 0;
            for (const std::string& w : m_words) {
                if (text.substr(i, w.size()) == w) {
                    matched = w.size();
                    break;
                }
            }
            if (matched == 0) {
                ++i;
                while (i < text.size() && (static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) {
                    ++i;
                }
                continue;
            }
            i += matched;
            if (i < text.size() && text[i] != ' ') {
                out.push_back(i);
            }
        }
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }

private:
    std::vector<std::string> m_words;
};

/// The registry's entry for `id`.
inline ModelSpec modelSpec(const std::string& id) {
    const ModelSpec* m = ModelRegistry::builtin().model(id);
    REQUIRE(m != nullptr);
    return *m;
}

/// A voice provider that "speaks" at `charsPerSecond`: the audio is the text "LEN <seconds>" (FakeSpeechAudio decodes
/// it). It records every request, can fail at a line, and clones / deletes voices on request.
class ScriptedSpeech final : public ISpeechSynth {
public:
    explicit ScriptedSpeech(std::string model = "eleven_v3", double charsPerSecond = 15.0)
        : m_spec(modelSpec(model)), m_cps(charsPerSecond) {}

    const ModelSpec& spec() const override { return m_spec; }

    std::expected<SpeechResult, AiError> synthesize(const SpeechRequest& request, std::stop_token stop) override {
        if (stop.stop_requested()) {
            return std::unexpected(AiError(AiErrorCode::Cancelled, "cancelled"));
        }
        requests.push_back(request);
        if (failAt && requests.size() == *failAt) {
            return std::unexpected(AiError(failCode, "scripted failure"));
        }
        double seconds = static_cast<double>(dub::characterCount(request.text)) / m_cps;
        if (const auto it = lengths.find(request.text); it != lengths.end()) {
            seconds = it->second;
        }
        SpeechResult r;
        const std::string text = "LEN " + std::to_string(seconds);
        r.audio.assign(text.begin(), text.end());
        r.mediaType = "audio/x-test";
        r.usage.characters = static_cast<std::int64_t>(dub::characterCount(request.text));
        return r;
    }

    std::expected<ClonedVoice, AiError> cloneVoice(std::span<const std::filesystem::path> samples, std::string_view, std::stop_token) override {
        clonedFrom.assign(samples.begin(), samples.end());
        if (cloneFails) {
            return std::unexpected(AiError(AiErrorCode::QuotaExceeded, "no clone slots"));
        }
        ClonedVoice v;
        v.voiceId = "ClonedVoice42";
        v.requiresVerification = cloneNeedsVerification;
        return v;
    }
    std::expected<void, AiError> deleteVoice(std::string_view voiceId, std::stop_token) override {
        deleted.emplace_back(voiceId);
        if (deleteFails) {
            return std::unexpected(AiError(AiErrorCode::Network, "connection reset"));
        }
        return {};
    }

    std::vector<SpeechRequest> requests;
    std::map<std::string, double> lengths; ///< text -> seconds, overriding the speaking rate
    std::optional<std::size_t> failAt;     ///< 1-based request that fails
    AiErrorCode failCode = AiErrorCode::QuotaExceeded;
    bool cloneFails = false;
    bool cloneNeedsVerification = false;
    bool deleteFails = false;
    std::vector<std::filesystem::path> clonedFrom;
    std::vector<std::string> deleted;

private:
    ModelSpec m_spec;
    double m_cps;
};

/// Lends a ScriptedSpeech the test keeps (a host's opener hands out a new synthesizer per run and drops it after).
class SharedSpeech final : public ISpeechSynth {
public:
    explicit SharedSpeech(std::shared_ptr<ScriptedSpeech> speech) : m_speech(std::move(speech)) {}
    const ModelSpec& spec() const override { return m_speech->spec(); }
    std::expected<SpeechResult, AiError> synthesize(const SpeechRequest& r, std::stop_token s) override { return m_speech->synthesize(r, s); }
    std::expected<ClonedVoice, AiError> cloneVoice(std::span<const std::filesystem::path> samples, std::string_view name,
                                                   std::stop_token s) override {
        return m_speech->cloneVoice(samples, name, s);
    }
    std::expected<void, AiError> deleteVoice(std::string_view id, std::stop_token s) override { return m_speech->deleteVoice(id, s); }

private:
    std::shared_ptr<ScriptedSpeech> m_speech;
};

/// The audio side of a dub without FFmpeg: "LEN <seconds>" decodes to a tone of that length with 50 ms of silence on
/// each side; stretching resamples the length; written files are described, not written.
class FakeSpeechAudio final : public dub::SpeechAudio {
public:
    std::expected<std::vector<float>, Error> decode(const SpeechResult& speech, int rate) override {
        const std::string text(speech.audio.begin(), speech.audio.end());
        if (!text.starts_with("LEN ")) {
            return std::unexpected(Error{ErrorCode::DecoderError, "not test audio"});
        }
        const double seconds = std::stod(text.substr(4));
        const auto pad = static_cast<std::size_t>(rate / 20);
        std::vector<float> samples(pad, 0.0f);
        const auto n = static_cast<std::size_t>(std::llround(seconds * rate));
        for (std::size_t i = 0; i < n; ++i) {
            samples.push_back(0.5f * static_cast<float>(std::sin(2.0 * 3.14159265358979 * 220.0 * static_cast<double>(i) / rate)) + 0.01f);
        }
        samples.insert(samples.end(), pad, 0.0f);
        return samples;
    }
    std::vector<float> stretch(std::span<const float> samples, int, double speed) override {
        ++stretched;
        speeds.push_back(speed);
        return std::vector<float>(static_cast<std::size_t>(std::llround(static_cast<double>(samples.size()) / speed)), 0.25f);
    }
    std::expected<MediaItem, Error> write(const std::filesystem::path& file, std::span<const float> samples, int rate) override {
        written = file;
        frames = samples.size();
        MediaItem item;
        item.name = file.stem().string();
        item.locator.path = file.generic_string();
        AudioStreamInfo stream;
        stream.index = 0;
        stream.codec = "pcm_f32le";
        stream.sampleRate = Rate{rate, 1};
        stream.channels = 1;
        stream.channelLayout = "mono";
        stream.duration = Ticks{static_cast<std::int64_t>(samples.size()) * kTicksPerSecond / rate};
        item.info.audio.push_back(stream);
        item.info.duration = stream.duration;
        item.info.path = item.locator.path;
        return item;
    }
    std::expected<std::filesystem::path, Error> extractSample(const MediaItem& media, int stream, std::span<const TimeRange> ranges,
                                                              const std::filesystem::path& file) override {
        sampleMedia = media.id;
        sampleStream = stream;
        sampleRanges.assign(ranges.begin(), ranges.end());
        samplePath = file;
        return file;
    }

    int stretched = 0;
    std::vector<double> speeds;
    std::filesystem::path written;
    std::size_t frames = 0;
    MediaId sampleMedia;
    int sampleStream = -1;
    std::vector<TimeRange> sampleRanges;
    std::filesystem::path samplePath;
};

} // namespace wabcut::ai::global::test
