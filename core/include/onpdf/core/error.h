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
  kSaveVerifyFailed,  // 저장한 파일을 다시 열어 검증하지 못했다 (원본은 그대로)
  kNotWritable,       // 대상 파일에 쓸 수 없다: 읽기 전용·잠금·다른 프로그램이 사용 중
  kFolderNotWritable, // 대상 폴더에 새 파일(임시 파일)을 만들 수 없다 — 다른 폴더에 저장해야 한다
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
    case ErrorCode::kSaveVerifyFailed: return "saved file failed verification";
    case ErrorCode::kNotWritable: return "file is not writable";
    case ErrorCode::kFolderNotWritable: return "cannot create files in folder";
  }
  return "unknown";
}

}  // namespace onpdf
