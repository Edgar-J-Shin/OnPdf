#include "onpdf/core/engine/qpdf_tools.h"

#include "onpdf/core/fnv.h"
#include "onpdf/core/platform/path.h"

#include "cff_charset.h"
#include <qpdf/Constants.h>
#include <qpdf/qpdf-c.h>
#include <qpdf/qpdfjob-c.h>
#include <qpdf/qpdflogger-c.h>

// qpdf C++ API (수술, #65). 헤더가 -Wshadow 경고를 낸다
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#endif
#include <qpdf/Buffer.hh>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFWriter.hh>
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <optional>
#include <span>
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
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
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

std::string json_path(const std::filesystem::path& p) {
  return json_string(path_to_utf8(p));
}

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

// qpdf 가 암호 오류를 낼 때의 정확한 문구("<파일>: invalid password")만 본다 — 파일 경로에
// "password" 가 들어간 손상 파일을 암호 오류로 오판하지 않게 (리뷰 W1)
bool is_password_error(const std::string& text) {
  for (const std::string& line : split_lines(text)) {
    const std::string needle = ": invalid password";
    if (line.size() >= needle.size() &&
        line.compare(line.size() - needle.size(), needle.size(), needle) == 0)
      return true;
  }
  return false;
}

// 오류 텍스트 정리: qpdf 접두("qpdfjob json: ")와 끝 개행 제거 (대화상자에 그대로 보인다)
std::string tidy_error(std::string text) {
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
    text.pop_back();
  const std::string prefix = "qpdfjob json: ";
  if (text.rfind(prefix, 0) == 0) text.erase(0, prefix.size());
  return text;
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
  if (progress)
    qpdfjob_register_progress_reporter(job.get(), &report_progress,
                                       const_cast<ProgressFn*>(&progress));
  if (qpdfjob_initialize_from_json(job.get(), json.c_str()) != 0) {
    return make_error(ErrorCode::kJobFailed,
                      sink.errors.empty() ? "invalid qpdf job" : tidy_error(sink.errors));
  }
  const int status = qpdfjob_run(job.get());
  if (status != qpdf_exit_success && status != qpdf_exit_warning) {
    const std::string text = tidy_error(sink.errors.empty() ? sink.warnings : sink.errors);
    if (is_password_error(text)) return make_error(ErrorCode::kPasswordRequired, text);
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
    qpdf_error err = qpdf_get_error(q.get());
    const char* text = qpdf_get_error_full_text(q.get(), err);
    const std::string message = tidy_error(text != nullptr ? text : "cannot read");
    if (qpdf_get_error_code(q.get(), err) == qpdf_e_password)
      return make_error(ErrorCode::kPasswordRequired, message);
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

Result<JobReport> QpdfTools::merge(const std::vector<StructureInput>& inputs,
                                   const std::filesystem::path& output,
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
      return make_error(ErrorCode::kPermissionDenied,
                        path_to_utf8(input.path) + ": document forbids page extraction");
    }
    std::error_code ec;
    if (std::filesystem::exists(output, ec) &&
        std::filesystem::equivalent(output, input.path, ec)) {
      return make_error(ErrorCode::kInvalidArgument,
                        "output path is one of the inputs: " + path_to_utf8(output));
    }
  }
  std::string json = "{\"inputFile\": " + json_path(inputs.front().path);
  if (!inputs.front().password.empty())
    json += ", \"password\": " + json_string(inputs.front().password);
  json += ", \"outputFile\": " + json_path(output) + ", \"pages\": [";
  for (std::size_t i = 0; i < inputs.size(); ++i) {
    if (i > 0) json += ", ";
    json += page_entry(inputs[i], "1-z");
  }
  json += "], \"decrypt\": \"\"";  // 주 입력이 암호 문서여도 결과는 평문 (리뷰 C1: 그렇지 않으면
                                   // 검증이 암호 없이 열지 못한다)
  if (progress) json += ", \"progress\": \"\"";
  json += "}";
  return run_job(json, progress);
}

