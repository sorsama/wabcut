#pragma once

// r07: the common shape of the generation adapters added in round 07 (private to wabcut_ai_adapters). An adapter either
// answers at once (the image is in the response: create() keeps it and poll() reports it done) or runs a job on the
// provider (create() returns its id and URLs, poll() asks for its state). fetch() writes inline outputs or downloads
// output URLs from the provider's own result hosts only, with credentials only where the vendor requires them.

#include "AdapterUtil.h"

#include <wabcut/ai/Generation.h>
#include <wabcut/ai/JsonAccess.h>

#include <atomic>
#include <initializer_list>
#include <map>
#include <mutex>

namespace wabcut::ai::net {

class GeneratorBase : public IMediaGenerator {
public:
    explicit GeneratorBase(ProviderContext context)
        : m_provider(context.provider), m_model(context.model), m_baseUrl(std::move(context.baseUrl)), m_key(std::move(context.key)),
          m_http(context.transport) {}

    const ModelSpec& spec() const override { return m_model; }

    std::expected<GenerationTicket, AiError> create(const MediaGenerationRequest& request, std::stop_token stop) final {
        if (auto valid = checkGenerationRequest(m_model, request); !valid) {
            return std::unexpected(valid.error());
        }
        m_kind = request.kind;
        auto submitted = submit(request, stop);
        if (!submitted) {
            return std::unexpected(submitted.error());
        }
        if (submitted->done) { // answered at once: kept until poll()
            GenerationTicket ticket{m_provider.id, m_model.id, "done-" + std::to_string(++m_serial), {}, {}, {}};
            std::lock_guard lock(m_mutex);
            m_done[ticket.id] = std::move(*submitted->done);
            return ticket;
        }
        GenerationTicket ticket = std::move(submitted->ticket);
        ticket.provider = m_provider.id;
        ticket.model = m_model.id;
        return ticket;
    }

    std::expected<GenerationStatus, AiError> poll(const GenerationTicket& ticket, std::stop_token stop) final {
        if (ticket.provider != m_provider.id || ticket.model != m_model.id) {
            return std::unexpected(AiError(AiErrorCode::InvalidRequest, "invalid generation ticket"));
        }
        {
            std::lock_guard lock(m_mutex);
            if (const auto it = m_done.find(ticket.id); it != m_done.end()) {
                return it->second;
            }
        }
        return status(ticket, stop);
    }

    std::expected<std::vector<std::filesystem::path>, AiError> fetch(const GenerationStatus& status, const std::filesystem::path& directory,
                                                                    std::stop_token stop) final {
        if (status.state != GenerationState::Succeeded || (status.outputUrls.empty() && status.inlineOutputs.empty())) {
            return std::unexpected(malformedEvent("generation has no completed output"));
        }
        if (status.outputUrls.size() + status.inlineOutputs.size() > 8) {
            return std::unexpected(malformedEvent("unexpected output count"));
        }
        std::vector<std::filesystem::path> files;
        const auto cleanup = [&] {
            std::error_code ec;
            for (const auto& f : files) {
                std::filesystem::remove(f, ec);
            }
        };
        for (std::size_t i = 0; i < status.inlineOutputs.size(); ++i) {
            const std::string type = i < status.inlineTypes.size() ? status.inlineTypes[i] : std::string();
            auto file = writeGenerated(status.inlineOutputs[i], type, directory, m_kind);
            if (!file) {
                cleanup();
                return std::unexpected(file.error());
            }
            files.push_back(std::move(*file));
        }
        for (const std::string& url : status.outputUrls) {
            auto file = download(url, directory, stop);
            if (!file) {
                cleanup();
                return std::unexpected(file.error());
            }
            files.push_back(std::move(*file));
        }
        return files;
    }

protected:
    struct Submitted {
        std::optional<GenerationStatus> done; ///< the outputs came with the answer
        GenerationTicket ticket;              ///< else: the provider's job
    };

    virtual std::expected<Submitted, AiError> submit(const MediaGenerationRequest& request, std::stop_token stop) = 0;
    /// The state of a provider job (adapters that always answer at once never get here).
    virtual std::expected<GenerationStatus, AiError> status(const GenerationTicket& ticket, std::stop_token stop) {
        (void)ticket, (void)stop;
        return std::unexpected(AiError(AiErrorCode::InvalidRequest, "invalid generation ticket"));
    }
    /// Downloads one output URL (each adapter names its result hosts).
    virtual std::expected<std::filesystem::path, AiError> download(const std::string& url, const std::filesystem::path& directory,
                                                                   std::stop_token stop) = 0;

    ProviderContext context() const { return ProviderContext{m_provider, m_model, m_baseUrl, m_key.clone(), m_http}; }

    /// A paid JSON submission (never retried) and its JSON answer.
    std::expected<Json, AiError> postJson(const std::string& url, const Json& body, std::string_view what, std::stop_token stop) {
        return sendForJson(m_http, paidSubmit(jsonRequest(context(), url, body, BodyMode::Buffered)), what, stop);
    }

    /// A finished status with inline outputs.
    static GenerationStatus doneWith(std::vector<std::vector<std::uint8_t>> outputs, std::vector<std::string> types) {
        GenerationStatus s;
        s.state = GenerationState::Succeeded;
        s.progress = 1.0f;
        s.inlineOutputs = std::move(outputs);
        s.inlineTypes = std::move(types);
        return s;
    }

    ProviderSpec m_provider;
    ModelSpec m_model;
    std::string m_baseUrl;
    Secret m_key;
    IHttpTransport& m_http;
    GeneratedMediaKind m_kind = GeneratedMediaKind::Image;

private:
    std::mutex m_mutex;
    std::map<std::string, GenerationStatus> m_done;
    std::atomic<int> m_serial{0};
};

} // namespace wabcut::ai::net
