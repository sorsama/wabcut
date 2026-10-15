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

    TranslateServices(LocalOpener local, ChatOpener chat) : m_local(std::move(local)), m_chat(std::move(chat)) {}

    const LocalOpener& local() const { return m_local; }
    const ChatOpener& chat() const { return m_chat; }

private:
    LocalOpener m_local;
    ChatOpener m_chat;
};

/// The Windows line breaker (Uniscribe: Thai, Lao, Khmer and Burmese between words) where available, else the simple
/// one. The app may register Qt's instead.
std::shared_ptr<const LineBreaker> systemLineBreaker();

void registerTranslateCommands(command::CommandRegistry& registry);

} // namespace wabcut::ai::translate