Result<JobReport> QpdfTools::extract(const StructureInput& input, PageRange range,
                                     const std::filesystem::path& output,
                                     const ProgressFn& progress) {
  if (range.first < 1 || range.last < range.first)
    return make_error(ErrorCode::kInvalidArgument, "bad page range");
  std::error_code ec;
  if (std::filesystem::exists(output, ec) && std::filesystem::equivalent(output, input.path, ec)) {
    return make_error(ErrorCode::kInvalidArgument,
                      "output path is the input: " + path_to_utf8(output));
  }
  std::string json = "{\"inputFile\": " + json_path(input.path);
  if (!input.password.empty()) json += ", \"password\": " + json_string(input.password);
  json += ", \"outputFile\": " + json_path(output) + ", \"pages\": [" +
          page_entry(input, range_text(range)) + "], \"decrypt\": \"\"";
  if (progress) json += ", \"progress\": \"\"";
  json += "}";
  return run_job(json, progress);
}

Result<JobReport> QpdfTools::check(const std::filesystem::path& file, std::string_view password) {
  std::error_code ec;
  if (!std::filesystem::exists(file, ec))
    return make_error(ErrorCode::kFileNotFound, "file not found: " + path_to_utf8(file));
  std::string json = "{\"inputFile\": " + json_path(file);
  if (!password.empty()) json += ", \"password\": " + json_string(password);
  json += ", \"check\": \"\"}";
  return run_job(json, {});
}

namespace {

// OnPdf 가 넣은 CFF 서브셋 글꼴을 규격대로 (#73). 바꾼 글꼴 수를 돌려준다
int convert_cff_fonts(QPDF& q, const std::vector<std::string>& keys) {
  int converted = 0;
  for (auto& font : q.getAllObjects()) {
    if (!font.isDictionaryOfType("/Font") ||
        !font.getKey("/Subtype").isNameAndEquals("/CIDFontType2"))
      continue;
    QPDFObjectHandle desc = font.getKey("/FontDescriptor");
    if (!desc.isDictionary()) continue;
    QPDFObjectHandle file = desc.getKey("/FontFile2");
    if (!file.isStream()) continue;
    // PDFium 이 넣은 글꼴만 후보: /CIDToGIDMap(스트림)이 있고 FontFile2 사전에 /Subtype 이 없다.
    // 다른 글꼴은 풀지도 않는다 (리뷰 W5)
    if (!font.hasKey("/CIDToGIDMap") || file.getDict().hasKey("/Subtype")) continue;
    try {
      std::shared_ptr<Buffer> data = file.getStreamData(qpdf_dl_all);
      if (data == nullptr || data->getSize() < 4 || std::memcmp(data->getBuffer(), "OTTO", 4) != 0)
        continue;
      std::vector<std::uint8_t> otf(data->getBuffer(), data->getBuffer() + data->getSize());
      if (std::find(keys.begin(), keys.end(), onpdf::fnv1a_hex(otf)) == keys.end())
        continue;  // 우리 글꼴만
      const auto info = detail::inspect_cff_charset(otf);
      if (!info) continue;
      // CID-keyed 면 charset 을 항등으로 (자리가 모자라면 그대로 둔다 — FreeType 뷰어는 여전히
      // 그린다). 비CID CFF 는 규격상 CID = GID 라 charset 을 손대지 않는다 (리뷰 W4)
      if (info->cid_keyed && !detail::write_identity_charset(otf)) continue;
      file.replaceStreamData(std::string(otf.begin(), otf.end()), QPDFObjectHandle::newNull(),
                             QPDFObjectHandle::newNull());
      file.getDict().replaceKey("/Subtype", QPDFObjectHandle::newName("/OpenType"));
      file.getDict().removeKey("/Length1");
      desc.removeKey("/FontFile2");
      desc.replaceKey("/FontFile3", file);
      font.replaceKey("/Subtype", QPDFObjectHandle::newName("/CIDFontType0"));
      font.removeKey("/CIDToGIDMap");
      ++converted;
    } catch (const std::exception&) {
      continue;  // 풀 수 없는 스트림(손상 필터)은 건너뛴다 — 다른 글꼴 변환·미참조 정리는 계속
    }
  }
  return converted;
}

}  // namespace

