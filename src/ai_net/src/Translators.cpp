// Dedicated translation APIs (r07, docs/ai.md "Providers"; ITranslator in Capability.h), checked on 2026-11-03:
//   deepl             POST {base}/v2/translate, Authorization: DeepL-Auth-Key <key> (header auth), {"text": [...] (at most
//                     50), "target_lang": "DE" | "EN-US" | "PT-BR" | ..., "source_lang"?, "context"?} ->
//                     {"translations": [{"detected_source_language", "text"}]}. Request bodies up to 128 KiB. Free keys
//                     (ending ":fx") use api-free.deepl.com: a provider of its own. 456 = quota exceeded.
//                     developers.deepl.com/api-reference/translate
//   google-translate  POST {base}/language/translate/v2, x-goog-api-key, {"q": [...] (at most 128), "target", "source"?,
//                     "format": "text"} -> {"data": {"translations": [{"translatedText", "detectedSourceLanguage"?}]}}.
//                     cloud.google.com/translate/docs/reference/rest/v2/translate
// Segments go in batches the APIs accept; ids and order come back unchanged. Usage = characters sent (what both bill).
// Never retried: a network ambiguity must not bill a batch twice.

#include "AdapterUtil.h"

#include <wabcut/ai/JsonAccess.h>
#include <wabcut/ai_net/Adapters.h>

#include <algorithm>
#include <cctype>

namespace wabcut::ai::net {

namespace {

std::string upper(std::string text) {
    for (char& c : text) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return text;
}

/// A language tag that may go into a request: letters, digits and '-' only, short.
bool plainTag(std::string_view tag) {
    return !tag.empty() && tag.size() <= 16 &&
           std::all_of(tag.begin(), tag.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-' || c == '_'; });
}

/// DeepL's target code for an ISO 639-1 tag: regional variants where DeepL requires them ("en" -> "EN-US", "pt" -> "PT-BR",
/// "zh" -> "ZH-HANS"), a given region kept ("en-GB" -> "EN-GB").
std::string deeplTarget(std::string_view tag) {
    std::string code = upper(std::string(tag));
    std::replace(code.begin(), code.end(), '_', '-');
    if (code == "EN") {
        return "EN-US";
    }
    if (code == "PT") {
        return "PT-BR";
    }
    if (code == "ZH" || code == "ZH-CN" || code == "ZH-SG") {
        return "ZH-HANS";
    }
    if (code == "ZH-TW" || code == "ZH-HK") {
        return "ZH-HANT";
    }
    return code;
}

AiError translationError(const HttpResponse& response, std::string_view vendor) {
    AiError error = httpError(response);
    if (response.status == 456) { // DeepL: the character quota of the billing period is used up
        error.code = AiErrorCode::QuotaExceeded;
        error.message = std::string(vendor) + ": the translation quota of this billing period is used up";
    }
    return error;
}

class TranslatorBase : public ITranslator {
public:
    explicit TranslatorBase(ProviderContext context)
        : m_provider(context.provider), m_spec(context.model), m_baseUrl(std::move(context.baseUrl)), m_key(std::move(context.key)),
          m_transport(context.transport) {}

    std::expected<TranslationResult, AiError> translate(const TranslationRequest& request, std::stop_token stop) override {
        if (!plainTag(request.targetLanguage) || (!request.sourceLanguage.empty() && !plainTag(request.sourceLanguage))) {
            return std::unexpected(AiError(AiErrorCode::InvalidRequest, "not a language code"));
        }
        TranslationResult result;
        std::size_t from = 0;
        while (from < request.segments.size()) {
            // A batch: at most maxSegments() segments and about maxBytes() of text.
            std::size_t to = from;
            std::size_t bytes = 0;
            while (to < request.segments.size() && to - from < maxSegments() &&
                   (to == from || bytes + request.segments[to].text.size() <= maxBytes())) {
                bytes += request.segments[to].text.size();
                ++to;
            }
            if (bytes > maxBytes()) {
                return std::unexpected(AiError(AiErrorCode::PayloadTooLarge, "one caption is longer than " + m_provider.name + " takes"));
            }
            std::vector<std::string> texts;
            for (std::size_t i = from; i < to; ++i) {
                texts.push_back(request.segments[i].text);
            }
            auto translated = translateBatch(texts, request, stop);
            if (!translated) {
                return std::unexpected(translated.error());
            }
            if (translated->size() != texts.size()) {
                return std::unexpected(AiError(AiErrorCode::ProviderError, m_provider.name + " returned " + std::to_string(translated->size()) +
                                                                               " translations for " + std::to_string(texts.size()) + " lines"));
            }
            for (std::size_t i = from; i < to; ++i) {
                result.segments.push_back(TranslationSegment{request.segments[i].id, std::move((*translated)[i - from])});
                result.usage.characters += codePointCount(request.segments[i].text);
            }
            from = to;
        }
        return result;
    }

protected:
    virtual std::size_t maxSegments() const = 0;
    virtual std::size_t maxBytes() const = 0;
    virtual std::expected<std::vector<std::string>, AiError> translateBatch(const std::vector<std::string>& texts,
                                                                            const TranslationRequest& request, std::stop_token stop) = 0;

    ProviderContext context() const { return ProviderContext{m_provider, m_spec, m_baseUrl, m_key.clone(), m_transport}; }

    std::expected<Json, AiError> post(const std::string& url, const Json& body, std::string_view vendor, std::stop_token stop) {
        HttpRequest http = jsonRequest(context(), url, body, BodyMode::Buffered);
        http.retrySafe = false;
        http.timeout = std::chrono::seconds(120);
        auto response = m_transport.send(http, {}, stop);
        if (!response) {
            return std::unexpected(response.error());
        }
        if (!response->ok()) {
            return std::unexpected(translationError(*response, vendor));
        }
        return parseJsonBody(response->body, vendor);
    }

    ProviderSpec m_provider;
    ModelSpec m_spec;
    std::string m_baseUrl;
    Secret m_key;
    IHttpTransport& m_transport;
};

// --- DeepL ------------------------------------------------------------------------------------------------------------

class DeepLTranslator final : public TranslatorBase {
public:
    using TranslatorBase::TranslatorBase;

protected:
    std::size_t maxSegments() const override { return 50; }
    std::size_t maxBytes() const override { return 100 * 1024; } // under the 128 KiB request limit, with JSON overhead

    std::expected<std::vector<std::string>, AiError> translateBatch(const std::vector<std::string>& texts, const TranslationRequest& request,
                                                                    std::stop_token stop) override {
        Json body{{"text", texts}, {"target_lang", deeplTarget(request.targetLanguage)}};
        if (!request.sourceLanguage.empty()) {
            body["source_lang"] = upper(normalizeLanguage(request.sourceLanguage)); // source codes have no region
        }
        if (!request.context.empty()) {
            body["context"] = request.context; // not translated or billed; guides the translation
        }
        const ProviderContext c = context();
        auto j = post(m_spec.endpoint.empty() ? joinUrl(m_baseUrl, "/v2/translate") : endpointUrl(c), body, "DeepL", stop);
        if (!j) {
            return std::unexpected(j.error());
        }
        std::vector<std::string> out;
        if (const Json* translations = jsonArray(*j, "translations")) {
            for (const Json& t : *translations) {
                if (!t.is_object()) {
                    return std::unexpected(malformedEvent("DeepL translation"));
                }
                out.push_back(jsonStringOr(t, "text"));
            }
        }
        return out;
    }
};

// --- Google Cloud Translation (Basic, v2) ------------------------------------------------------------------------------

/// The text of format=text answers is plain, but decode the few entities older answers carried, to be safe.
std::string unescapeEntities(std::string text) {
    if (text.find('&') == std::string::npos) {
        return text;
    }
    static constexpr std::pair<std::string_view, std::string_view> kEntities[] = {
        {"&#39;", "'"}, {"&quot;", "\""}, {"&lt;", "<"}, {"&gt;", ">"}, {"&amp;", "&"}};
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        bool replaced = false;
        if (text[i] == '&') {
            for (const auto& [entity, plain] : kEntities) {
                if (std::string_view(text).substr(i, entity.size()) == entity) {
                    out += plain;
                    i += entity.size();
                    replaced = true;
                    break;
                }
            }
        }
        if (!replaced) {
            out += text[i++];
        }
    }
    return out;
}

class GoogleTranslator final : public TranslatorBase {
public:
    using TranslatorBase::TranslatorBase;

protected:
    std::size_t maxSegments() const override { return 128; }
    std::size_t maxBytes() const override { return 24 * 1024; } // Google recommends at most 5,000 characters x a few

