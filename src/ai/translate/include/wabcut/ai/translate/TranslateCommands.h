#pragma once

// captions.translate (AI-12): a caption track into new caption tracks, one per language (CaptionTranslation.h). The
// host decides where text may go: a local model (TranslateServices::local, the wabcut-embed helper) needs nothing
// else; the user's cloud chat model (TranslateServices::chat) is refused for a no-cloud project and goes through the
// platform's consent, spend cap and request log. "estimate": true prices the cloud run without sending anything.

#include <wabcut/ai/translate/CaptionTranslation.h>
#include <wabcut/command/ExecutionContext.h>
#include <wabcut/command/Registry.h>

#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace wabcut::ai::translate {

class TranslateServices : public command::CommandServices {
public:
    /// A local translator for the pair, or Unsupported / NotFound (no model for it, or not downloaded).
    using LocalOpener =
        std::function<std::expected<std::unique_ptr<ITranslator>, AiError>(std::string_view source, std::string_view target)>;
    /// The user's chat model, guarded by the platform; NoCredentials when none is set up.
    using ChatOpener = std::function<std::expected<std::unique_ptr<IChatModel>, AiError>()>;
    /// r07: a dedicated translation service (DeepL, Google Cloud Translation) that the user routed caption translation to
    /// (Settings > AI > Features), guarded by the platform. When the host gives one, it takes the chat model's place.
    struct Service {
        std::unique_ptr<ITranslator> translator;
        ModelSpec spec; ///< prices the estimate (per character) and names the model in the result
    };
    using ServiceOpener = std::function<std::expected<Service, AiError>()>;

    /// `routedLocal` (r07, QA-07): the chat model or service runs on this computer, so a no-cloud project may use it.
    TranslateServices(LocalOpener local, ChatOpener chat, ServiceOpener service = {}, bool routedLocal = false)
        : m_local(std::move(local)), m_chat(std::move(chat)), m_service(std::move(service)), m_routedLocal(routedLocal) {}

    const LocalOpener& local() const { return m_local; }
    const ChatOpener& chat() const { return m_chat; }
    const ServiceOpener& service() const { return m_service; }
    bool routedLocal() const { return m_routedLocal; }

private:
    LocalOpener m_local;
    ChatOpener m_chat;
    ServiceOpener m_service;
    bool m_routedLocal = false;
};

/// The Windows line breaker (Uniscribe: Thai, Lao, Khmer and Burmese between words) where available, else the simple
/// one. The app may register Qt's instead.
std::shared_ptr<const LineBreaker> systemLineBreaker();

void registerTranslateCommands(command::CommandRegistry& registry);

} // namespace wabcut::ai::translate