Result<JobReport> QpdfTools::rewrite(const std::filesystem::path& input,
                                     const std::filesystem::path& output, std::string_view password,
                                     const RewriteOptions& options) {
  std::error_code ec;
  if (!std::filesystem::exists(input, ec))
    return make_error(ErrorCode::kFileNotFound, "file not found: " + path_to_utf8(input));
  if (std::filesystem::exists(output, ec) && std::filesystem::equivalent(input, output, ec)) {
    return make_error(ErrorCode::kInvalidArgument, "output must differ from input");
  }
  if (!options.convert_cff_keys.empty()) {
    // 객체 모델 경로: 글꼴 사전을 고친 뒤 쓴다. QPDFWriter 는 트레일러에서 닿는 객체만 쓰므로
    // 미참조 폐기는 같다
    try {
      QPDF q;
      q.setSuppressWarnings(true);
      const std::string pw(password);
      q.processFile(path_to_utf8(input).c_str(), pw.empty() ? nullptr : pw.c_str());
      JobReport report;
      report.converted_fonts = convert_cff_fonts(q, options.convert_cff_keys);
      QPDFWriter w(q, path_to_utf8(output).c_str());
      w.setObjectStreamMode(qpdf_o_preserve);
      w.setPreserveEncryption(true);
      w.write();
      for (const auto& warning : q.getWarnings()) report.warnings.push_back(warning.what());
      return report;
    } catch (const std::exception& e) {
      const std::string text = e.what();
      if (is_password_error(text)) return make_error(ErrorCode::kPasswordRequired, text);
      return make_error(ErrorCode::kJobFailed, text);
    }
  }
  std::string json = "{\"inputFile\": " + json_path(input);
  if (!password.empty()) json += ", \"password\": " + json_string(password);
  // decrypt 를 넣지 않으면 입력의 암호화가 그대로 유지된다 (병합·분할과 다르다 — 저장 검증은 같은
  // 암호로 다시 연다)
  json += ", \"outputFile\": " + json_path(output) + ", \"objectStreams\": \"preserve\"}";
  return run_job(json, {});
}

Result<FontCensus> QpdfTools::font_census(const std::filesystem::path& file,
                                          std::string_view password) {
  try {
    QPDF q;
    q.setSuppressWarnings(true);
    const std::string pw(password);
    q.processFile(path_to_utf8(file).c_str(), pw.empty() ? nullptr : pw.c_str());
    FontCensus census;
    for (auto& oh : q.getAllObjects()) {
      if (!oh.isDictionaryOfType("/Font")) continue;
      try {
        if (oh.getKey("/Subtype").isNameAndEquals("/CIDFontType2")) ++census.cid_type2;
        if (oh.getKey("/Subtype").isNameAndEquals("/CIDFontType0")) ++census.cid_type0;
        if (oh.hasKey("/CIDToGIDMap")) ++census.cid_to_gid_maps;
        QPDFObjectHandle desc = oh.getKey("/FontDescriptor");
        if (!desc.isDictionary()) continue;
        if (QPDFObjectHandle f2 = desc.getKey("/FontFile2"); f2.isStream()) {
          auto data = f2.getStreamData(qpdf_dl_all);
          if (data != nullptr && data->getSize() >= 4 &&
              std::memcmp(data->getBuffer(), "OTTO", 4) == 0)
            ++census.fontfile2_otto;
        }
        if (QPDFObjectHandle f3 = desc.getKey("/FontFile3");
            f3.isStream() && f3.getDict().getKey("/Subtype").isNameAndEquals("/OpenType")) {
          ++census.fontfile3_opentype;
          auto data = f3.getStreamData(qpdf_dl_all);
          if (data != nullptr) {
            const auto info = detail::inspect_cff_charset(
                std::span<const std::uint8_t>(data->getBuffer(), data->getSize()));
            if (info && info->cid_keyed && !info->identity) census.charsets_identity = false;
          }
        }
      } catch (const std::exception&) {
        continue;  // 풀 수 없는 글꼴 스트림은 세지 않는다
      }
    }
    return census;
  } catch (const std::exception& e) {
    const std::string text = e.what();
    if (is_password_error(text)) return make_error(ErrorCode::kPasswordRequired, text);
    return make_error(ErrorCode::kJobFailed, text);
  }
}

