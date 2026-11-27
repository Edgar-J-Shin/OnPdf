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
  kSaveVerifyFailed,   // 저장한 파일을 다시 열어 검증하지 못했다 (원본은 그대로)
  kNotWritable,        // 대상 파일에 쓸 수 없다: 읽기 전용·잠금·다른 프로그램이 사용 중
  kFolderNotWritable,  // 대상 폴더에 새 파일(임시 파일)을 만들 수 없다 — 다른 폴더에 저장해야 한다
  kPermissionDenied,   // 문서 권한이 그 동작을 허용하지 않는다 (예: 추출 금지 문서를 삽입 소스로)
  kJobFailed,          // 구조 작업(qpdf 병합·분할·검사)이 오류로 끝났다. message 에 qpdf 의 설명
  kUnsupported,  // 엔진이 할 수 없는 동작 (예: 라디오 그룹 선택 해제 — PDFium 공개 API 에 없음)
  kRenderRefused,  // 렌더 위험(#121)이 있는 쪽이라 PDFium 을 부르지 않고 거절했다. message 에 사유
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
    case ErrorCode::kUnknown:
      return "unknown";
    case ErrorCode::kFileNotFound:
      return "file not found";
    case ErrorCode::kIoError:
      return "I/O error";
    case ErrorCode::kInvalidFormat:
      return "invalid PDF format";
    case ErrorCode::kPasswordRequired:
      return "password required";
    case ErrorCode::kSecurityError:
      return "security error";
    case ErrorCode::kPageNotFound:
      return "page not found";
    case ErrorCode::kRenderFailed:
      return "render failed";
    case ErrorCode::kInvalidArgument:
      return "invalid argument";
    case ErrorCode::kCancelled:
      return "cancelled";
    case ErrorCode::kInvalidState:
      return "invalid state";
    case ErrorCode::kSaveVerifyFailed:
      return "saved file failed verification";
    case ErrorCode::kNotWritable:
      return "file is not writable";
    case ErrorCode::kFolderNotWritable:
      return "cannot create files in folder";
    case ErrorCode::kPermissionDenied:
      return "operation not permitted by document permissions";
    case ErrorCode::kJobFailed:
      return "structure job failed";
    case ErrorCode::kUnsupported:
      return "not supported";
    case ErrorCode::kRenderRefused:
      return "render refused";
  }
  return "unknown";
}

}  // namespace onpdf
