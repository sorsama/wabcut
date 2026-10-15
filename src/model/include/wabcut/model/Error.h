#pragma once

// Shared error type for std::expected<T, wabcut::Error> across module boundaries.

#include <string>
#include <utility>

namespace wabcut {

enum class ErrorCode {
    Unknown,
    InvalidArgument,
    NotFound,
    IoError,
    ParseError,
    Unsupported,
    VersionMismatch,
    DecoderError,
    EncoderError,
    DeviceError,
    Cancelled,
};

struct Error {
    ErrorCode code = ErrorCode::Unknown;
    std::string message;

    Error() = default;
    Error(ErrorCode c, std::string msg) : code(c), message(std::move(msg)) {}
};

} // namespace wabcut