Result<int> QpdfTools::page_count(const std::filesystem::path& file, std::string_view password) {
  auto q = open_for_query(file, password);
  if (!q) return std::unexpected(q.error());
  const int n = qpdf_get_num_pages(q->get());
  if (n < 0 || qpdf_has_error(q->get()))
    return make_error(ErrorCode::kInvalidFormat, "cannot count pages");
  return n;
}

Result<int> QpdfTools::direct_annotation_count(const std::filesystem::path& file,
                                               std::string_view password) {
  auto q = open_for_query(file, password);
  if (!q) return std::unexpected(q.error());
  qpdf_data data = q->get();
  const int pages = qpdf_get_num_pages(data);
  if (pages < 0) return make_error(ErrorCode::kJobFailed, "cannot read pages");
  int direct = 0;
  for (int i = 0; i < pages; ++i) {
    const qpdf_oh page = qpdf_get_page_n(data, static_cast<std::size_t>(i));
    if (!qpdf_oh_has_key(data, page, "/Annots")) continue;
    const qpdf_oh annots = qpdf_oh_get_key(data, page, "/Annots");
    if (!qpdf_oh_is_array(data, annots)) continue;
    const int count = qpdf_oh_get_array_n_items(data, annots);
    for (int k = 0; k < count; ++k) {
      const qpdf_oh item = qpdf_oh_get_array_item(data, annots, k);
      if (qpdf_oh_is_dictionary(data, item) && !qpdf_oh_is_indirect(data, item)) ++direct;
    }
  }
  if (qpdf_has_error(data)) return make_error(ErrorCode::kJobFailed, "cannot read annotations");
  return direct;
}

Result<bool> QpdfTools::allows_extraction(const std::filesystem::path& file,
                                          std::string_view password) {
  auto q = open_for_query(file, password);
  if (!q) return std::unexpected(q.error());
  return qpdf_allow_extract_all(q->get()) != 0;
}

