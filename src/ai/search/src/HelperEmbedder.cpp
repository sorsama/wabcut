#include <wabcut/ai/search/HelperEmbedder.h>

#include <wabcut/ai/search/EmbedProtocol.h>

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <random>

namespace wabcut::ai::search {

namespace {

std::unexpected<Error> fail(ErrorCode code, std::string message) {
    return std::unexpected(Error(code, std::move(message)));
}

std::string randomToken() {
    std::random_device device;
    std::mt19937_64 rng((static_cast<std::uint64_t>(device()) << 32) ^ device());
    char text[17];
    std::snprintf(text, sizeof text, "%016llx", static_cast<unsigned long long>(rng()));
    return text;
}

EmbedHelperOptions helperOptions(const HelperEmbedderOptions& o) {
    EmbedHelperOptions h;
    h.executable = o.executable;
    h.arguments = o.arguments;
    h.log = o.log;
    h.startTimeout = o.startTimeout;
    return h;
}

} // namespace

std::vector<const ModelInfo*> siglipModelFiles() {
    return {findModel("siglip2-base-vision"), findModel("siglip2-base-text"), findModel("siglip2-base-tokenizer")};
}

std::string siglipModelVersion() {
    // The weights' digests (first 8 hex digits each) and the preprocessing: 224 x 224 PIL bilinear, mean/std 0.5,
    // lowercased text of at most 64 tokens. Vectors of another version are never compared with these.
    std::string version(kSiglipModelId);
    version += "/q8";
    for (const ModelInfo* m : siglipModelFiles()) {
        if (m != nullptr && m->kind == ModelKind::Embedding && m->id != "siglip2-base-tokenizer") {
            version += '-' + m->sha256.substr(0, 8);
        }
    }
    return version + "/pp1";
}

HelperEmbedderOptions siglipOptions(const ModelStore& store) {
    HelperEmbedderOptions options;
    const auto files = siglipModelFiles();
    if (files[0] != nullptr) {
        options.visionModel = store.pathFor(*files[0]);
    }
    if (files[1] != nullptr) {
        options.textModel = store.pathFor(*files[1]);
    }
    if (files[2] != nullptr) {
        options.tokenizer = store.pathFor(*files[2]);
    }
    options.modelVersion = siglipModelVersion();
    options.dimension = 768;
    options.inputSize = 224;
    return options;
}

// --- HelperEmbedder -------------------------------------------------------------------------------------------------

struct HelperEmbedder::Impl {
    explicit Impl(HelperEmbedderOptions o) : options(std::move(o)), helper(helperOptions(options)) {}

    HelperEmbedderOptions options;
    EmbedHelper helper;
    mutable std::mutex mutex;       ///< one request at a time
    mutable std::mutex deviceMutex; ///< `device` only, so status reads never wait for a request
    int visionSession = 0;          ///< the helper session the image model was loaded in (0 = not loaded)
    int textSession = 0;
    std::string device;
    std::string token = randomToken();

    std::expected<void, Error> load(bool vision, const std::stop_token& stop) {
        if (auto started = helper.start(stop); !started) {
            return started;
        }
        const int session = helper.session();
        if ((vision ? visionSession : textSession) == session) {
            return {};
        }
        embed::Load request;
        request.id = helper.nextId();
        request.device = options.device;
        request.threads = options.threads;
        if (vision) {
            request.vision = options.visionModel;
        } else {
            request.text = options.textModel;
            request.tokenizer = options.tokenizer;
        }
        auto loaded = helper.call<embed::Loaded>(embed::Request{request}, request.id,
                                                 vision ? "loading the image model" : "loading the text model", options.loadTimeout, stop);
        if (!loaded) {
            return std::unexpected(loaded.error());
        }
        if (loaded->dimension != options.dimension) {
            return std::unexpected(helper.abandon(ErrorCode::VersionMismatch, "the search model makes vectors of " +
                                                                                  std::to_string(loaded->dimension) + " dimensions, not " +
                                                                                  std::to_string(options.dimension)));
        }
        {
            std::lock_guard deviceLock(deviceMutex);
            device = loaded->device;
        }
        (vision ? visionSession : textSession) = session;
        return {};
    }

