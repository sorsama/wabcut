#include <wabcut/ai/AiError.h>

namespace wabcut::ai {

std::string_view toString(AiErrorCode code) {
    switch (code) {
    case AiErrorCode::NoCredentials:
        return "NoCredentials";
    case AiErrorCode::Unauthorized:
        return "Unauthorized";
    case AiErrorCode::RateLimited:
        return "RateLimited";
    case AiErrorCode::QuotaExceeded:
        return "QuotaExceeded";
    case AiErrorCode::PayloadTooLarge:
        return "PayloadTooLarge";
    case AiErrorCode::Network:
        return "Network";
    case AiErrorCode::ProviderError:
        return "ProviderError";
    case AiErrorCode::Cancelled:
        return "Cancelled";
    case AiErrorCode::ModelMissing:
        return "ModelMissing";
    case AiErrorCode::Deprecated:
        return "Deprecated";
    case AiErrorCode::InvalidRequest:
        return "InvalidRequest";
    case AiErrorCode::Unsupported:
        return "Unsupported";
    case AiErrorCode::ConsentRequired:
        return "ConsentRequired";
    case AiErrorCode::Offline:
        return "Offline";
    case AiErrorCode::SpendCapExceeded:
        return "SpendCapExceeded";
    case AiErrorCode::InsufficientResources:
        return "InsufficientResources";
    }
    return "Unknown";
}

bool AiError::isRetryable() const {
    switch (code) {
    case AiErrorCode::RateLimited:
    case AiErrorCode::Network:
        return true;
    case AiErrorCode::ProviderError:
        return httpStatus >= 500;
    default:
        return false;
    }
}

Error toError(const AiError& error) {
    ErrorCode code = ErrorCode::Unknown;
    switch (error.code) {
    case AiErrorCode::Cancelled:
        code = ErrorCode::Cancelled;
        break;
    case AiErrorCode::Network:
        code = ErrorCode::IoError;
        break;
    case AiErrorCode::InvalidRequest:
        code = ErrorCode::InvalidArgument;
        break;
    case AiErrorCode::ModelMissing:
        code = ErrorCode::NotFound;
        break;
    case AiErrorCode::Unsupported:
        code = ErrorCode::Unsupported;
        break;
    default:
        break;
    }
    std::string message(toString(error.code));
    if (!error.message.empty()) {
        message += ": ";
        message += error.message;
    }
    return Error(code, std::move(message));
}

} // namespace wabcut::ai