// ---- 콘텐츠 스트림 수술 (#65) ----
namespace {

bool non_empty_show(const std::string& op, const std::vector<QPDFObjectHandle>& operands) {
  if (operands.empty()) return false;
  const QPDFObjectHandle& last = operands.back();
  if (op == "TJ") {
    if (!last.isArray()) return false;
    for (auto& item : last.getArrayAsVector()) {
      if (item.isString() && !item.getStringValue().empty()) return true;
    }
    return false;
  }
  return last.isString() && !last.getStringValue().empty();
}

struct ContentOp {
  std::string name;
  std::size_t start = 0;  // 첫 피연산자(없으면 연산자) 바이트 위치
  std::size_t end = 0;
  std::vector<QPDFObjectHandle> operands;
};

// parseContentStream 콜백: 연산자마다 바이트 범위와 피연산자를 모은다
struct CollectOps : QPDFObjectHandle::ParserCallbacks {
  std::vector<ContentOp> ops;
  std::vector<QPDFObjectHandle> pending;
  std::size_t pending_start = 0;
  bool has_pending = false;
  void handleObject(QPDFObjectHandle obj, size_t offset, size_t length) override {
    if (obj.isOperator()) {
      ContentOp op;
      op.name = obj.getOperatorValue();
      op.start = has_pending ? pending_start : offset;
      op.end = offset + length;
      op.operands = std::move(pending);
      ops.push_back(std::move(op));
      pending.clear();
      has_pending = false;
    } else {
      if (!has_pending) pending_start = offset;
      has_pending = true;
      pending.push_back(obj);
    }
  }
  bool dangling =
      false;  // 스트림이 피연산자만 남긴 채 끝났다 (연산자가 다음 스트림에) — 순번을 믿을 수 없다
  void handleEOF() override { dangling = has_pending; }
};

bool is_show_op(const std::string& n) {
  return n == "Tj" || n == "TJ" || n == "'" || n == "\"";
}
bool sets_text_line(const std::string& n) {
  return n == "Td" || n == "TD" || n == "Tm" || n == "T*" || n == "'" || n == "\"" || n == "BT";
}

// data 에서 ordinals(비어 있지 않은 표시 연산자 순번)의 연산자만 지운 바이트. 지운 자리는 공백,
// '→T*, "→Tw Tc T*. 쪽 콘텐츠가 여러 스트림이면 이어 붙인 바이트에서 한 번에 센다 (#81)
// 바이트 범위 치환 목록. 쪽 콘텐츠가 여러 스트림이면 이어 붙인 바이트에서 연산자를 세고(PDFium 과
// 같은 규칙 — Word·LiveCycle 은 피연산자와 연산자 사이에서 스트림을 나눈다, #81) 치환은 스트림마다
// 그 범위 조각에 적용한다
struct ContentEdit {
  std::size_t start = 0;
  std::size_t end = 0;
  std::string text;  // 범위를 지운 자리에 넣을 글 (범위가 끝나는 스트림에)
};
struct RewriteResult {
  std::vector<ContentEdit> edits;
  int show_ops = 0;
  bool dangling = false;
};

// data[from, to) 에 치환을 적용한 결과. 범위가 조각을 넘어가면 조각 안 부분만 지우고 text 는 끝나는
// 조각에만 넣는다
std::string apply_edits(const std::string& data, const std::vector<ContentEdit>& edits,
                        std::size_t from, std::size_t to, bool* changed) {
  std::string out;
  std::size_t pos = from;
  for (const ContentEdit& e : edits) {
    if (e.end <= from || e.start >= to) continue;
    const std::size_t cut_start = std::max(e.start, from), cut_end = std::min(e.end, to);
    if (cut_start > pos) out.append(data, pos, cut_start - pos);
    if (e.end <= to) out += e.text;
    pos = cut_end;
    if (changed != nullptr) *changed = true;
  }
  if (pos < to) out.append(data, pos, to - pos);
  return out;
}
// fit 대상 연산자를 `[<c1> k <c2> … <cn>] TJ` 로 (PDFium 생성기 형태 `[<hex>] TJ` 만). 형태가
// 다르면 nullopt
// fit 대상 표시 연산자(Tj/TJ)를 `<cs> Tc <원래 피연산자와 연산자> 0 Tc` 로. 피연산자 바이트는
// 그대로 둔다
std::optional<std::string> fitted_operands(const ContentOp& op, std::string_view op_bytes,
                                           const ShowOpFit& fit) {
  if ((op.name != "TJ" && op.name != "Tj") || op.operands.empty()) return std::nullopt;
  // 숫자는 로캘과 무관하게 (snprintf 는 LC_NUMERIC 을 따라 "1,50" 이 될 수 있다 — 리뷰 W1)
  const std::string number = QPDFObjectHandle::newReal(fit.char_space, 3).unparse();
  return number + " Tc " + std::string(op_bytes) + " 0 Tc";
}

RewriteResult rewrite_content(QPDF& owner, const std::string& data, const std::set<int>& ordinals,
                              int ordinal_base, int& needs_displacement,
                              std::vector<std::string>* do_names,
                              const std::vector<ShowOpFit>& fits = {}, int* fitted = nullptr,
                              int* already = nullptr) {
  CollectOps c;
  auto stream = QPDFObjectHandle::newStream(
      &owner, data);  // 파서용 임시 객체 (트레일러에서 닿지 않아 쓰이진 않는다)
  QPDFObjectHandle::parseContentStream(stream, &c);
  RewriteResult r;
  r.dangling = c.dangling;
  std::vector<int> ordinal_of(c.ops.size(), -1);
  int n = ordinal_base;  // 쪽 콘텐츠가 여러 스트림이면 순번은 스트림을 이어서 센다
  for (std::size_t i = 0; i < c.ops.size(); ++i) {
    if (is_show_op(c.ops[i].name) && non_empty_show(c.ops[i].name, c.ops[i].operands))
      ordinal_of[i] = n++;
    if (do_names != nullptr && c.ops[i].name == "Do" && !c.ops[i].operands.empty() &&
        c.ops[i].operands.back().isName()) {
      do_names->push_back(c.ops[i].operands.back().getName());
    }
  }
  r.show_ops = n - ordinal_base;
  // 연산자 시점의 유효 Tc (텍스트 상태는 BT/ET 를 넘어 유지되고 q/Q 로 저장·복원된다) — fit 의 멱등
  // 판정용 (#87)
  std::vector<double> tc_at(c.ops.size(), 0.0);
  if (!fits.empty()) {
    double tc = 0.0;
    std::vector<double> saved;
    const auto number = [](const QPDFObjectHandle& h, double fallback) {
      return h.isNumber() ? h.getNumericValue() : fallback;
    };
    for (std::size_t i = 0; i < c.ops.size(); ++i) {
      tc_at[i] = tc;  // 표시 연산자 자신이 바꾸는 값(")은 그 글자에 적용되지만 fit 대상이 아니다
      const ContentOp& op = c.ops[i];
      if (op.name == "q") {
        saved.push_back(tc);
      } else if (op.name == "Q") {
        tc = saved.empty() ? 0.0 : saved.back();
        if (!saved.empty()) saved.pop_back();
      } else if (op.name == "Tc" && !op.operands.empty()) {
        tc = number(op.operands.back(), tc);
      } else if (op.name == "\"" && op.operands.size() >= 3) {
        tc = number(op.operands[op.operands.size() - 2], tc);
      }
    }
  }
  // 앞 스트림에서 넘어온 밀림 검사: 이 스트림 첫머리에서 ET·위치 연산자 전에 남는 표시 연산자가
  // 오면 밀린다
  auto follower_shifts = [&](std::size_t from) {
    for (std::size_t k = from; k < c.ops.size(); ++k) {
      const std::string& nm = c.ops[k].name;
      if (nm == "ET" || sets_text_line(nm)) return false;  // 끝남
      if ((nm == "Tj" || nm == "TJ") && ordinal_of[k] >= 0 && !ordinals.count(ordinal_of[k]))
        return true;  // 밀림
    }
    return false;  // 콘텐츠 끝 (ET 없이 끝난 BT 는 그릴 글자가 없다)
  };
  for (std::size_t i = 0; i < c.ops.size(); ++i) {
    if (ordinal_of[i] < 0) continue;
    if (!ordinals.count(ordinal_of[i])) {
      const auto fit = std::find_if(fits.begin(), fits.end(),
                                    [&](const ShowOpFit& f) { return f.ordinal == ordinal_of[i]; });
      if (fit == fits.end()) continue;
      if (std::abs(tc_at[i] - fit->char_space) < 1e-3) {
        if (already != nullptr) ++*already;
        continue;
      }
      const auto replaced = fitted_operands(
          c.ops[i], std::string_view(data).substr(c.ops[i].start, c.ops[i].end - c.ops[i].start),
          *fit);
      if (!replaced) continue;  // 예상 형태가 아니면 자간만 건너뛴다 (수정은 성공)
      r.edits.push_back({c.ops[i].start, c.ops[i].end, *replaced});
      if (fitted != nullptr) ++*fitted;
      continue;
    }
    const ContentOp& op = c.ops[i];
    std::string text = " ";
    if (op.name == "'") {
      text = " T* ";
    } else if (op.name == "\"") {
      // aw ac string " : 마지막 세 피연산자 (PDFium 도 끝에서 센다). 줄 이동은 피연산자가 모자라도
      // 일어난다
      const std::size_t m = op.operands.size();
      text.clear();
      if (m >= 3)
        text += " " + op.operands[m - 3].unparse() + " Tw " + op.operands[m - 2].unparse() + " Tc";
      text += " T* ";
    }
    r.edits.push_back({op.start, op.end, text});
    // 같은 BT 안에서 다음 표시 연산자가 줄 위치 연산자 없이 이어지면 그 글자가 밀린다 (Tj/TJ 는 줄
    // 행렬을 바꾸지 않는다)
    if (follower_shifts(i + 1)) ++needs_displacement;
  }
  return r;
}

std::string stream_bytes(QPDFObjectHandle h) {
  auto buf = h.getStreamData(qpdf_dl_all);
  return {reinterpret_cast<const char*>(buf->getBuffer()), buf->getSize()};
}

}  // namespace

