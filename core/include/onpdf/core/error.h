#pragma once

#include <expected>
#include <string>

namespace onpdf {

enum class ErrorCode {
  kUnknown,
  kFileNotFound,
  kIoError,
  kInvalidFormat,
  kPasswordRequired,
  kSecurityError,
  kPageNotFound,
  kRenderFailed,
  kInvalidArgument,
  kCancelled,
  kInvalidState,
};

struct Error {
  ErrorCode code = ErrorCode::kUnknown;
  std::string message;
};

template <class T>
using Result = std::expected<T, Error>;

inline std::unexpected<Error> make_error(ErrorCode code, std::string message) {
  return std::unexpected(Error{code, std::move(message)});
}

inline const char* to_string(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::kUnknown: return "unknown";
    case ErrorCode::kFileNotFound: return "file not found";
    case ErrorCode::kIoError: return "I/O error";
    case ErrorCode::kInvalidFormat: return "invalid PDF format";
    case ErrorCode::kPasswordRequired: return "password required";
    case ErrorCode::kSecurityError: return "security error";
    case ErrorCode::kPageNotFound: return "page not found";
    case ErrorCode::kRenderFailed: return "render failed";
    case ErrorCode::kInvalidArgument: return "invalid argument";
    case ErrorCode::kCancelled: return "cancelled";
    case ErrorCode::kInvalidState: return "invalid state";
  }
  return "unknown";
}

}  // namespace onpdf