    std::expected<std::vector<Vector>, Error> checked(embed::Embeddings&& embeddings, std::size_t expected) {
        if (embeddings.vectors.size() != expected) {
            return std::unexpected(helper.abandon(ErrorCode::DecoderError, "wabcut-embed returned " +
                                                                               std::to_string(embeddings.vectors.size()) + " vectors for " +
                                                                               std::to_string(expected) + " inputs"));
        }
        for (Vector& v : embeddings.vectors) {
            if (v.size() != static_cast<std::size_t>(options.dimension)) {
                return std::unexpected(helper.abandon(ErrorCode::DecoderError, "wabcut-embed returned a vector of the wrong dimension"));
            }
            normalize(v);
        }
        return std::move(embeddings.vectors);
    }
};

HelperEmbedder::HelperEmbedder(HelperEmbedderOptions options) {
    if (options.workDir.empty()) {
        std::error_code ec;
        options.workDir = std::filesystem::temp_directory_path(ec) / "wabcut-embed";
    }
    options.batchSize = std::clamp(options.batchSize, 1, embed::kMaxImagesPerRequest);
    m_impl = std::make_unique<Impl>(std::move(options));
}

HelperEmbedder::~HelperEmbedder() {
    std::lock_guard lock(m_impl->mutex);
    m_impl->helper.stop();
}

std::expected<std::vector<Vector>, Error> HelperEmbedder::embedImages(std::span<const Image> images, std::stop_token stop) {
    std::lock_guard lock(m_impl->mutex);
    Impl& d = *m_impl;
    std::vector<Vector> out;
    if (images.empty()) {
        return out;
    }
    if (auto loaded = d.load(true, stop); !loaded) {
        return std::unexpected(loaded.error());
    }
    std::error_code ec;
    std::filesystem::create_directories(d.options.workDir, ec);
    const std::filesystem::path file = d.options.workDir / ("batch-" + d.token + ".rgb");
    struct Remove {
        std::filesystem::path path;
        ~Remove() {
            std::error_code e;
            std::filesystem::remove(path, e);
        }
    } removeFile{file};

    const auto size = d.options.inputSize;
    for (std::size_t first = 0; first < images.size(); first += static_cast<std::size_t>(d.options.batchSize)) {
        const std::size_t count = std::min(images.size() - first, static_cast<std::size_t>(d.options.batchSize));
        std::vector<Image> batch;
        batch.reserve(count);
        for (std::size_t i = first; i < first + count; ++i) {
            const Image& image = images[i];
            Image resized = image.width == size && image.height == size ? image : resizeBilinear(image, size, size);
            if (!resized.valid()) {
                return fail(ErrorCode::InvalidArgument, "smart search: picture " + std::to_string(i) + " is empty or damaged");
            }
            batch.push_back(std::move(resized));
        }
        auto written = embed::writeImageBatch(file, batch);
        if (!written) {
            return std::unexpected(written.error());
        }
        const embed::EmbedImages request{d.helper.nextId(), *written};
        auto embeddings =
            d.helper.call<embed::Embeddings>(embed::Request{request}, request.id, "embedding pictures", d.options.requestTimeout, stop);
        if (!embeddings) {
            return std::unexpected(embeddings.error());
        }
        auto vectors = d.checked(std::move(*embeddings), count);
        if (!vectors) {
            return std::unexpected(vectors.error());
        }
        for (Vector& v : *vectors) {
            out.push_back(std::move(v));
        }
    }
    return out;
}

std::expected<Vector, Error> HelperEmbedder::embedText(std::string_view text, std::stop_token stop) {
    std::lock_guard lock(m_impl->mutex);
    Impl& d = *m_impl;
    if (text.size() > embed::kMaxTextBytes) {
        std::size_t cut = embed::kMaxTextBytes;
        while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
            --cut;
        }
        text = text.substr(0, cut);
    }
    if (auto loaded = d.load(false, stop); !loaded) {
        return std::unexpected(loaded.error());
    }
    const embed::EmbedText request{d.helper.nextId(), {std::string(text)}};
    auto embeddings = d.helper.call<embed::Embeddings>(embed::Request{request}, request.id, "embedding the search text",
                                                       d.options.requestTimeout, stop);
    if (!embeddings) {
        return std::unexpected(embeddings.error());
    }
    auto vectors = d.checked(std::move(*embeddings), 1);
    if (!vectors) {
        return std::unexpected(vectors.error());
    }
    return std::move(vectors->front());
}

std::string HelperEmbedder::modelVersion() const { return m_impl->options.modelVersion; }

int HelperEmbedder::dimension() const { return m_impl->options.dimension; }

std::string HelperEmbedder::device() const {
    std::lock_guard lock(m_impl->deviceMutex);
    return m_impl->device;
}

void HelperEmbedder::shutdown() {
    std::lock_guard lock(m_impl->mutex);
    m_impl->helper.stop();
}

int HelperEmbedder::starts() const { return m_impl->helper.starts(); }

} // namespace wabcut::ai::search