Result<SurgeryReport> QpdfTools::remove_text_show_ops(const std::vector<std::uint8_t>& in,
                                                      std::string_view password, int page_index,
                                                      const ShowOpSurgery& ops) {
  SurgeryReport report;
  try {
    QPDF q;
    q.setSuppressWarnings(true);
    const std::string pw(password);
    q.processMemoryFile("document", reinterpret_cast<const char*>(in.data()), in.size(),
                        pw.empty() ? nullptr : pw.c_str());
    auto pages = QPDFPageDocumentHelper(q).getAllPages();
    if (page_index < 0 || page_index >= static_cast<int>(pages.size())) {
      return make_error(ErrorCode::kInvalidArgument, "page index out of range");
    }
    QPDFPageObjectHelper& page = pages[static_cast<std::size_t>(page_index)];
    // /Contents 가 배열이면 스트림마다 따로 다룬다 — 하나로 합치면 PDFium 이 뒤의 편집에서 원본
    // 스트림까지 다시 쓴다(손실)
    std::vector<QPDFObjectHandle> streams;
    QPDFObjectHandle contents = page.getObjectHandle().getKey("/Contents");
    if (contents.isArray()) {
      for (auto& item : contents.getArrayAsVector()) {
        if (item.isStream()) streams.push_back(item);
      }
    } else if (contents.isStream()) {
      streams.push_back(contents);
    }
    std::vector<std::string> do_names;
    std::vector<ShowOpFit> page_fits;
    for (const auto& f : ops.fits) {
      if (f.container < 0) page_fits.push_back(f);
    }
    // 스트림을 개행으로 이어 붙여 한 번에 파싱한다(규격: 배열 스트림은 사이에 공백이 있는 것처럼
    // 읽는다). 치환은 스트림마다 자기 범위 조각에만 적용해 파일에는 스트림이 그대로 남는다 — 합쳐
    // 쓰면 PDFium 이 뒤 편집에서 원본까지 다시 쓴다
    std::string joined;
    std::vector<std::pair<std::size_t, std::size_t>> segments;  // 스트림 k 의 [from, to)
    for (QPDFObjectHandle& stream : streams) {
      if (!joined.empty()) joined += '\n';
      const std::string data = stream_bytes(stream);
      segments.emplace_back(joined.size(), joined.size() + data.size());
      joined += data;
    }
    RewriteResult r = rewrite_content(q, joined, ops.page_ordinals, 0, report.needs_displacement,
                                      &do_names, page_fits, &report.fitted, &report.already_fitted);
    if (r.dangling)
      return make_error(ErrorCode::kUnsupported, "content stream ends inside an operator");
    for (std::size_t k = 0; k < streams.size(); ++k) {
      bool changed = false;
      const std::string out =
          apply_edits(joined, r.edits, segments[k].first, segments[k].second, &changed);
      if (changed)
        streams[k].replaceStreamData(out, QPDFObjectHandle::newNull(), QPDFObjectHandle::newNull());
    }
    report.page_show_ops = r.show_ops;
    if (!ops.form_ordinals.empty()) {
      // /Resources 는 /Pages 트리에서 상속될 수 있다
      QPDFObjectHandle resources = page.getAttribute("/Resources", false);
      QPDFObjectHandle xobjects =
          resources.isDictionary() ? resources.getKey("/XObject") : QPDFObjectHandle::newNull();
      std::vector<QPDFObjectHandle> forms;  // 쪽의 Form Do 순서 (이미지 Do 제외)
      for (const std::string& name : do_names) {
        QPDFObjectHandle x =
            xobjects.isDictionary() ? xobjects.getKey(name) : QPDFObjectHandle::newNull();
        if (x.isStream() && x.getDict().getKey("/Subtype").isNameAndEquals("/Form"))
          forms.push_back(x);
      }
      for (const auto& [form_index, ordinals] : ops.form_ordinals) {
        if (form_index < 0 || form_index >= static_cast<int>(forms.size())) {
          return make_error(ErrorCode::kInvalidArgument, "form index out of range");
        }
        QPDFObjectHandle form = forms[static_cast<std::size_t>(form_index)];
        // 같은 Form 을 쓰는 "사용자"(쪽, 또는 자원을 가진 스트림 — 다른 Form·주석 외형·패턴)가 둘
        // 이상이거나 이 쪽이 두 번 그리면 수술이 그쪽까지 바꾼다 → 거절 (호출자). 쪽의 /Resources
        // 는 대개 직접 사전이라 객체 순회로는 안 보인다 → 쪽은 따로 센다
        const auto uses_form = [&](QPDFObjectHandle resources) {
          if (!resources.isDictionary()) return false;
          QPDFObjectHandle xo = resources.getKey("/XObject");
          if (!xo.isDictionary()) return false;
          for (const auto& [key, value] : xo.ditems()) {
            if (value.isIndirect() && form.isIndirect() && value.getObjGen() == form.getObjGen())
              return true;
          }
          return false;
        };
        int users = 0;
        for (QPDFPageObjectHelper& other : pages) {
          if (uses_form(other.getAttribute("/Resources", false))) ++users;
        }
        for (QPDFObjectHandle& object : q.getAllObjects()) {
          if (!object.isStream()) continue;  // Form·외형·패턴 스트림의 자원 (쪽은 위에서 셌다)
          if (uses_form(object.getDict().getKey("/Resources"))) ++users;
        }
        int drawn_here = 0;
        for (const QPDFObjectHandle& f : forms) {
          if (f.isIndirect() && form.isIndirect() && f.getObjGen() == form.getObjGen())
            ++drawn_here;
        }
        if (users > 1 || drawn_here > 1) ++report.shared_forms;
        const std::string form_data = stream_bytes(form);
        RewriteResult fr =
            rewrite_content(q, form_data, ordinals, 0, report.needs_displacement, nullptr);
        if (fr.dangling)
          return make_error(ErrorCode::kUnsupported, "content stream ends inside an operator");
        report.form_show_ops[form_index] = fr.show_ops;
        bool changed = false;
        const std::string out = apply_edits(form_data, fr.edits, 0, form_data.size(), &changed);
        if (changed)
          form.replaceStreamData(out, QPDFObjectHandle::newNull(), QPDFObjectHandle::newNull());
      }
    }
    QPDFWriter w(q);
    w.setOutputMemory();
    w.setPreserveEncryption(true);
    w.setObjectStreamMode(qpdf_o_preserve);
    w.write();
    auto result = w.getBufferSharedPointer();
    report.bytes.assign(result->getBuffer(), result->getBuffer() + result->getSize());
  } catch (const std::exception& e) {
    return make_error(ErrorCode::kJobFailed, std::string("qpdf: ") + e.what());
  }
  return report;
}

}  // namespace onpdf::engine
