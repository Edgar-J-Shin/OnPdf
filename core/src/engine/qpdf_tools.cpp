#include "onpdf/core/engine/qpdf_tools.h"

#include "onpdf/core/platform/path.h"

#include <qpdf/Constants.h>
#include <qpdf/qpdf-c.h>
#include <qpdf/qpdfjob-c.h>
#include <qpdf/qpdflogger-c.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>

namespace onpdf::engine {
namespace {

// ---- RAII ----
struct JobDeleter {
  void operator()(qpdfjob_handle h) const noexcept {
    if (h != nullptr) qpdfjob_cleanup(&h);
  }
};
using JobHandle = std::unique_ptr<std::remove_pointer_t<qpdfjob_handle>, JobDeleter>;

struct LoggerDeleter {
  void operator()(qpdflogger_handle h) const noexcept {
    if (h != nullptr) qpdflogger_cleanup(&h);
  }
};
using LoggerHandle = std::unique_ptr<std::remove_pointer_t<qpdflogger_handle>, LoggerDeleter>;

struct QpdfDeleter {
  void operator()(qpdf_data q) const noexcept {
    if (q != nullptr) qpdf_cleanup(&q);
  }
};
using QpdfHandle = std::unique_ptr<std::remove_pointer_t<qpdf_data>, QpdfDeleter>;

// JSON 문자열 리터럴 (UTF-8 그대로, 따옴표·역슬래시·제어 문자만 이스케이프)
std::string json_string(std::string_view s) {
  std::string out = "\"";
  for (const char ch : s) {
    const auto c = static_cast<unsigned char>(ch);
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04x", c);
          out += buf;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  out += "\"";
  return out;
}

std::string json_path(const std::filesystem::path& p) { return json_string(path_to_utf8(p)); }

// 로거 콜백이 모으는 텍스트
struct LogSink {
  std::string warnings;  // 줄바꿈으로 구분
  std::string errors;
};

int append_warning(char const* data, size_t len, void* udata) {
  static_cast<LogSink*>(udata)->warnings.append(data, len);
  return 0;
}
int append_error(char const* data, size_t len, void* udata) {
  static_cast<LogSink*>(udata)->errors.append(data, len);
  return 0;
}

void report_progress(int percent, void* data) {
  const auto* fn = static_cast<const ProgressFn*>(data);
  if (fn != nullptr && *fn) (*fn)(percent);
}

std::vector<std::string> split_lines(const std::string& text) {
  std::vector<std::string> lines;
  std::size_t start = 0;
  while (start < text.size()) {
    std::size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    if (end > start) lines.emplace_back(text.substr(start, end - start));
    start = end + 1;
  }
  return lines;
}

bool mentions_password(const std::string& text) {
  return text.find("invalid password") != std::string::npos || text.find("password") != std::string::npos;
}

// JSON 작업 실행. 종료 코드 0·3 → 성공(+경고), 그 외 → 오류
Result<JobReport> run_job(const std::string& json, const ProgressFn& progress) {
  JobHandle job(qpdfjob_init());
  if (!job) return make_error(ErrorCode::kJobFailed, "qpdfjob_init failed");
  LoggerHandle logger(qpdflogger_create());
  if (!logger) return make_error(ErrorCode::kJobFailed, "qpdflogger_create failed");
  LogSink sink;
  qpdflogger_set_info(logger.get(), qpdf_log_dest_discard, nullptr, nullptr);
  qpdflogger_set_warn(logger.get(), qpdf_log_dest_custom, &append_warning, &sink);
  qpdflogger_set_error(logger.get(), qpdf_log_dest_custom, &append_error, &sink);
  qpdfjob_set_logger(job.get(), logger.get());
  if (progress) qpdfjob_register_progress_reporter(job.get(), &report_progress, const_cast<ProgressFn*>(&progress));
  if (qpdfjob_initialize_from_json(job.get(), json.c_str()) != 0) {
    return make_error(ErrorCode::kJobFailed, sink.errors.empty() ? "invalid qpdf job" : sink.errors);
  }
  const int status = qpdfjob_run(job.get());
  if (status != qpdf_exit_success && status != qpdf_exit_warning) {
    const std::string text = sink.errors.empty() ? sink.warnings : sink.errors;
    if (mentions_password(text)) return make_error(ErrorCode::kPasswordRequired, text);
    return make_error(ErrorCode::kJobFailed, text.empty() ? "qpdf job failed" : text);
  }
  JobReport report;
  report.warnings = split_lines(sink.warnings);
  return report;
}

// 읽기 전용 열기 (쪽 수·권한 조회)
Result<QpdfHandle> open_for_query(const std::filesystem::path& file, std::string_view password) {
  QpdfHandle q(qpdf_init());
  if (!q) return make_error(ErrorCode::kJobFailed, "qpdf_init failed");
  qpdf_set_suppress_warnings(q.get(), QPDF_TRUE);
  const std::string path = path_to_utf8(file);
  const std::string pw(password);
  qpdf_read(q.get(), path.c_str(), pw.c_str());
  if (qpdf_has_error(q.get())) {
    const char* text = qpdf_get_error_full_text(q.get(), qpdf_get_error(q.get()));
    const std::string message = text != nullptr ? text : "cannot read";
    if (mentions_password(message)) return make_error(ErrorCode::kPasswordRequired, message);
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) return make_error(ErrorCode::kFileNotFound, message);
    return make_error(ErrorCode::kInvalidFormat, message);
  }
  return q;
}

std::string page_entry(const StructureInput& input, const std::string& range) {
  std::string entry = "{\"file\": " + json_path(input.path);
  if (!input.password.empty()) entry += ", \"password\": " + json_string(input.password);
  entry += ", \"range\": " + json_string(range) + "}";
  return entry;
}

}  // namespace

Result<JobReport> QpdfTools::merge(const std::vector<StructureInput>& inputs, const std::filesystem::path& output,
                                   const ProgressFn& progress) {
  if (inputs.empty()) return make_error(ErrorCode::kInvalidArgument, "no input files");
  if (output.empty()) return make_error(ErrorCode::kInvalidArgument, "no output path");
  for (const auto& input : inputs) {
    auto allowed = allows_extraction(input.path, input.password);
    if (!allowed) {
      Error e = allowed.error();
      e.message = path_to_utf8(input.path) + ": " + e.message;
      return std::unexpected(e);
    }
    if (!*allowed) {
      return make_error(ErrorCode::kPermissionDenied, path_to_utf8(input.path) + ": document forbids page extraction");
    }
    std::error_code ec;
    if (std::filesystem::exists(output, ec) && std::filesystem::equivalent(output, input.path, ec)) {
      return make_error(ErrorCode::kInvalidArgument, "output path is one of the inputs: " + path_to_utf8(output));
    }
  }
  std::string json = "{\"inputFile\": " + json_path(inputs.front().path);
  if (!inputs.front().password.empty()) json += ", \"password\": " + json_string(inputs.front().password);
  json += ", \"outputFile\": " + json_path(output) + ", \"pages\": [";
  for (std::size_t i = 0; i < inputs.size(); ++i) {
    if (i > 0) json += ", ";
    json += page_entry(inputs[i], "1-z");
  }
  json += "]";
  if (progress) json += ", \"progress\": \"\"";
  json += "}";
  return run_job(json, progress);
}

Result<JobReport> QpdfTools::extract(const StructureInput& input, PageRange range, const std::filesystem::path& output,
                                     const ProgressFn& progress) {
  if (range.first < 1 || range.last < range.first) return make_error(ErrorCode::kInvalidArgument, "bad page range");
  std::error_code ec;
  if (std::filesystem::exists(output, ec) && std::filesystem::equivalent(output, input.path, ec)) {
    return make_error(ErrorCode::kInvalidArgument, "output path is the input: " + path_to_utf8(output));
  }
  std::string json = "{\"inputFile\": " + json_path(input.path);
  if (!input.password.empty()) json += ", \"password\": " + json_string(input.password);
  json += ", \"outputFile\": " + json_path(output) + ", \"pages\": [" + page_entry(input, range_text(range)) + "]";
  if (progress) json += ", \"progress\": \"\"";
  json += "}";
  return run_job(json, progress);
}

Result<JobReport> QpdfTools::check(const std::filesystem::path& file, std::string_view password) {
  std::error_code ec;
  if (!std::filesystem::exists(file, ec)) return make_error(ErrorCode::kFileNotFound, "file not found: " + path_to_utf8(file));
  std::string json = "{\"inputFile\": " + json_path(file);
  if (!password.empty()) json += ", \"password\": " + json_string(password);
  json += ", \"check\": \"\"}";
  return run_job(json, {});
}

Result<int> QpdfTools::page_count(const std::filesystem::path& file, std::string_view password) {
  auto q = open_for_query(file, password);
  if (!q) return std::unexpected(q.error());
  const int n = qpdf_get_num_pages(q->get());
  if (n < 0 || qpdf_has_error(q->get())) return make_error(ErrorCode::kInvalidFormat, "cannot count pages");
  return n;
}

Result<bool> QpdfTools::allows_extraction(const std::filesystem::path& file, std::string_view password) {
  auto q = open_for_query(file, password);
  if (!q) return std::unexpected(q.error());
  return qpdf_allow_extract_all(q->get()) != 0;
}

}  // namespace onpdf::engine