    std::expected<std::vector<std::string>, AiError> translateBatch(const std::vector<std::string>& texts, const TranslationRequest& request,
                                                                    std::stop_token stop) override {
        std::string target = std::string(request.targetLanguage);
        std::replace(target.begin(), target.end(), '_', '-');
        Json body{{"q", texts}, {"target", target}, {"format", "text"}};
        if (!request.sourceLanguage.empty()) {
            body["source"] = normalizeLanguage(request.sourceLanguage);
        }
        const ProviderContext c = context();
        auto j = post(m_spec.endpoint.empty() ? joinUrl(m_baseUrl, "/language/translate/v2") : endpointUrl(c), body, "Google Cloud Translation",
                      stop);
        if (!j) {
            return std::unexpected(j.error());
        }
        std::vector<std::string> out;
        if (const Json* translations = jsonAt(*j, {"data", "translations"}); translations && translations->is_array()) {
            for (const Json& t : *translations) {
                if (!t.is_object()) {
                    return std::unexpected(malformedEvent("Google translation"));
                }
                out.push_back(unescapeEntities(jsonStringOr(t, "translatedText")));
            }
        }
        return out;
    }
};

} // namespace

std::unique_ptr<ITranslator> makeDeepLTranslator(ProviderContext context) { return std::make_unique<DeepLTranslator>(std::move(context)); }
std::unique_ptr<ITranslator> makeGoogleTranslator(ProviderContext context) { return std::make_unique<GoogleTranslator>(std::move(context)); }

} // namespace wabcut::ai::net
