#include <wabcut/ai/translate/LocalTranslation.h>

#include <wabcut/ai/translate/Languages.h>

#include <algorithm>
#include <array>

namespace wabcut::ai::translate {

namespace embed = search::embed;

namespace {

constexpr std::array<LocalPair, 20> kPairs{{
    {"en", "th", "opus-mt-en-mul", ">>tha<<"},
    {"en", "ja", "opus-mt-en-mul", ">>jpn<<"},
    {"en", "vi", "opus-mt-en-mul", ">>vie<<"},
    {"en", "id", "opus-mt-en-mul", ">>ind<<"},
    {"en", "lo", "opus-mt-en-mul", ">>lao<<"},
    {"en", "km", "opus-mt-en-mul", ">>khm<<"},
    {"en", "my", "opus-mt-en-mul", ">>mya<<"},
    {"en", "es", "opus-mt-en-es", ""},
    {"en", "fr", "opus-mt-en-fr", ""},
    {"en", "de", "opus-mt-en-de", ""},
    {"en", "zh", "opus-mt-en-zh", ">>cmn_Hans<<"},
    {"en", "zh-Hant", "opus-mt-en-zh", ">>cmn_Hant<<"},
    {"th", "en", "opus-mt-th-en", ""},
    {"ja", "en", "opus-mt-ja-en", ""},
    {"ko", "en", "opus-mt-ko-en", ""},
    {"zh", "en", "opus-mt-zh-en", ""},
    {"zh-Hant", "en", "opus-mt-zh-en", ""},
    {"es", "en", "opus-mt-es-en", ""},
    {"fr", "en", "opus-mt-fr-en", ""},
    {"de", "en", "opus-mt-de-en", ""},
}};

AiError aiError(const Error& e, std::string_view what) {
    const AiErrorCode code = e.code == ErrorCode::Cancelled  ? AiErrorCode::Cancelled
                             : e.code == ErrorCode::NotFound ? AiErrorCode::ModelMissing
                                                             : AiErrorCode::ProviderError;
    return AiError(code, std::string(what) + ": " + e.message);
}

} // namespace

std::span<const LocalPair> localPairs() { return kPairs; }

const LocalPair* findLocalPair(std::string_view source, std::string_view target) {
    const std::string s = normalizeLanguage(source);
    const std::string t = normalizeLanguage(target);
    for (const LocalPair& p : kPairs) {
        if (p.source == s && p.target == t && !p.model.empty()) {
            return &p;
        }
    }
    return nullptr;
}

std::vector<const ModelInfo*> localModelFiles(std::string_view model) {
    std::vector<const ModelInfo*> out;
    if (model.empty()) {
        return out;
    }
    for (const char* part : {"-encoder", "-decoder", "-tokenizer", "-config"}) {
        const ModelInfo* m = findModel(std::string(model) + part);
        if (m == nullptr || m->kind != ModelKind::Translation) {
            return {};
        }
        out.push_back(m);
    }
    return out;
}

std::filesystem::path localModelDirectory(const ModelStore& store, std::string_view model) { return store.root() / std::string(model); }

bool isLocalModelInstalled(const ModelStore& store, std::string_view model) {
    const auto files = localModelFiles(model);
    return !files.empty() && std::ranges::all_of(files, [&](const ModelInfo* m) { return store.isInstalled(*m); });
}

std::uint64_t localModelDownloadBytes(const ModelStore& store, std::string_view model) {
    std::uint64_t bytes = 0;
    for (const ModelInfo* m : localModelFiles(model)) {
        bytes += store.isInstalled(*m) ? 0 : m->size;
    }
    return bytes;
}

std::expected<std::filesystem::path, Error> ensureLocalModel(ModelStore& store, std::string_view model, IDownloadTransport* transport,
                                                             const std::function<void(const DownloadProgress&)>& progress,
                                                             std::stop_token stop) {
    const auto files = localModelFiles(model);
    if (files.empty()) {
        return std::unexpected(Error{ErrorCode::NotFound, "no translation model \"" + std::string(model) + "\""});
    }
    for (const ModelInfo* m : files) {
        if (auto ok = store.ensure(*m, transport, progress, stop); !ok) {
            return std::unexpected(ok.error());
        }
    }
    return localModelDirectory(store, model);
}

LocalTranslator::LocalTranslator(LocalTranslatorOptions options) : m_options(std::move(options)) {
    search::EmbedHelperOptions helper;
    helper.executable = m_options.executable;
    helper.arguments = m_options.arguments;
    helper.log = m_options.log;
    helper.startTimeout = m_options.startTimeout;
    helper.lowPriority = false; // the user waits for it
    m_helper = std::make_unique<search::EmbedHelper>(std::move(helper));
}

LocalTranslator::~LocalTranslator() = default;

std::expected<TranslationResult, AiError> LocalTranslator::translate(const TranslationRequest& request, std::stop_token stop) {
    TranslationResult result;
    if (request.segments.empty()) {
        return result;
    }
    if (auto started = m_helper->start(stop); !started) {
        return std::unexpected(aiError(started.error(), "the translation helper did not start"));
    }
    if (m_helper->session() != m_loadedSession) {
        const auto load = [&](const std::string& device) {
            embed::Load loadRequest;
            loadRequest.id = m_helper->nextId();
            loadRequest.translation = m_options.modelDirectory;
            loadRequest.device = device;
            loadRequest.threads = m_options.threads;
            return m_helper->call<embed::Loaded>(embed::Request{loadRequest}, loadRequest.id, "loading the translation model", m_options.loadTimeout, stop);
        };
        auto loaded = load(m_cpuOnly ? std::string("cpu") : m_options.device);
        if (!loaded && !m_cpuOnly && m_options.device != "cpu" && !m_helper->running() && !stop.stop_requested()) {
            // The GPU provider took the helper down (an access violation is not an exception): once more on the CPU.
            m_cpuOnly = true;
            if (auto restarted = m_helper->start(stop); !restarted) {
                return std::unexpected(aiError(restarted.error(), "the translation helper did not start"));
            }
            loaded = load("cpu");
        }
        if (!loaded) {
            return std::unexpected(aiError(loaded.error(), "loading the translation model failed"));
        }
        m_loadedSession = m_helper->session();
        m_device = loaded->device;
    }
    const std::size_t batch = std::clamp<std::size_t>(m_options.batch, 1, static_cast<std::size_t>(embed::kMaxTextsPerRequest));
    for (std::size_t from = 0; from < request.segments.size(); from += batch) {
        if (stop.stop_requested()) {
            return std::unexpected(AiError(AiErrorCode::Cancelled, "translation cancelled"));
        }
        embed::TranslateText call;
        call.id = m_helper->nextId();
        call.target = m_options.targetToken;
        const std::size_t to = std::min(request.segments.size(), from + batch);
        std::size_t longest = 0;
        for (std::size_t i = from; i < to; ++i) {
            std::string text = request.segments[i].text;
            if (text.size() > embed::kMaxTextBytes) {
                text.resize(embed::kMaxTextBytes); // captions are far shorter; a longer one is cut, never sent whole
                while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xC0) == 0x80) {
                    text.pop_back();
                }
            }
            longest = std::max(longest, text.size());
            call.texts.push_back(std::move(text));
        }
        // Two target tokens per source byte is ample for a caption; OPUS-MT stops at </s> long before.
        call.maxTokens = std::clamp(static_cast<int>(longest) * 2 + 16, 32, embed::kMaxTranslationTokens);
        auto answer = m_helper->call<embed::Translations>(embed::Request{call}, call.id, "translating", m_options.requestTimeout, stop);
        if (!answer) {
            return std::unexpected(aiError(answer.error(), "local translation failed"));
        }
        if (answer->texts.size() != to - from) {
            return std::unexpected(AiError(AiErrorCode::ProviderError, "the translation helper answered " + std::to_string(answer->texts.size()) +
                                                                           " texts for " + std::to_string(to - from)));
        }
        for (std::size_t i = from; i < to; ++i) {
            result.segments.push_back(TranslationSegment{request.segments[i].id, std::move(answer->texts[i - from])});
        }
        m_tokens += answer->tokens;
        m_milliseconds += answer->milliseconds;
        if (!answer->device.empty()) {
            m_device = answer->device;
        }
    }
    for (const TranslationSegment& s : request.segments) {
        result.usage.characters += static_cast<std::int64_t>(s.text.size());
    }
    return result;
}

} // namespace wabcut::ai::translate
