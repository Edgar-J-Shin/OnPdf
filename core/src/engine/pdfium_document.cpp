#include "onpdf/core/engine/pdfium_document.h"

#include "form_env.h"

#include "onpdf/core/engine/pdfium_library.h"
#include "dest.h"
#include "onpdf/core/engine/text_page.h"
#include "text_lines.h"
#include "onpdf/core/engine/qpdf_tools.h"
#include "onpdf/core/platform/path.h"

#include <fpdf_annot.h>
#include <fpdf_doc.h>
#include <fpdf_signature.h>
#include <fpdf_save.h>
#include <fpdf_text.h>
#include <fpdf_edit.h>
#include <fpdf_ppo.h>
#include <fpdfview.h>

#include <algorithm>
#include <variant>
#include <cassert>
#include <charconv>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <random>
#include <string>
#include <unordered_set>

namespace onpdf::engine {

namespace {

Error last_pdfium_error(const std::string& context) {
  const unsigned long code = FPDF_GetLastError();
  ErrorCode mapped = ErrorCode::kUnknown;
  switch (code) {
    case FPDF_ERR_FILE: mapped = ErrorCode::kFileNotFound; break;
    case FPDF_ERR_FORMAT: mapped = ErrorCode::kInvalidFormat; break;
    case FPDF_ERR_PASSWORD: mapped = ErrorCode::kPasswordRequired; break;
    case FPDF_ERR_SECURITY: mapped = ErrorCode::kSecurityError; break;
    case FPDF_ERR_PAGE: mapped = ErrorCode::kPageNotFound; break;
    default: break;
  }
  return Error{mapped, context + " (FPDF_GetLastError=" + std::to_string(code) + ")"};
}

Result<std::vector<std::uint8_t>> read_file(const std::filesystem::path& path) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec)) {
    return make_error(ErrorCode::kFileNotFound, "file not found: " + path_to_utf8(path));
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return make_error(ErrorCode::kIoError, "cannot open: " + path_to_utf8(path));
  }
  std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
  if (in.bad()) {
    return make_error(ErrorCode::kIoError, "read failed: " + path_to_utf8(path));
  }
  return data;
}

// 메모리의 PDF 바이트를 연다. data 는 돌려준 문서보다 오래 살아야 한다
Result<ScopedDocument> load_document(const std::vector<std::uint8_t>& data, std::string_view password) {
  if (data.empty()) return make_error(ErrorCode::kInvalidFormat, "empty document");
  const std::string pw(password);
  ScopedDocument doc(FPDF_LoadMemDocument64(data.data(), data.size(), pw.empty() ? nullptr : pw.c_str()));
  if (!doc) return std::unexpected(last_pdfium_error("FPDF_LoadMemDocument64 failed"));
  return doc;
}

// 쪽 번호 목록이 오름차순·중복 없음·범위 안인지
Result<void> check_page_list(const std::vector<int>& pages, int count, const char* what) {
  if (pages.empty()) return make_error(ErrorCode::kInvalidArgument, std::string(what) + ": no pages");
  for (std::size_t i = 0; i < pages.size(); ++i) {
    if (pages[i] < 0 || pages[i] >= count) {
      return make_error(ErrorCode::kInvalidArgument, std::string(what) + ": page index out of range: " + std::to_string(pages[i]));
    }
    if (i > 0 && pages[i] <= pages[i - 1]) {
      return make_error(ErrorCode::kInvalidArgument, std::string(what) + ": pages must be ascending and unique");
    }
  }
  return {};
}

}  // namespace

PdfiumDocument::PdfiumDocument(std::vector<std::uint8_t> data, ScopedDocument doc)
    : data_(std::move(data)), doc_(std::move(doc)) {
  switch (FPDF_GetFormType(doc_.get())) {
    case FORMTYPE_ACRO_FORM: form_type_ = FormType::kAcroForm; break;
    case FORMTYPE_XFA_FULL: form_type_ = FormType::kXfaFull; break;
    case FORMTYPE_XFA_FOREGROUND: form_type_ = FormType::kXfaForeground; break;
    default: form_type_ = FormType::kNone; break;
  }
  form_ = detail::FormEnvironment::create(doc_.get());
}

PdfiumDocument::PdfiumDocument(PdfiumDocument&&) noexcept = default;

PdfiumDocument& PdfiumDocument::operator=(PdfiumDocument&& other) noexcept {
  if (this == &other) return *this;
  // 정리 순서: 폼 환경 → 문서에 딸린 객체 → 문서 → 바이트 (문서가 data_ 를 참조한다)
  form_.reset();
  detached_.clear();
  fonts_.clear();
  snapshots_.clear();
  states_.clear();
  doc_.reset();
  data_ = std::move(other.data_);
  doc_ = std::move(other.doc_);
  password_ = std::move(other.password_);
  snapshots_ = std::move(other.snapshots_);
  next_snapshot_ = other.next_snapshot_;
  fonts_ = std::move(other.fonts_);
  cff_keys_ = std::move(other.cff_keys_);
  detached_ = std::move(other.detached_);
  next_detached_ = other.next_detached_;
  form_type_ = other.form_type_;
  form_ = std::move(other.form_);
  field_pages_ = std::move(other.field_pages_);
  field_pages_ready_ = other.field_pages_ready_;
  states_ = std::move(other.states_);
  state_order_ = std::move(other.state_order_);
  state_bytes_ = other.state_bytes_;
  state_byte_limit_ = other.state_byte_limit_;
  next_state_ = other.next_state_;
  dropped_marks_ = std::move(other.dropped_marks_);
  return *this;
}

PdfiumDocument::~PdfiumDocument() = default;

FPDF_FORMHANDLE PdfiumDocument::form_handle() const noexcept { return form_ ? form_->handle.get() : nullptr; }

void PdfiumDocument::reset_form() {
  field_pages_.clear();
  field_pages_ready_ = false;
  if (!form_) return;
  form_.reset();  // 필드 목록·페이지 뷰가 이전 쪽 구조를 가리키지 않게 새로 만든다
  form_ = detail::FormEnvironment::create(doc_.get());
}

void PdfiumDocument::draw_form(FPDF_BITMAP bitmap, FPDF_PAGE page, int start_x, int start_y, int size_x, int size_y,
                               int rotate, int flags) const {
  FPDF_FORMHANDLE form = form_handle();
  if (form == nullptr || !detail::page_has_widgets(page)) return;
  detail::FormPageView view(page, form);
  FPDF_FFLDraw(form, bitmap, page, start_x, start_y, size_x, size_y, rotate, flags);
}

Result<PdfiumDocument> PdfiumDocument::open(const std::filesystem::path& path,
                                            std::string_view password) {
  auto data = read_file(path);
  if (!data) {
    return std::unexpected(data.error());
  }
  return open_from_memory(std::move(*data), password);
}

Result<PdfiumDocument> PdfiumDocument::open_from_memory(std::vector<std::uint8_t> data,
                                                        std::string_view password) {
  PdfiumLibrary::assert_current_thread();
  auto doc = load_document(data, password);
  if (!doc) return std::unexpected(doc.error());
  PdfiumDocument result(std::move(data), std::move(*doc));
  result.password_ = std::string(password);
  return result;
}

int PdfiumDocument::page_count() const noexcept {
  PdfiumLibrary::assert_current_thread();
  return FPDF_GetPageCount(doc_.get());
}

Result<int> PdfiumDocument::page_rotation(int page_index) const {
  PdfiumLibrary::assert_current_thread();
  auto page = load_page(page_index);
  if (!page) return std::unexpected(page.error());
  const int rotation = FPDFPage_GetRotation(page->get());
  if (rotation < 0 || rotation > 3) return make_error(ErrorCode::kInvalidFormat, "invalid page rotation");
  return rotation;
}

Result<void> PdfiumDocument::set_page_rotation(int page_index, int quarter_turns) {
  PdfiumLibrary::assert_current_thread();
  if (quarter_turns < 0 || quarter_turns > 3) {
    return make_error(ErrorCode::kInvalidArgument, "rotation must be 0..3 quarter turns");
  }
  auto page = load_page(page_index);
  if (!page) return std::unexpected(page.error());
  FPDFPage_SetRotation(page->get(), quarter_turns);  // 쪽 사전의 /Rotate 에 기록되어 쪽을 닫아도 유지된다
  if (FPDFPage_GetRotation(page->get()) != quarter_turns) {
    return make_error(ErrorCode::kInvalidState, "FPDFPage_SetRotation did not apply");
  }
  return {};
}

// ---------------------------------------------------------------- 구조 편집

Result<void> PdfiumDocument::delete_pages(const std::vector<int>& pages) {
  PdfiumLibrary::assert_current_thread();
  const int count = page_count();
  if (auto ok = check_page_list(pages, count, "delete"); !ok) return ok;
  const int n = static_cast<int>(pages.size());
  if (count - n < 1) return make_error(ErrorCode::kInvalidArgument, "cannot delete all pages");
  // 내림차순으로 지워야 앞쪽 번호가 밀리지 않는다. FPDFPage_Delete 는 반환값이 없어 쪽 수로 확인한다
  for (auto it = pages.rbegin(); it != pages.rend(); ++it) FPDFPage_Delete(doc_.get(), *it);
  if (page_count() != count - n) return make_error(ErrorCode::kInvalidState, "page count mismatch after delete");
  reset_form();
  std::vector<int> new_index(static_cast<std::size_t>(count), -1);
  for (int old = 0, next = 0; old < count; ++old) {
    if (!std::binary_search(pages.begin(), pages.end(), old)) new_index[static_cast<std::size_t>(old)] = next++;
  }
  remap_detached_pages(new_index);
  return {};
}

Result<void> PdfiumDocument::move_pages(const std::vector<int>& pages, int dest) {
  PdfiumLibrary::assert_current_thread();
  const int count = page_count();
  if (pages.empty()) return make_error(ErrorCode::kInvalidArgument, "move: no pages");
  std::vector<int> sorted(pages);
  std::sort(sorted.begin(), sorted.end());
  if (auto ok = check_page_list(sorted, count, "move"); !ok) return ok;  // 범위·중복 (순서는 블록 안 순서라 자유)
  const int n = static_cast<int>(pages.size());
  if (dest < 0 || dest > count - n) {
    return make_error(ErrorCode::kInvalidArgument, "move: destination out of range: " + std::to_string(dest));
  }
  // 여기까지 검증했으므로 실패하면 PDFium 내부 문제다 (실패 시 문서가 불확정일 수 있다고 문서화되어 있다)
  if (!FPDF_MovePages(doc_.get(), pages.data(), static_cast<unsigned long>(pages.size()), dest)) {
    return make_error(ErrorCode::kInvalidState, "FPDF_MovePages failed");
  }
  reset_form();
  // 새 순서 = 나머지 쪽(오름차순)에 블록(주어진 순서)을 dest 에 끼운 것
  std::vector<int> order;
  for (int old = 0; old < count; ++old) {
    if (!std::binary_search(sorted.begin(), sorted.end(), old)) order.push_back(old);
  }
  order.insert(order.begin() + dest, pages.begin(), pages.end());
  std::vector<int> new_index(static_cast<std::size_t>(count), -1);
  for (int pos = 0; pos < count; ++pos) new_index[static_cast<std::size_t>(order[static_cast<std::size_t>(pos)])] = pos;
  remap_detached_pages(new_index);
  return {};
}

Result<SnapshotId> PdfiumDocument::copy_pages_to_snapshot(const std::vector<int>& pages) {
  PdfiumLibrary::assert_current_thread();
  if (auto ok = check_page_list(pages, page_count(), "snapshot"); !ok) return std::unexpected(ok.error());
  ScopedDocument snapshot(FPDF_CreateNewDocument());
  if (!snapshot) return make_error(ErrorCode::kIoError, "FPDF_CreateNewDocument failed");
  if (!FPDF_ImportPagesByIndex(snapshot.get(), doc_.get(), pages.data(), static_cast<unsigned long>(pages.size()), 0)) {
    return make_error(ErrorCode::kInvalidState, "cannot copy pages to snapshot");
  }
  if (FPDF_GetPageCount(snapshot.get()) != static_cast<int>(pages.size())) {
    return make_error(ErrorCode::kInvalidState, "snapshot page count mismatch");
  }
  return store_snapshot({}, std::move(snapshot));
}

Result<SnapshotId> PdfiumDocument::open_snapshot(const std::filesystem::path& file, std::string_view password) {
  PdfiumLibrary::assert_current_thread();
  auto data = read_file(file);
  if (!data) return std::unexpected(data.error());
  auto doc = load_document(*data, password);
  if (!doc) return std::unexpected(doc.error());
  return store_snapshot(std::move(*data), std::move(*doc));
}

Result<SnapshotId> PdfiumDocument::store_snapshot(std::vector<std::uint8_t> data, ScopedDocument doc) {
  const SnapshotId id = next_snapshot_++;
  snapshots_.emplace(id, Snapshot{std::move(data), std::move(doc)});
  return id;
}

Result<SnapshotInfo> PdfiumDocument::snapshot_info(SnapshotId id) const {
  PdfiumLibrary::assert_current_thread();
  const auto it = snapshots_.find(id);
  if (it == snapshots_.end()) return make_error(ErrorCode::kInvalidArgument, "unknown snapshot");
  SnapshotInfo info;
  info.page_count = std::max(0, FPDF_GetPageCount(it->second.doc.get()));
  info.permissions = permissions_of(it->second.doc.get());
  return info;
}

Result<void> PdfiumDocument::insert_from_snapshot(SnapshotId id, const std::vector<int>& src_pages, int at) {
  PdfiumLibrary::assert_current_thread();
  const auto it = snapshots_.find(id);
  if (it == snapshots_.end()) return make_error(ErrorCode::kInvalidArgument, "unknown snapshot");
  FPDF_DOCUMENT source = it->second.doc.get();
  const int source_count = std::max(0, FPDF_GetPageCount(source));
  const int count = page_count();
  if (at < 0 || at > count) return make_error(ErrorCode::kInvalidArgument, "insert: position out of range: " + std::to_string(at));
  int n = source_count;
  if (!src_pages.empty()) {
    std::vector<int> sorted(src_pages);
    std::sort(sorted.begin(), sorted.end());
    if (auto ok = check_page_list(sorted, source_count, "insert"); !ok) return ok;
    n = static_cast<int>(src_pages.size());
  }
  if (n == 0) return make_error(ErrorCode::kInvalidArgument, "insert: source has no pages");
  const FPDF_BOOL ok = src_pages.empty()
                           ? FPDF_ImportPagesByIndex(doc_.get(), source, nullptr, 0, at)
                           : FPDF_ImportPagesByIndex(doc_.get(), source, src_pages.data(),
                                                     static_cast<unsigned long>(src_pages.size()), at);
  if (!ok) return make_error(ErrorCode::kInvalidState, "FPDF_ImportPagesByIndex failed");
  if (page_count() != count + n) return make_error(ErrorCode::kInvalidState, "page count mismatch after insert");
  reset_form();
  std::vector<int> new_index(static_cast<std::size_t>(count));
  for (int old = 0; old < count; ++old) new_index[static_cast<std::size_t>(old)] = old < at ? old : old + n;
  remap_detached_pages(new_index);
  return {};
}

void PdfiumDocument::drop_snapshot(SnapshotId id) noexcept {
  PdfiumLibrary::assert_current_thread();
  snapshots_.erase(id);
}

// ---------------------------------------------------------------- 주석

namespace {

FPDF_ANNOTATION_SUBTYPE subtype_of(AnnotationKind kind) {
  switch (kind) {
    case AnnotationKind::kHighlight: return FPDF_ANNOT_HIGHLIGHT;
    case AnnotationKind::kUnderline: return FPDF_ANNOT_UNDERLINE;
    case AnnotationKind::kStrikeOut: return FPDF_ANNOT_STRIKEOUT;
    case AnnotationKind::kSquiggly: return FPDF_ANNOT_SQUIGGLY;
    case AnnotationKind::kText: return FPDF_ANNOT_TEXT;
    case AnnotationKind::kInk: return FPDF_ANNOT_INK;
    case AnnotationKind::kSquare: return FPDF_ANNOT_SQUARE;
    case AnnotationKind::kCircle: return FPDF_ANNOT_CIRCLE;
    case AnnotationKind::kLink: return FPDF_ANNOT_LINK;
    case AnnotationKind::kOther: break;
  }
  return FPDF_ANNOT_UNKNOWN;
}

AnnotationKind kind_of(FPDF_ANNOTATION_SUBTYPE subtype) {
  switch (subtype) {
    case FPDF_ANNOT_HIGHLIGHT: return AnnotationKind::kHighlight;
    case FPDF_ANNOT_UNDERLINE: return AnnotationKind::kUnderline;
    case FPDF_ANNOT_STRIKEOUT: return AnnotationKind::kStrikeOut;
    case FPDF_ANNOT_SQUIGGLY: return AnnotationKind::kSquiggly;
    case FPDF_ANNOT_TEXT: return AnnotationKind::kText;
    case FPDF_ANNOT_INK: return AnnotationKind::kInk;
    case FPDF_ANNOT_SQUARE: return AnnotationKind::kSquare;
    case FPDF_ANNOT_CIRCLE: return AnnotationKind::kCircle;
    case FPDF_ANNOT_LINK: return AnnotationKind::kLink;
    default: return AnnotationKind::kOther;
  }
}

bool is_markup(AnnotationKind kind) {
  return kind == AnnotationKind::kHighlight || kind == AnnotationKind::kUnderline ||
         kind == AnnotationKind::kStrikeOut || kind == AnnotationKind::kSquiggly;
}

bool is_editable_kind(AnnotationKind kind) {
  return is_markup(kind) || kind == AnnotationKind::kText || kind == AnnotationKind::kInk ||
         kind == AnnotationKind::kSquare || kind == AnnotationKind::kCircle;
}

// 문자열 키 읽기 (UTF-16LE → UTF-8). 없으면 빈 문자열
std::string annot_string(FPDF_ANNOTATION annot, const char* key) {
  const unsigned long bytes = FPDFAnnot_GetStringValue(annot, key, nullptr, 0);
  if (bytes <= 2) return {};
  std::vector<unsigned char> buf(bytes);
  FPDFAnnot_GetStringValue(annot, key, reinterpret_cast<FPDF_WCHAR*>(buf.data()), bytes);
  return detail::utf16le_to_utf8(buf.data(), buf.size());
}

bool set_annot_string(FPDF_ANNOTATION annot, const char* key, const std::string& utf8) {
  const std::vector<unsigned short> wide = detail::to_wide(utf8);
  return FPDFAnnot_SetStringValue(annot, key, wide.data()) != 0;
}

// PDF 날짜 문자열 (UTC, "D:YYYYMMDDHHmmSSZ"). std::gmtime 은 재진입 불가지만 워커 스레드에서만 부른다
std::string pdf_date_now() {
  const std::time_t now = std::time(nullptr);
  const std::tm* utc = std::gmtime(&now);
  if (utc == nullptr) return {};
  char buf[32];
  std::strftime(buf, sizeof buf, "D:%Y%m%d%H%M%SZ", utc);
  return buf;
}

std::string new_text_id() { return make_text_object_id(); }
std::string new_box_id() { return make_text_box_id(); }

// 쪽 객체의 OnPdf 마크 (없으면 nullptr)
FPDF_PAGEOBJECTMARK onpdf_mark_of(FPDF_PAGEOBJECT obj) {
  const int marks = FPDFPageObj_CountMarks(obj);
  for (int m = 0; m < marks; ++m) {
    FPDF_PAGEOBJECTMARK mark = FPDFPageObj_GetMark(obj, static_cast<unsigned long>(m));
    if (mark == nullptr) continue;
    unsigned char name[32] = {};  // UTF-16LE
    unsigned long name_len = 0;
    if (!FPDFPageObjMark_GetName(mark, reinterpret_cast<FPDF_WCHAR*>(name), sizeof name, &name_len) || name_len > sizeof name) continue;
    if (detail::utf16le_to_utf8(name, name_len) == "OnPdf") return mark;
  }
  return nullptr;
}

// 마크의 문자열 파라미터 (ASCII 전용: id·box). 값은 UTF-16LE 로 돌아온다
std::string mark_string(FPDF_PAGEOBJECTMARK mark, const char* key) {
  unsigned long bytes = 0;
  if (mark == nullptr || !FPDFPageObjMark_GetParamStringValue(mark, key, nullptr, 0, &bytes) || bytes == 0) return {};
  std::vector<unsigned char> buffer(bytes);
  if (!FPDFPageObjMark_GetParamStringValue(mark, key, reinterpret_cast<FPDF_WCHAR*>(buffer.data()), bytes, &bytes)) return {};
  return detail::utf16le_to_utf8(buffer.data(), bytes);
}

// 마크의 블롭 파라미터 (원문 UTF-8 — 문자열 파라미터는 비ASCII 를 PDFDocEncoding 으로 오해하고 UTF-16 은 NUL 에서 잘린다)
std::string mark_blob(FPDF_PAGEOBJECTMARK mark, const char* key) {
  unsigned long bytes = 0;
  if (mark == nullptr || !FPDFPageObjMark_GetParamBlobValue(mark, key, nullptr, 0, &bytes) || bytes == 0) return {};
  std::string out(bytes, '\0');
  if (!FPDFPageObjMark_GetParamBlobValue(mark, key, reinterpret_cast<unsigned char*>(out.data()), bytes, &bytes)) return {};
  out.resize(bytes);
  return out;
}

// 쪽 객체의 OnPdf 마크 id (없으면 빈 문자열)
std::string text_object_id_of(FPDF_PAGEOBJECT obj) { return mark_string(onpdf_mark_of(obj), "id"); }

constexpr std::string_view kObjIndexPrefix = "#obj";

// 마크 id 또는 "#obj<쪽 객체 index>" 로 객체를 찾는다 (#49). 못 찾으면 {nullptr, -1}
std::pair<FPDF_PAGEOBJECT, int> find_page_object(FPDF_PAGE page, std::string_view id) {
  const int count = FPDFPage_CountObjects(page);
  if (id.starts_with(kObjIndexPrefix)) {
    int index = -1;
    const auto digits = id.substr(kObjIndexPrefix.size());
    const auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), index);
    if (ec != std::errc{} || ptr != digits.data() + digits.size() || index < 0 || index >= count) return {nullptr, -1};
    return {FPDFPage_GetObject(page, index), index};
  }
  for (int i = count - 1; i >= 0; --i) {
    FPDF_PAGEOBJECT obj = FPDFPage_GetObject(page, i);
    if (obj != nullptr && text_object_id_of(obj) == id) return {obj, i};
  }
  return {nullptr, -1};
}

// 쪽 포인트 사각형 → 이미지 행렬 (이미지 = 사용자 좌표 단위 정사각형). 기준점은 사각형의 왼쪽 아래(쪽 포인트)
FS_MATRIX image_matrix_for(const detail::PageSpace& space, const Rect& r) {
  const auto m = space.user_matrix_at({r.x, r.bottom()});
  return {static_cast<float>(m[0] * r.w), static_cast<float>(m[1] * r.w), static_cast<float>(m[2] * r.h),
          static_cast<float>(m[3] * r.h), static_cast<float>(m[4]), static_cast<float>(m[5])};
}

// 메모리 바이트를 FPDF_FILEACCESS 로 (JPEG 삽입). LoadJpegFileInline 이 내용을 복사하므로 호출 동안만 살면 된다
struct MemoryFileAccess {
  FPDF_FILEACCESS access{};
  const std::vector<std::uint8_t>* data = nullptr;
  explicit MemoryFileAccess(const std::vector<std::uint8_t>& bytes) : data(&bytes) {
    access.m_FileLen = static_cast<unsigned long>(bytes.size());
    access.m_Param = this;
    access.m_GetBlock = [](void* param, unsigned long pos, unsigned char* buf, unsigned long size) -> int {
      const auto* self = static_cast<MemoryFileAccess*>(param);
      if (pos > self->data->size() || size > self->data->size() - pos) return 0;
      std::memcpy(buf, self->data->data() + pos, size);
      return 1;
    };
  }
};

// 마크의 포인트 값 (폭): 로캘과 무관하게 "123.456" 으로 쓰고 읽는다 — printf/strtod 는 LC_NUMERIC 을 따른다 (리뷰 W1)
std::string format_points(double value) {
  const long long milli = std::llround(std::max(0.0, value) * 1000.0);
  char buf[32];
  std::snprintf(buf, sizeof buf, "%lld.%03lld", milli / 1000, milli % 1000);
  return buf;
}

double parse_points(std::string_view text) {
  const std::size_t dot = text.find('.');
  long long whole = 0;
  const std::string_view int_part = text.substr(0, dot);
  if (auto [ptr, ec] = std::from_chars(int_part.data(), int_part.data() + int_part.size(), whole);
      ec != std::errc{} || ptr != int_part.data() + int_part.size() || whole < 0) {
    return 0.0;
  }
  double value = static_cast<double>(whole);
  if (dot != std::string_view::npos) {
    double scale = 0.1;
    for (char c : text.substr(dot + 1)) {
      if (c < '0' || c > '9') return 0.0;
      value += (c - '0') * scale;
      scale /= 10.0;
    }
  }
  return value;
}

std::string image_filter_of(FPDF_PAGEOBJECT obj) {
  if (FPDFImageObj_GetImageFilterCount(obj) <= 0) return {};
  const unsigned long len = FPDFImageObj_GetImageFilter(obj, 0, nullptr, 0);
  if (len <= 1) return {};
  std::string name(len, '\0');
  FPDFImageObj_GetImageFilter(obj, 0, name.data(), len);
  name.resize(len - 1);  // 끝 NUL
  return name;
}

std::string new_nm() {
  std::random_device rd;
  const std::uint64_t v = (static_cast<std::uint64_t>(rd()) << 32) ^ rd();
  char buf[32];
  std::snprintf(buf, sizeof buf, "onpdf-%016llx", static_cast<unsigned long long>(v));
  return buf;
}

struct FoundAnnot {
  ScopedAnnotation annot;
  int index = -1;
};

// id 로 주석 찾기: "#<index>" 또는 /NM
Result<FoundAnnot> find_annot(FPDF_PAGE page, std::string_view id) {
  const int count = FPDFPage_GetAnnotCount(page);
  if (!id.empty() && id.front() == '#') {
    int index = -1;
    const auto digits = id.substr(1);
    if (std::from_chars(digits.data(), digits.data() + digits.size(), index).ec == std::errc{} && index >= 0 &&
        index < count) {
      ScopedAnnotation annot(FPDFPage_GetAnnot(page, index));
      if (annot) return FoundAnnot{std::move(annot), index};
    }
    return make_error(ErrorCode::kInvalidArgument, "annotation not found: " + std::string(id));
  }
  for (int i = 0; i < count; ++i) {
    ScopedAnnotation annot(FPDFPage_GetAnnot(page, i));
    if (annot && annot_string(annot.get(), "NM") == id) return FoundAnnot{std::move(annot), i};
  }
  return make_error(ErrorCode::kInvalidArgument, "annotation not found: " + std::string(id));
}

// NM 이 없으면 부여한다. 돌려주는 값이 그 뒤의 id
Result<std::string> ensure_nm(FPDF_ANNOTATION annot) {
  std::string nm = annot_string(annot, "NM");
  if (!nm.empty()) return nm;
  nm = new_nm();
  if (!set_annot_string(annot, "NM", nm)) return make_error(ErrorCode::kInvalidState, "cannot set /NM");
  return nm;
}

bool fill_color(FPDF_ANNOTATION annot, Color& color, FPDFANNOT_COLORTYPE type = FPDFANNOT_COLORTYPE_Color) {
  unsigned r = 0, g = 0, b = 0, a = 255;
  if (!FPDFAnnot_GetColor(annot, type, &r, &g, &b, &a)) return false;
  color = {static_cast<std::uint8_t>(r), static_cast<std::uint8_t>(g), static_cast<std::uint8_t>(b),
           static_cast<std::uint8_t>(a)};
  return true;
}

}  // namespace

Result<std::vector<AnnotationInfo>> PdfiumDocument::annotations(int page_index) const {
  PdfiumLibrary::assert_current_thread();
  auto page = load_page(page_index);
  if (!page) return std::unexpected(page.error());
  const detail::PageSpace space = detail::PageSpace::of(page->get());
  std::vector<AnnotationInfo> out;
  const int count = std::max(0, FPDFPage_GetAnnotCount(page->get()));
  out.reserve(static_cast<std::size_t>(count));
  for (int i = 0; i < count; ++i) {
    ScopedAnnotation annot(FPDFPage_GetAnnot(page->get(), i));
    if (!annot) continue;
    AnnotationInfo info;
    info.kind = kind_of(FPDFAnnot_GetSubtype(annot.get()));
    info.id = annot_string(annot.get(), "NM");
    if (info.id.empty()) info.id = "#" + std::to_string(i);
    FS_RECTF rect{};
    if (FPDFAnnot_GetRect(annot.get(), &rect)) info.rect = space.to_rect(rect.left, rect.right, rect.bottom, rect.top);
    info.has_color = FPDFAnnot_HasKey(annot.get(), "C") && fill_color(annot.get(), info.color);
    info.contents = annot_string(annot.get(), "Contents");
    info.author = annot_string(annot.get(), "T");
    info.modified = annot_string(annot.get(), "M");
    info.flags = FPDFAnnot_GetFlags(annot.get());
    info.editable = is_editable_kind(info.kind);
    float hr = 0, vr = 0, bw = 0;
    info.has_border = FPDFAnnot_GetBorder(annot.get(), &hr, &vr, &bw) != 0;
    info.border_width = info.has_border ? bw : 0.0f;
    float ca = 1.0f;
    if (FPDFAnnot_GetNumberValue(annot.get(), "CA", &ca)) info.opacity = std::clamp(ca, 0.0f, 1.0f);
    info.has_bs = FPDFAnnot_HasKey(annot.get(), "BS") != 0;
    info.has_appearance = FPDFAnnot_GetAP(annot.get(), FPDF_ANNOT_APPEARANCEMODE_NORMAL, nullptr, 0) > 2;  // 빈 문자열 = NUL 2바이트
    out.push_back(std::move(info));
  }
  return out;
}

Result<int> PdfiumDocument::ensure_annotation_appearances() const {
  // 새 주석은 AP 없이 만들고, 색·불투명도를 바꾸면 AP 를 지운다: PDFium 은 쪽을 렌더할 때 AP 를 만들어 사전에 넣는다.
  // 렌더되지 않은 채 저장하면 AP 가 없어 다른 뷰어가 제멋대로 그린다(미리보기: /CA 무시) — 저장 전에 그 쪽만 렌더한다 (#53).
  // OnPdf 가 만든·고친 주석(/NM onpdf-…)만 본다: 다른 프로그램 주석의 AP 를 새로 만들면 서명 문서의 증분 저장에 섞인다
  int rendered = 0;
  for (int i = 0; i < page_count(); ++i) {
    auto page = load_page(i);
    if (!page) return std::unexpected(page.error());
    bool needs = false;
    const int count = std::max(0, FPDFPage_GetAnnotCount(page->get()));
    for (int a = 0; a < count && !needs; ++a) {
      ScopedAnnotation annot(FPDFPage_GetAnnot(page->get(), a));
      if (!annot || !is_editable_kind(kind_of(FPDFAnnot_GetSubtype(annot.get())))) continue;
      if (annot_string(annot.get(), "NM").rfind("onpdf-", 0) != 0) continue;
      needs = FPDFAnnot_GetAP(annot.get(), FPDF_ANNOT_APPEARANCEMODE_NORMAL, nullptr, 0) <= 2;
    }
    if (!needs) continue;
    // 주석 목록을 만드는 렌더가 AP 를 생성하고, /Annots 의 직접 사전을 간접 객체로 바꾼다(CPDF_AnnotList) —
    // FPDFPage_CreateAnnot 은 직접 사전을 넣으므로 이 렌더가 규격(표 30: 간접 참조) 위반도 함께 고친다 (크기 무관: 1×1)
    ScopedBitmap bitmap(FPDFBitmap_Create(1, 1, 1));
    if (!bitmap) return make_error(ErrorCode::kRenderFailed, "FPDFBitmap_Create failed");
    FPDF_RenderPageBitmap(bitmap.get(), page->get(), 0, 0, 1, 1, 0, FPDF_ANNOT);
    ++rendered;
  }
  return rendered;
}

Result<std::string> PdfiumDocument::add_annotation(int page_index, const AnnotationSpec& spec) {
  PdfiumLibrary::assert_current_thread();
  if (!is_editable_kind(spec.kind)) return make_error(ErrorCode::kInvalidArgument, "unsupported annotation kind");
  if (is_markup(spec.kind) && spec.quads.empty()) return make_error(ErrorCode::kInvalidArgument, "markup needs quads");
  if (spec.kind == AnnotationKind::kInk && spec.ink.empty()) return make_error(ErrorCode::kInvalidArgument, "ink needs strokes");
  const bool is_shape = spec.kind == AnnotationKind::kSquare || spec.kind == AnnotationKind::kCircle;
  if (is_shape && spec.rect.empty()) return make_error(ErrorCode::kInvalidArgument, "shape needs rect");
  auto page = load_page(page_index);
  if (!page) return std::unexpected(page.error());
  const detail::PageSpace space = detail::PageSpace::of(page->get());
  ScopedAnnotation annot(FPDFPage_CreateAnnot(page->get(), subtype_of(spec.kind)));
  if (!annot) return make_error(ErrorCode::kInvalidState, "FPDFPage_CreateAnnot failed");
  FS_RECTF rect{};
  if (is_markup(spec.kind)) {
    bool first = true;
    for (const Rect& quad_pt : spec.quads) {
      const std::array<float, 8> q = space.quad_of(quad_pt);
      FS_QUADPOINTSF quad{q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7]};
      if (!FPDFAnnot_AppendAttachmentPoints(annot.get(), &quad)) {
        return make_error(ErrorCode::kInvalidState, "FPDFAnnot_AppendAttachmentPoints failed");
      }
      const auto u = space.user_rect_of(quad_pt);
      if (first) {
        rect = {static_cast<float>(u[0]), static_cast<float>(u[3]), static_cast<float>(u[2]), static_cast<float>(u[1])};
        first = false;
      } else {
        rect.left = std::min(rect.left, static_cast<float>(u[0]));
        rect.bottom = std::min(rect.bottom, static_cast<float>(u[1]));
        rect.right = std::max(rect.right, static_cast<float>(u[2]));
        rect.top = std::max(rect.top, static_cast<float>(u[3]));
      }
    }
  } else {
    const auto u = space.user_rect_of(spec.rect);
    rect = {static_cast<float>(u[0]), static_cast<float>(u[3]), static_cast<float>(u[2]), static_cast<float>(u[1])};
  }
  if (spec.kind == AnnotationKind::kInk) {
    for (const auto& stroke : spec.ink) {
      std::vector<FS_POINTF> pts;
      pts.reserve(stroke.size());
      for (const Point& p : stroke) {
        const Point u = space.to_user(p);
        pts.push_back({static_cast<float>(u.x), static_cast<float>(u.y)});
      }
      if (pts.empty() || FPDFAnnot_AddInkStroke(annot.get(), pts.data(), pts.size()) < 0) {
        return make_error(ErrorCode::kInvalidState, "FPDFAnnot_AddInkStroke failed");
      }
    }
  }
  // 선 폭: 펜·도형. /Border 로 쓴다 (PDFium 이 AP 를 만들 때 /BS /W → /Border 순으로 읽는다)
  if ((spec.kind == AnnotationKind::kInk || is_shape) && spec.border_width > 0 &&
      !FPDFAnnot_SetBorder(annot.get(), 0, 0, static_cast<float>(spec.border_width))) {
    return make_error(ErrorCode::kInvalidState, "FPDFAnnot_SetBorder failed");
  }
  // 내부색의 알파 인자도 /CA 에 쓰이므로 테두리 색의 알파를 그대로 준다 (불투명도는 주석당 하나)
  if (is_shape && spec.interior &&
      !FPDFAnnot_SetColor(annot.get(), FPDFANNOT_COLORTYPE_InteriorColor, spec.interior->r, spec.interior->g,
                          spec.interior->b, spec.color.a)) {
    return make_error(ErrorCode::kInvalidState, "FPDFAnnot_SetColor(interior) failed");
  }
  if (!FPDFAnnot_SetRect(annot.get(), &rect)) return make_error(ErrorCode::kInvalidState, "FPDFAnnot_SetRect failed");
  if (!FPDFAnnot_SetColor(annot.get(), FPDFANNOT_COLORTYPE_Color, spec.color.r, spec.color.g, spec.color.b, spec.color.a)) {
    return make_error(ErrorCode::kInvalidState, "FPDFAnnot_SetColor failed");
  }
  if (!FPDFAnnot_SetFlags(annot.get(), spec.flags)) return make_error(ErrorCode::kInvalidState, "FPDFAnnot_SetFlags failed");
  const std::string nm = spec.id.empty() ? new_nm() : spec.id;
  if (!set_annot_string(annot.get(), "NM", nm)) return make_error(ErrorCode::kInvalidState, "cannot set /NM");
  if (!spec.contents.empty() && !set_annot_string(annot.get(), "Contents", spec.contents)) {
    return make_error(ErrorCode::kInvalidState, "cannot set /Contents");
  }
  if (!spec.author.empty()) (void)set_annot_string(annot.get(), "T", spec.author);
  (void)set_annot_string(annot.get(), "M", pdf_date_now());
  return nm;
}

Result<AnnotationSnapshot> PdfiumDocument::snapshot_annotation(int page_index, std::string_view id) {
  PdfiumLibrary::assert_current_thread();
  auto page = load_page(page_index);
  if (!page) return std::unexpected(page.error());
  auto found = find_annot(page->get(), id);
  if (!found) return std::unexpected(found.error());
  FPDF_ANNOTATION annot = found->annot.get();
  auto nm = ensure_nm(annot);
  if (!nm) return std::unexpected(nm.error());
  AnnotationSnapshot snap;
  snap.kind = kind_of(FPDFAnnot_GetSubtype(annot));
  snap.id = *nm;
  FS_RECTF rect{};
  if (FPDFAnnot_GetRect(annot, &rect)) snap.rect = {rect.left, rect.bottom, rect.right, rect.top};
  const std::size_t quads = FPDFAnnot_CountAttachmentPoints(annot);
  for (std::size_t i = 0; i < quads; ++i) {
    FS_QUADPOINTSF q{};
    if (FPDFAnnot_GetAttachmentPoints(annot, i, &q)) snap.quads.push_back({q.x1, q.y1, q.x2, q.y2, q.x3, q.y3, q.x4, q.y4});
  }
  const unsigned long strokes = FPDFAnnot_GetInkListCount(annot);
  for (unsigned long s = 0; s < strokes; ++s) {
    const unsigned long n = FPDFAnnot_GetInkListPath(annot, s, nullptr, 0);
    std::vector<FS_POINTF> pts(n);
    if (n > 0) FPDFAnnot_GetInkListPath(annot, s, pts.data(), n);
    std::vector<Point> stroke;
    stroke.reserve(pts.size());
    for (const auto& p : pts) stroke.push_back({p.x, p.y});
    snap.ink.push_back(std::move(stroke));
  }
  // AP 가 있으면 색을 읽지 못한다: 스냅샷은 삭제·색 변경 직전에만 뜨므로 여기서 AP 를 지우고 읽는다 (다음 렌더가 다시 만든다)
  if (!FPDFAnnot_SetAP(annot, FPDF_ANNOT_APPEARANCEMODE_NORMAL, nullptr)) {
    return make_error(ErrorCode::kInvalidState, "cannot clear appearance stream");
  }
  // FPDFAnnot_GetColor 는 키가 없어도 기본 검정으로 성공한다 → 키 존재를 함께 본다 (없던 /IC 를 검정 채움으로 복원하지 않게)
  snap.has_color = FPDFAnnot_HasKey(annot, "C") && fill_color(annot, snap.color);
  snap.has_interior = FPDFAnnot_HasKey(annot, "IC") && fill_color(annot, snap.interior, FPDFANNOT_COLORTYPE_InteriorColor);
  snap.interior.a = snap.color.a;  // 알파는 /CA 하나 — 내부색 알파는 별도 값이 아니다
  float hr = 0, vr = 0, bw = 0;
  snap.has_border = FPDFAnnot_GetBorder(annot, &hr, &vr, &bw) != 0;
  snap.border_width = bw;
  snap.has_bs = FPDFAnnot_HasKey(annot, "BS") != 0;
  snap.flags = FPDFAnnot_GetFlags(annot);
  snap.contents = annot_string(annot, "Contents");
  snap.author = annot_string(annot, "T");
  snap.modified = annot_string(annot, "M");
  snap.subject = annot_string(annot, "Subj");
  return snap;
}

Result<void> PdfiumDocument::remove_annotation(int page_index, std::string_view id) {
  PdfiumLibrary::assert_current_thread();
  auto page = load_page(page_index);
  if (!page) return std::unexpected(page.error());
  auto found = find_annot(page->get(), id);
  if (!found) return std::unexpected(found.error());
  std::vector<int> indices{found->index};
  // 외부 프로그램이 만든 메모의 /Popup 은 함께 지운다 (남으면 고아가 된다). 복원 때는 다시 만들지 않는다 — 뷰어는 Popup 없이도 내용을 보여 준다
  if (ScopedAnnotation popup(FPDFAnnot_GetLinkedAnnot(found->annot.get(), "Popup")); popup) {
    const int popup_index = FPDFPage_GetAnnotIndex(page->get(), popup.get());
    if (popup_index >= 0 && popup_index != found->index) indices.push_back(popup_index);
  }
  found->annot.reset();  // 지우기 전에 핸들을 닫는다
  std::sort(indices.rbegin(), indices.rend());  // 뒤쪽부터 지워야 앞 인덱스가 밀리지 않는다
  const int before = FPDFPage_GetAnnotCount(page->get());
  for (int i : indices) {
    if (!FPDFPage_RemoveAnnot(page->get(), i)) return make_error(ErrorCode::kInvalidState, "FPDFPage_RemoveAnnot failed");
  }
  if (FPDFPage_GetAnnotCount(page->get()) != before - static_cast<int>(indices.size())) {
    return make_error(ErrorCode::kInvalidState, "annotation count mismatch after remove");
  }
  return {};
}

Result<void> PdfiumDocument::restore_annotation(int page_index, const AnnotationSnapshot& snap) {
  PdfiumLibrary::assert_current_thread();
  if (snap.id.empty()) return make_error(ErrorCode::kInvalidArgument, "snapshot without id");
  auto page = load_page(page_index);
  if (!page) return std::unexpected(page.error());
  const int before = FPDFPage_GetAnnotCount(page->get());
  ScopedAnnotation annot(FPDFPage_CreateAnnot(page->get(), subtype_of(snap.kind)));
  if (!annot) return make_error(ErrorCode::kInvalidState, "FPDFPage_CreateAnnot failed");
  // 중간에 실패하면 반만 만든 주석을 남기지 않는다
  const auto fail = [&](const char* what) -> Result<void> {
    annot.reset();
    FPDFPage_RemoveAnnot(page->get(), before);
    return make_error(ErrorCode::kInvalidState, what);
  };
  for (const auto& q : snap.quads) {
    FS_QUADPOINTSF quad{q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7]};
    if (!FPDFAnnot_AppendAttachmentPoints(annot.get(), &quad)) return fail("FPDFAnnot_AppendAttachmentPoints failed");
  }
  for (const auto& stroke : snap.ink) {
    std::vector<FS_POINTF> pts;
    for (const Point& p : stroke) pts.push_back({static_cast<float>(p.x), static_cast<float>(p.y)});
    if (!pts.empty() && FPDFAnnot_AddInkStroke(annot.get(), pts.data(), pts.size()) < 0) return fail("FPDFAnnot_AddInkStroke failed");
  }
  FS_RECTF rect{static_cast<float>(snap.rect[0]), static_cast<float>(snap.rect[3]), static_cast<float>(snap.rect[2]),
                static_cast<float>(snap.rect[1])};
  if (!FPDFAnnot_SetRect(annot.get(), &rect)) return fail("FPDFAnnot_SetRect failed");
  if (snap.has_border) (void)FPDFAnnot_SetBorder(annot.get(), 0, 0, snap.border_width);
  if (snap.has_color) {
    (void)FPDFAnnot_SetColor(annot.get(), FPDFANNOT_COLORTYPE_Color, snap.color.r, snap.color.g, snap.color.b, snap.color.a);
  }
  if (snap.has_interior) {
    (void)FPDFAnnot_SetColor(annot.get(), FPDFANNOT_COLORTYPE_InteriorColor, snap.interior.r, snap.interior.g,
                             snap.interior.b, snap.interior.a);
  }
  (void)FPDFAnnot_SetFlags(annot.get(), snap.flags);
  if (!set_annot_string(annot.get(), "NM", snap.id)) return fail("cannot set /NM");
  if (!snap.contents.empty()) (void)set_annot_string(annot.get(), "Contents", snap.contents);
  if (!snap.author.empty()) (void)set_annot_string(annot.get(), "T", snap.author);
  if (!snap.modified.empty()) (void)set_annot_string(annot.get(), "M", snap.modified);
  if (!snap.subject.empty()) (void)set_annot_string(annot.get(), "Subj", snap.subject);
  return {};  // AP 는 두지 않는다: 다음 렌더가 PDFium 모양으로 다시 만든다
}

Result<void> PdfiumDocument::set_annotation_color(int page_index, std::string_view id, Color color) {
  PdfiumLibrary::assert_current_thread();
  auto page = load_page(page_index);
  if (!page) return std::unexpected(page.error());
  auto found = find_annot(page->get(), id);
  if (!found) return std::unexpected(found.error());
  FPDF_ANNOTATION annot = found->annot.get();
  if (auto nm = ensure_nm(annot); !nm) return std::unexpected(nm.error());
  // AP 가 있으면 SetColor 가 실패한다: 지우고 색을 쓰면 다음 렌더가 새 AP 를 만든다
  if (!FPDFAnnot_SetAP(annot, FPDF_ANNOT_APPEARANCEMODE_NORMAL, nullptr)) {
    return make_error(ErrorCode::kInvalidState, "cannot clear appearance stream");
  }
  if (!FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_Color, color.r, color.g, color.b, color.a)) {
    return make_error(ErrorCode::kInvalidState, "FPDFAnnot_SetColor failed");
  }
  (void)set_annot_string(annot, "M", pdf_date_now());
  return {};
}

Result<void> PdfiumDocument::set_annotation_border_width(int page_index, std::string_view id, double width) {
  PdfiumLibrary::assert_current_thread();
  if (!(width > 0.0)) return make_error(ErrorCode::kInvalidArgument, "border width must be positive");
  auto page = load_page(page_index);
  if (!page) return std::unexpected(page.error());
  auto found = find_annot(page->get(), id);
  if (!found) return std::unexpected(found.error());
  FPDF_ANNOTATION annot = found->annot.get();
  if (auto nm = ensure_nm(annot); !nm) return std::unexpected(nm.error());
  // /BS /W 가 있으면 PDFium 은 /Border 를 무시하고, /BS 를 지울 API 는 없다 → 호출자가 삭제·재생성으로 우회한다
  if (FPDFAnnot_HasKey(annot, "BS")) return make_error(ErrorCode::kInvalidArgument, "border width is fixed by /BS");
  // FPDFAnnot_SetBorder 도 AP 를 지우지만(실측), 색 변경과 같은 규칙을 눈에 보이게 먼저 지운다
  if (!FPDFAnnot_SetAP(annot, FPDF_ANNOT_APPEARANCEMODE_NORMAL, nullptr)) {
    return make_error(ErrorCode::kInvalidState, "cannot clear appearance stream");
  }
  if (!FPDFAnnot_SetBorder(annot, 0, 0, static_cast<float>(width))) return make_error(ErrorCode::kInvalidState, "FPDFAnnot_SetBorder failed");
  (void)set_annot_string(annot, "M", pdf_date_now());
  return {};
}

// ---------------------------------------------------------------- 텍스트 객체 (#45)

Result<TextObjectsResult> PdfiumDocument::add_text_objects(int page_index, const TextObjectsSpec& spec) {
  PdfiumLibrary::assert_current_thread();
  auto page = load_page(page_index);
  if (!page) return std::unexpected(page.error());
  return add_text_objects_to(page->get(), spec);
}

// 같은 쪽 핸들 안에서 넣는다 (FPDF_LoadPage 는 호출마다 새 객체 목록을 만들어 다른 핸들의 변경을 보지 못한다, #65)
Result<TextObjectsResult> PdfiumDocument::add_text_objects_to(FPDF_PAGE raw_page, const TextObjectsSpec& spec) {
  if (spec.lines.empty()) return make_error(ErrorCode::kInvalidArgument, "no text lines");
  if (spec.font.key.empty() || spec.font.ttf.empty()) return make_error(ErrorCode::kInvalidArgument, "font spec incomplete");
  if (!spec.ids.empty() && spec.ids.size() != spec.lines.size()) return make_error(ErrorCode::kInvalidArgument, "ids/lines mismatch");
  for (const auto& line : spec.lines) {
    if (line.cids.empty()) return make_error(ErrorCode::kInvalidArgument, "empty line");
    if (!(line.font_size > 0.0)) return make_error(ErrorCode::kInvalidArgument, "font size must be positive");
  }
  struct PageRef { FPDF_PAGE raw; FPDF_PAGE get() const { return raw; } } page_ref{raw_page};
  const PageRef* page = &page_ref;
  // 폰트: 같은 서브셋은 문서당 한 번만 임베딩한다 (두 번 로드 = 두 번 임베딩, #43)
  FPDF_FONT font = nullptr;
  if (auto it = fonts_.find(spec.font.key); it != fonts_.end()) {
    font = it->second.font.get();
  } else {
    ScopedFont loaded(FPDFText_LoadCidType2Font(doc_.get(), spec.font.ttf.data(), static_cast<uint32_t>(spec.font.ttf.size()),
                                                spec.font.to_unicode_cmap.c_str(), spec.font.cid_to_gid.data(),
                                                static_cast<uint32_t>(spec.font.cid_to_gid.size())));
    if (!loaded) return make_error(ErrorCode::kInvalidState, "FPDFText_LoadCidType2Font failed");
    font = loaded.get();
    const bool cff = spec.font.ttf.size() >= 4 && std::memcmp(spec.font.ttf.data(), "OTTO", 4) == 0;
    if (cff) cff_keys_.insert(spec.font.key);
    fonts_.emplace(spec.font.key, LoadedFont{std::move(loaded), cff});
  }
  const detail::PageSpace space = detail::PageSpace::of(page->get());
  std::vector<std::string> ids;
  const std::string box = spec.box.empty() ? new_box_id() : spec.box;
  std::vector<FPDF_PAGEOBJECT> inserted;  // 소유권은 쪽에 있다 (보상 때 Remove+Destroy)
  const auto fail = [&](const char* what) -> Result<TextObjectsResult> {
    for (FPDF_PAGEOBJECT obj : inserted) {
      if (FPDFPage_RemoveObject(page->get(), obj)) FPDFPageObj_Destroy(obj);
    }
    if (!inserted.empty()) (void)FPDFPage_GenerateContent(page->get());
    return make_error(ErrorCode::kInvalidState, what);
  };
  for (std::size_t i = 0; i < spec.lines.size(); ++i) {
    const TextLineSpec& line = spec.lines[i];
    ScopedFPDFPageObject obj(FPDFPageObj_CreateTextObj(doc_.get(), font, static_cast<float>(line.font_size)));
    if (!obj) return fail("FPDFPageObj_CreateTextObj failed");
    if (!FPDFText_SetCharcodes(obj.get(), line.cids.data(), line.cids.size())) return fail("FPDFText_SetCharcodes failed");
    if (!FPDFPageObj_SetFillColor(obj.get(), line.color.r, line.color.g, line.color.b, 255)) return fail("FPDFPageObj_SetFillColor failed");
    const auto m = space.user_matrix_at(line.origin);
    const FS_MATRIX matrix{static_cast<float>(m[0]), static_cast<float>(m[1]), static_cast<float>(m[2]),
                           static_cast<float>(m[3]), static_cast<float>(m[4]), static_cast<float>(m[5])};
    if (!FPDFPageObj_SetMatrix(obj.get(), &matrix)) return fail("FPDFPageObj_SetMatrix failed");
    const std::string id = spec.ids.empty() ? new_text_id() : spec.ids[i];
    FPDF_PAGEOBJECTMARK mark = FPDFPageObj_AddMark(obj.get(), "OnPdf");
    if (mark == nullptr) return fail("FPDFPageObj_AddMark failed");
    if (!FPDFPageObjMark_SetStringParam(doc_.get(), obj.get(), mark, "id", id.c_str())) return fail("cannot set mark id");
    if (spec.write_box && !FPDFPageObjMark_SetStringParam(doc_.get(), obj.get(), mark, "box", box.c_str())) return fail("cannot set mark box");
    if (spec.write_box && !line.text.empty() &&
        !FPDFPageObjMark_SetBlobParam(doc_.get(), obj.get(), mark, "text", reinterpret_cast<const unsigned char*>(line.text.data()),
                                      static_cast<unsigned long>(line.text.size()))) {
      return fail("cannot set mark text");
    }
    if (spec.write_box && i == 0 && spec.width > 0.0) {
      const std::string width = format_points(spec.width);
      if (!FPDFPageObjMark_SetStringParam(doc_.get(), obj.get(), mark, "width", width.c_str())) return fail("cannot set mark width");
    }
    if (spec.write_box && i == 0 && !spec.box_text.empty() &&
        !FPDFPageObjMark_SetBlobParam(doc_.get(), obj.get(), mark, "box_text",
                                      reinterpret_cast<const unsigned char*>(spec.box_text.data()),
                                      static_cast<unsigned long>(spec.box_text.size()))) {
      return fail("cannot set mark box_text");
    }
    FPDF_PAGEOBJECT raw = obj.release();
    FPDFPage_InsertObject(page->get(), raw);  // 소유권 이전
    inserted.push_back(raw);
    ids.push_back(id);
  }
  if (!FPDFPage_GenerateContent(page->get())) return fail("FPDFPage_GenerateContent failed");
  return TextObjectsResult{std::move(ids), box};
}

Result<void> PdfiumDocument::remove_text_objects(int page_index, const std::vector<std::string>& ids) {
  PdfiumLibrary::assert_current_thread();
  if (ids.empty()) return make_error(ErrorCode::kInvalidArgument, "no ids");
  auto page = load_page(page_index);
  if (!page) return std::unexpected(page.error());
  // 전부 찾은 뒤에만 지운다 (부분 삭제 방지)
  std::vector<FPDF_PAGEOBJECT> found;
  const int count = FPDFPage_CountObjects(page->get());
  std::vector<std::string> unique_ids(ids);  // 중복 id 는 한 번만 (두 번째 RemoveObject 가 실패해 반만 지우지 않게, 리뷰 I11)
  std::sort(unique_ids.begin(), unique_ids.end());
  unique_ids.erase(std::unique(unique_ids.begin(), unique_ids.end()), unique_ids.end());
  for (const std::string& id : unique_ids) {
    FPDF_PAGEOBJECT hit = nullptr;
    for (int i = count - 1; i >= 0 && hit == nullptr; --i) {
      FPDF_PAGEOBJECT obj = FPDFPage_GetObject(page->get(), i);
      if (obj != nullptr && text_object_id_of(obj) == id) hit = obj;
    }
    if (hit == nullptr) return make_error(ErrorCode::kInvalidArgument, "text object not found: " + id);
    found.push_back(hit);
  }
  for (FPDF_PAGEOBJECT obj : found) {
    if (!FPDFPage_RemoveObject(page->get(), obj)) return make_error(ErrorCode::kInvalidState, "FPDFPage_RemoveObject failed");
    FPDFPageObj_Destroy(obj);
  }
  if (!FPDFPage_GenerateContent(page->get())) return make_error(ErrorCode::kInvalidState, "FPDFPage_GenerateContent failed");
  return {};
}

Result<std::vector<TextObjectInfo>> PdfiumDocument::text_objects(int page_index) const {
  PdfiumLibrary::assert_current_thread();
  auto page = load_page(page_index);
  if (!page) return std::unexpected(page.error());
  const detail::PageSpace space = detail::PageSpace::of(page->get());
  std::vector<TextObjectInfo> out;
  ScopedTextPage text_page;  // 옛 객체(원문 블롭 없음)의 글자 추출에만 연다
  const int count = FPDFPage_CountObjects(page->get());
  for (int i = 0; i < count; ++i) {
    FPDF_PAGEOBJECT obj = FPDFPage_GetObject(page->get(), i);
    if (obj == nullptr || FPDFPageObj_GetType(obj) != FPDF_PAGEOBJ_TEXT) continue;
    FPDF_PAGEOBJECTMARK mark = onpdf_mark_of(obj);
    std::string id = mark_string(mark, "id");
    if (id.empty()) continue;
    TextObjectInfo info;
    info.box = mark_string(mark, "box");
    if (info.box.empty()) info.box = id;  // #47 이전 객체: 줄 하나가 상자 하나
    info.text = mark_blob(mark, "text");
    info.box_text = mark_blob(mark, "box_text");
    if (const std::string width = mark_string(mark, "width"); !width.empty()) info.width = parse_points(width);
    info.original_font = mark_string(mark, "ofont") == "1";
    if (FPDF_FONT font = FPDFTextObj_GetFont(obj); font != nullptr) {
      const std::size_t n = FPDFFont_GetBaseFontName(font, nullptr, 0);
      if (n > 0) {
        std::string name(n, '\0');
        FPDFFont_GetBaseFontName(font, name.data(), n);
        if (!name.empty() && name.back() == '\0') name.pop_back();
        if (name.size() > 7 && name[6] == '+') name.erase(0, 7);  // 서브셋 태그
        info.font_name = std::move(name);
      }
    }
    if (info.text.empty()) {
      if (!text_page) text_page.reset(FPDFText_LoadPage(page->get()));
      if (text_page) {
        const unsigned long chars = FPDFTextObj_GetText(obj, text_page.get(), nullptr, 0);  // 바이트 수 (NUL 포함)
        if (chars > 2) {
          std::vector<unsigned char> buffer(chars);
          FPDFTextObj_GetText(obj, text_page.get(), reinterpret_cast<FPDF_WCHAR*>(buffer.data()), chars);
          info.text = detail::utf16le_to_utf8(buffer.data(), chars);
        }
      }
    }
    info.id = std::move(id);
    info.index = i;
    float size = 0;
    if (FPDFTextObj_GetFontSize(obj, &size)) info.font_size = size;
    unsigned r = 0, g = 0, b = 0, a = 255;
    if (FPDFPageObj_GetFillColor(obj, &r, &g, &b, &a)) {
      info.color = {static_cast<std::uint8_t>(r), static_cast<std::uint8_t>(g), static_cast<std::uint8_t>(b), static_cast<std::uint8_t>(a)};
    }
    FS_MATRIX m{};
    if (FPDFPageObj_GetMatrix(obj, &m)) info.baseline_origin = space.to_pt(m.e, m.f);
    float left = 0, bottom = 0, right = 0, top = 0;
    if (FPDFPageObj_GetBounds(obj, &left, &bottom, &right, &top)) info.bbox = space.to_rect(left, right, bottom, top);
    out.push_back(std::move(info));
  }
  return out;
}

Result<void> PdfiumDocument::move_page_objects(int page_index, const std::vector<std::string>& ids, Point delta_pt) {
  PdfiumLibrary::assert_current_thread();
  if (ids.empty()) return make_error(ErrorCode::kInvalidArgument, "no ids");
  auto page = load_page(page_index);
  if (!page) return std::unexpected(page.error());
  std::vector<FPDF_PAGEOBJECT> found;
  std::vector<std::string> unique_ids(ids);
  std::sort(unique_ids.begin(), unique_ids.end());
  unique_ids.erase(std::unique(unique_ids.begin(), unique_ids.end()), unique_ids.end());
  for (const std::string& id : unique_ids) {
    FPDF_PAGEOBJECT hit = find_page_object(page->get(), id).first;
    if (hit == nullptr) return make_error(ErrorCode::kInvalidArgument, "page object not found: " + id);
    found.push_back(hit);
  }
  // 쪽 포인트 Δ → 사용자 좌표 Δ: 기저 (a,b) = +x 1pt, (c,d) = 위쪽(−y) 1pt. 쪽 안에서 기저는 일정하다
  const detail::PageSpace space = detail::PageSpace::of(page->get());
  const auto basis = space.user_matrix_at({0.0, 0.0});
  const double du = basis[0] * delta_pt.x - basis[2] * delta_pt.y;
  const double dv = basis[1] * delta_pt.x - basis[3] * delta_pt.y;
  std::vector<std::pair<FPDF_PAGEOBJECT, FS_MATRIX>> moved;  // 중간 실패 시 되돌린다
  const auto fail = [&](const char* what) -> Result<void> {
    for (auto& [obj, original] : moved) (void)FPDFPageObj_SetMatrix(obj, &original);
    if (!moved.empty()) (void)FPDFPage_GenerateContent(page->get());
    return make_error(ErrorCode::kInvalidState, what);
  };
  for (FPDF_PAGEOBJECT obj : found) {
    FS_MATRIX m{};
    if (!FPDFPageObj_GetMatrix(obj, &m)) return fail("FPDFPageObj_GetMatrix failed");
    const FS_MATRIX original = m;
    m.e += static_cast<float>(du);
    m.f += static_cast<float>(dv);
    if (!FPDFPageObj_SetMatrix(obj, &m)) return fail("FPDFPageObj_SetMatrix failed");
    moved.emplace_back(obj, original);
  }
  if (!FPDFPage_GenerateContent(page->get())) return fail("FPDFPage_GenerateContent failed");
  return {};
}

// ---------------------------------------------------------------- 이미지·쪽 객체 일반 (#49)

Result<std::string> PdfiumDocument::add_image_object(int page_index, const ImageSpec& spec) {
  PdfiumLibrary::assert_current_thread();
  if (!(spec.rect.w > 0.0) || !(spec.rect.h > 0.0)) return make_error(ErrorCode::kInvalidArgument, "image rect must be positive");
  auto page = load_page(page_index);
  if (!page) return std::unexpected(page.error());
  ScopedFPDFPageObject obj(FPDFPageObj_NewImageObj(doc_.get()));
  if (!obj) return make_error(ErrorCode::kInvalidState, "FPDFPageObj_NewImageObj failed");
  FPDF_PAGE raw_page = page->get();
  if (const auto* pixels = std::get_if<ImagePixels>(&spec.source)) {
    if (pixels->width <= 0 || pixels->height <= 0) return make_error(ErrorCode::kInvalidArgument, "empty image");
    if (static_cast<std::int64_t>(pixels->width) * pixels->height > kMaxImagePixels) {
      return make_error(ErrorCode::kInvalidArgument, "image is too large");
    }
    if (pixels->bgra.size() != static_cast<std::size_t>(pixels->width) * static_cast<std::size_t>(pixels->height) * 4) {
      return make_error(ErrorCode::kInvalidArgument, "pixel buffer size mismatch");
    }
    // 외부 버퍼를 감싸기만 한다 — SetBitmap 이 복사하므로 PDFium 이 이 버퍼에 쓰지 않는다 (프로브 A)
    ScopedFPDFBitmap bitmap(FPDFBitmap_CreateEx(pixels->width, pixels->height, FPDFBitmap_BGRA,
                                                const_cast<std::uint8_t*>(pixels->bgra.data()), pixels->width * 4));
    if (!bitmap) return make_error(ErrorCode::kInvalidState, "FPDFBitmap_CreateEx failed");
    if (!FPDFImageObj_SetBitmap(&raw_page, 1, obj.get(), bitmap.get())) return make_error(ErrorCode::kInvalidState, "FPDFImageObj_SetBitmap failed");
  } else {
    const auto& jpeg = std::get<JpegBytes>(spec.source);
    // SOI(FFD8) 로 시작해야 한다: LoadJpegFileInline 은 헤더가 깨져도 성공을 돌려줄 수 있다
    if (jpeg.bytes.size() < 4 || jpeg.bytes[0] != 0xFF || jpeg.bytes[1] != 0xD8) {
      return make_error(ErrorCode::kInvalidFormat, "not a JPEG");
    }
    MemoryFileAccess access(jpeg.bytes);
    if (!FPDFImageObj_LoadJpegFileInline(&raw_page, 1, obj.get(), &access.access)) {
      return make_error(ErrorCode::kInvalidFormat, "FPDFImageObj_LoadJpegFileInline failed");
    }
    unsigned int pw = 0, ph = 0;
    if (!FPDFImageObj_GetImagePixelSize(obj.get(), &pw, &ph) || pw == 0 || ph == 0) {
      return make_error(ErrorCode::kInvalidFormat, "unreadable JPEG header");
    }
    if (static_cast<std::int64_t>(pw) * ph > kMaxImagePixels) return make_error(ErrorCode::kInvalidArgument, "image is too large");
  }
  const detail::PageSpace space = detail::PageSpace::of(raw_page);
  const FS_MATRIX matrix = image_matrix_for(space, spec.rect);
  if (!FPDFPageObj_SetMatrix(obj.get(), &matrix)) return make_error(ErrorCode::kInvalidState, "FPDFPageObj_SetMatrix failed");
  const std::string id = spec.id.empty() ? make_onpdf_id("onpdf-image-") : spec.id;
  FPDF_PAGEOBJECTMARK mark = FPDFPageObj_AddMark(obj.get(), "OnPdf");
  if (mark == nullptr || !FPDFPageObjMark_SetStringParam(doc_.get(), obj.get(), mark, "id", id.c_str()) ||
      !FPDFPageObjMark_SetStringParam(doc_.get(), obj.get(), mark, "kind", "image")) {
    return make_error(ErrorCode::kInvalidState, "cannot set image mark");
  }
  FPDF_PAGEOBJECT inserted = obj.release();
  FPDFPage_InsertObject(raw_page, inserted);  // 소유권 이전
  if (!FPDFPage_GenerateContent(raw_page)) {
    if (FPDFPage_RemoveObject(raw_page, inserted)) FPDFPageObj_Destroy(inserted);
    (void)FPDFPage_GenerateContent(raw_page);
    return make_error(ErrorCode::kInvalidState, "FPDFPage_GenerateContent failed");
  }
  return id;
}

Result<std::vector<PageObjectInfo>> PdfiumDocument::page_objects(int page_index) const {
  PdfiumLibrary::assert_current_thread();
  auto texts = text_objects(page_index);
  if (!texts) return std::unexpected(texts.error());
  auto page = load_page(page_index);
  if (!page) return std::unexpected(page.error());
  const detail::PageSpace space = detail::PageSpace::of(page->get());
  std::vector<PageObjectInfo> out;
  for (auto& t : *texts) {
    PageObjectInfo info;
    info.kind = ObjectKind::kText;
    info.id = t.id;
    info.marked = true;
    info.index = t.index;
    info.bbox = t.bbox;
    info.text = std::move(t);
    out.push_back(std::move(info));
  }
  const int count = FPDFPage_CountObjects(page->get());
  for (int i = 0; i < count; ++i) {
    FPDF_PAGEOBJECT obj = FPDFPage_GetObject(page->get(), i);
    if (obj == nullptr || FPDFPageObj_GetType(obj) != FPDF_PAGEOBJ_IMAGE) continue;
    PageObjectInfo info;
    info.kind = ObjectKind::kImage;
    info.index = i;
    info.id = text_object_id_of(obj);
    info.marked = !info.id.empty();
    if (!info.marked) info.id = std::string(kObjIndexPrefix) + std::to_string(i);
    unsigned int pw = 0, ph = 0;
    if (FPDFImageObj_GetImagePixelSize(obj, &pw, &ph)) {
      info.pixel_width = static_cast<int>(pw);
      info.pixel_height = static_cast<int>(ph);
    }
    info.filter = image_filter_of(obj);
    float left = 0, bottom = 0, right = 0, top = 0;
    if (FPDFPageObj_GetBounds(obj, &left, &bottom, &right, &top)) info.bbox = space.to_rect(left, right, bottom, top);
    out.push_back(std::move(info));
  }
  std::sort(out.begin(), out.end(), [](const PageObjectInfo& a, const PageObjectInfo& b) { return a.index < b.index; });
  return out;
}

Result<std::string> PdfiumDocument::mark_object(int page_index, std::string_view id) {
  PdfiumLibrary::assert_current_thread();
  if (!id.starts_with(kObjIndexPrefix)) return std::string(id);  // 이미 마크 id
  auto page = load_page(page_index);
  if (!page) return std::unexpected(page.error());
  auto [obj, index] = find_page_object(page->get(), id);
  if (obj == nullptr || FPDFPageObj_GetType(obj) != FPDF_PAGEOBJ_IMAGE) {
    return make_error(ErrorCode::kInvalidArgument, "image not found: " + std::string(id));
  }
  if (std::string existing = text_object_id_of(obj); !existing.empty()) return existing;
  const std::string new_id = make_onpdf_id("onpdf-image-");
  FPDF_PAGEOBJECTMARK mark = FPDFPageObj_AddMark(obj, "OnPdf");
  if (mark == nullptr || !FPDFPageObjMark_SetStringParam(doc_.get(), obj, mark, "id", new_id.c_str()) ||
      !FPDFPageObjMark_SetStringParam(doc_.get(), obj, mark, "kind", "image")) {
    return make_error(ErrorCode::kInvalidState, "cannot mark image");
  }
  if (!FPDFPage_GenerateContent(page->get())) return make_error(ErrorCode::kInvalidState, "FPDFPage_GenerateContent failed");
  return new_id;
}

Result<void> PdfiumDocument::set_object_rect(int page_index, std::string_view id, Rect rect_pt) {
  PdfiumLibrary::assert_current_thread();
  if (!(rect_pt.w > 0.0) || !(rect_pt.h > 0.0)) return make_error(ErrorCode::kInvalidArgument, "rect must be positive");
  auto page = load_page(page_index);
  if (!page) return std::unexpected(page.error());
  FPDF_PAGEOBJECT obj = find_page_object(page->get(), id).first;
  if (obj == nullptr || FPDFPageObj_GetType(obj) != FPDF_PAGEOBJ_IMAGE) {
    return make_error(ErrorCode::kInvalidArgument, "image not found: " + std::string(id));
  }
  FS_MATRIX original{};
  if (!FPDFPageObj_GetMatrix(obj, &original)) return make_error(ErrorCode::kInvalidState, "FPDFPageObj_GetMatrix failed");
  float l = 0, b = 0, r = 0, t = 0;
  if (!FPDFPageObj_GetBounds(obj, &l, &b, &r, &t)) return make_error(ErrorCode::kInvalidState, "FPDFPageObj_GetBounds failed");
  // 기존 행렬에 "현재 bbox → 새 bbox" 축척을 덧붙인다: 다른 프로그램 이미지의 회전·뒤집기가 남고,
  // 취소(이전 사각형으로 다시 축척)가 원래 행렬로 돌아온다 (리뷰 W2). 회전 쪽도 사용자 좌표에서 bbox 는 축 정렬이다
  const auto target = detail::PageSpace::of(page->get()).user_rect_of(rect_pt);  // {left, bottom, right, top}
  const double sx = (r - l) > 1e-6 ? (target[2] - target[0]) / (r - l) : 0.0;
  const double sy = (t - b) > 1e-6 ? (target[3] - target[1]) / (t - b) : 0.0;
  if (!(sx > 0.0) || !(sy > 0.0)) return make_error(ErrorCode::kInvalidState, "image has an empty bounding box");
  const FS_MATRIX matrix{static_cast<float>(original.a * sx), static_cast<float>(original.b * sy),
                         static_cast<float>(original.c * sx), static_cast<float>(original.d * sy),
                         static_cast<float>((original.e - l) * sx + target[0]), static_cast<float>((original.f - b) * sy + target[1])};
  if (!FPDFPageObj_SetMatrix(obj, &matrix)) return make_error(ErrorCode::kInvalidState, "FPDFPageObj_SetMatrix failed");
  if (!FPDFPage_GenerateContent(page->get())) {
    (void)FPDFPageObj_SetMatrix(obj, &original);
    (void)FPDFPage_GenerateContent(page->get());
    return make_error(ErrorCode::kInvalidState, "FPDFPage_GenerateContent failed");
  }
  return {};
}

Result<DetachedId> PdfiumDocument::detach_objects(int page_index, const std::vector<std::string>& ids) {
  PdfiumLibrary::assert_current_thread();
  if (ids.empty()) return make_error(ErrorCode::kInvalidArgument, "no ids");
  auto page = load_page(page_index);
  if (!page) return std::unexpected(page.error());
  std::vector<std::pair<int, FPDF_PAGEOBJECT>> found;
  for (const std::string& id : ids) {
    auto [obj, index] = find_page_object(page->get(), id);
    if (obj == nullptr) return make_error(ErrorCode::kInvalidArgument, "page object not found: " + id);
    if (std::none_of(found.begin(), found.end(), [&](const auto& f) { return f.second == obj; })) found.emplace_back(index, obj);
  }
  // 뒤에서부터 떼어 앞 객체의 index 가 흔들리지 않게 한다
  std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  Detached detached;
  detached.page = page_index;
  for (auto& [index, obj] : found) {
    if (!FPDFPage_RemoveObject(page->get(), obj)) {
      // 보상: 이미 뗀 것을 원래 자리에 되돌린다 (index 오름차순)
      for (auto it = detached.objects.rbegin(); it != detached.objects.rend(); ++it) {
        (void)FPDFPage_InsertObjectAtIndex(page->get(), it->second.release(), static_cast<std::size_t>(it->first));
      }
      (void)FPDFPage_GenerateContent(page->get());
      return make_error(ErrorCode::kInvalidState, "FPDFPage_RemoveObject failed");
    }
    if (text_object_id_of(obj).empty()) detached.foreign = true;  // 마크 없음: 다시 열기 뒤 찾을 수 없다 (#65)
    detached.objects.emplace_back(index, ScopedFPDFPageObject(obj));
  }
  std::reverse(detached.objects.begin(), detached.objects.end());  // index 오름차순
  if (!FPDFPage_GenerateContent(page->get())) {
    for (auto& [index, obj] : detached.objects) {
      (void)FPDFPage_InsertObjectAtIndex(page->get(), obj.release(), static_cast<std::size_t>(index));
    }
    (void)FPDFPage_GenerateContent(page->get());
    return make_error(ErrorCode::kInvalidState, "FPDFPage_GenerateContent failed");
  }
  const DetachedId token = next_detached_++;
  detached_.emplace(token, std::move(detached));
  return token;
}

Result<void> PdfiumDocument::reattach_objects(DetachedId id) {
  PdfiumLibrary::assert_current_thread();
  auto it = detached_.find(id);
  if (it == detached_.end()) return make_error(ErrorCode::kInvalidArgument, "unknown detached objects");
  if (it->second.lost) return make_error(ErrorCode::kInvalidState, "object lost by undo of a line edit");  // #65 C1
  auto page = load_page(it->second.page);
  if (!page) return std::unexpected(page.error());
  std::vector<FPDF_PAGEOBJECT> inserted;
  for (auto& [index, obj] : it->second.objects) {  // index 오름차순: 앞 것부터 넣어야 원래 자리가 맞는다
    const int count = FPDFPage_CountObjects(page->get());
    const std::size_t at = static_cast<std::size_t>(std::min(index, count));
    FPDF_PAGEOBJECT raw = obj.get();
    if (!FPDFPage_InsertObjectAtIndex(page->get(), raw, at)) {
      for (FPDF_PAGEOBJECT done : inserted) (void)FPDFPage_RemoveObject(page->get(), done);  // 보관 상태로 되돌림
      return make_error(ErrorCode::kInvalidState, "FPDFPage_InsertObjectAtIndex failed");
    }
    inserted.push_back(raw);
  }
  for (auto& [index, obj] : it->second.objects) (void)obj.release();  // 소유권은 쪽으로
  detached_.erase(it);
  if (!FPDFPage_GenerateContent(page->get())) return make_error(ErrorCode::kInvalidState, "FPDFPage_GenerateContent failed");
  return {};
}

void PdfiumDocument::remap_detached_pages(const std::vector<int>& new_index_of_old) {
  const auto remap = [&](int old) -> int {
    if (old < 0 || old >= static_cast<int>(new_index_of_old.size())) return -1;
    return new_index_of_old[static_cast<std::size_t>(old)];
  };
  for (auto& [id, d] : detached_) {
    const int now = remap(d.page);
    if (now < 0) {  // 쪽이 지워졌다: 되돌릴 자리가 없다 (쪽 삭제 취소는 스냅샷에서 새 쪽을 만들므로 이 객체와 무관)
      d.objects.clear();
      d.lost = true;
    } else {
      d.page = now;
    }
  }
  std::unordered_map<int, std::unordered_set<std::string>> moved;
  for (auto& [page, marks] : dropped_marks_) {
    const int now = remap(page);
    if (now >= 0) moved[now].insert(marks.begin(), marks.end());
  }
  dropped_marks_ = std::move(moved);
}

void PdfiumDocument::drop_detached(DetachedId id) noexcept {
  auto it = detached_.find(id);
  if (it == detached_.end()) return;
  for (auto& [index, obj] : it->second.objects) {
    if (const std::string mark = text_object_id_of(obj.get()); !mark.empty()) dropped_marks_[it->second.page].insert(mark);
  }
  detached_.erase(it);
}

Result<void> PdfiumDocument::set_annotation_contents(int page_index, std::string_view id, std::string_view utf8) {
  PdfiumLibrary::assert_current_thread();
  auto page = load_page(page_index);
  if (!page) return std::unexpected(page.error());
  auto found = find_annot(page->get(), id);
  if (!found) return std::unexpected(found.error());
  FPDF_ANNOTATION annot = found->annot.get();
  if (auto nm = ensure_nm(annot); !nm) return std::unexpected(nm.error());
  if (!set_annot_string(annot, "Contents", std::string(utf8))) return make_error(ErrorCode::kInvalidState, "cannot set /Contents");
  (void)set_annot_string(annot, "M", pdf_date_now());
  return {};
}

namespace {

// FPDF_FILEWRITE → std::ofstream. PDFium 이 WriteBlock 을 여러 번 부른다
struct FileWriter : FPDF_FILEWRITE {
  std::ofstream* out = nullptr;
  static int write_block(FPDF_FILEWRITE* self, const void* data, unsigned long size) {
    auto* writer = static_cast<FileWriter*>(self);
    writer->out->write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    return writer->out->good() ? 1 : 0;
  }
};

}  // namespace

std::vector<std::string> PdfiumDocument::cff_font_keys() const {
  std::vector<std::string> keys(cff_keys_.begin(), cff_keys_.end());
  std::sort(keys.begin(), keys.end());
  return keys;
}

bool PdfiumDocument::has_unreferenced_fonts() const {
  PdfiumLibrary::assert_current_thread();
  if (fonts_.empty()) return false;
  // FPDFText_LoadCidType2Font 와 FPDFPageObj_CreateTextObj 는 같은 CPDF_Font 를 공유하므로 핸들 포인터로 대조한다.
  // OnPdf 텍스트는 쪽 직속 객체뿐이라 Form XObject 안은 보지 않는다
  std::unordered_set<FPDF_FONT> unused;
  for (const auto& [key, loaded] : fonts_) unused.insert(loaded.font.get());
  const int pages = page_count();
  for (int i = 0; i < pages && !unused.empty(); ++i) {
    auto page = load_page(i);
    if (!page) return false;
    const int n = FPDFPage_CountObjects(page->get());
    for (int k = 0; k < n && !unused.empty(); ++k) {
      FPDF_PAGEOBJECT obj = FPDFPage_GetObject(page->get(), k);
      if (obj == nullptr || FPDFPageObj_GetType(obj) != FPDF_PAGEOBJ_TEXT) continue;
      if (FPDF_FONT font = FPDFTextObj_GetFont(obj); font != nullptr) unused.erase(font);
    }
  }
  return !unused.empty();
}

Result<void> PdfiumDocument::save_to(const std::filesystem::path& path, bool incremental) const {
  PdfiumLibrary::assert_current_thread();
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return make_error(ErrorCode::kIoError, "cannot create: " + path_to_utf8(path));
  FileWriter writer{};
  writer.version = 1;
  writer.WriteBlock = &FileWriter::write_block;
  writer.out = &out;
  if (auto prepared = prepare_for_save(); !prepared) return prepared;
  const FPDF_DWORD flags = incremental ? FPDF_INCREMENTAL : FPDF_NO_INCREMENTAL;
  const bool ok = FPDF_SaveAsCopy(doc_.get(), &writer, flags) != 0;
  out.flush();
  if (!ok || !out.good()) {
    return make_error(ErrorCode::kIoError, "write failed: " + path_to_utf8(path));
  }
  out.close();
  if (out.fail()) return make_error(ErrorCode::kIoError, "close failed: " + path_to_utf8(path));
  return {};
}

int PdfiumDocument::signature_count() const {
  PdfiumLibrary::assert_current_thread();
  return std::max(0, FPDF_GetSignatureCount(doc_.get()));
}

EditPermissions PdfiumDocument::edit_permissions() const {
  PdfiumLibrary::assert_current_thread();
  return permissions_of(doc_.get());
}

EditPermissions PdfiumDocument::permissions_of(FPDF_DOCUMENT doc) {
  // 소유자 암호로 열었거나 암호화되지 않았으면 모든 비트가 켜져 있다
  const unsigned long p = FPDF_GetDocPermissions(doc);
  const auto bit = [p](int n) { return (p & (1UL << (n - 1))) != 0; };
  const int revision = FPDF_GetSecurityHandlerRevision(doc);  // 암호화되지 않았으면 -1
  EditPermissions permissions;
  permissions.extract = bit(5);
  permissions.modify_contents = bit(4);
  permissions.annotate = bit(6);
  permissions.fill_forms = bit(6) || (revision >= 3 && bit(9));
  permissions.assemble = revision >= 3 ? bit(11) : bit(4);
  return permissions;
}

Result<ScopedPage> PdfiumDocument::load_page(int page_index) const {
  if (page_index < 0 || page_index >= page_count()) {
    return make_error(ErrorCode::kPageNotFound,
                      "page index out of range: " + std::to_string(page_index));
  }
  ScopedPage page(FPDF_LoadPage(doc_.get(), page_index));
  if (!page) {
    return std::unexpected(last_pdfium_error("FPDF_LoadPage failed"));
  }
  return page;
}

Result<PageSize> PdfiumDocument::page_size(int page_index) const {
  PdfiumLibrary::assert_current_thread();
  FS_SIZEF size{};
  if (page_index < 0 || page_index >= page_count() ||
      !FPDF_GetPageSizeByIndexF(doc_.get(), page_index, &size)) {
    return make_error(ErrorCode::kPageNotFound,
                      "page index out of range: " + std::to_string(page_index));
  }
  return PageSize{size.width, size.height};
}

Result<Bitmap> PdfiumDocument::render_page(int page_index, const RenderOptions& options) const {
  PdfiumLibrary::assert_current_thread();
  if (!(options.scale > 0.0f) || !std::isfinite(options.scale)) {
    return make_error(ErrorCode::kInvalidArgument, "scale must be positive");
  }
  int rotate = 0;
  switch (options.rotation) {
    case 0: rotate = 0; break;
    case 90: rotate = 1; break;
    case 180: rotate = 2; break;
    case 270: rotate = 3; break;
    default: return make_error(ErrorCode::kInvalidArgument, "rotation must be 0/90/180/270");
  }

  auto page = load_page(page_index);
  if (!page) {
    return std::unexpected(page.error());
  }

  double page_w = FPDF_GetPageWidthF(page->get());
  double page_h = FPDF_GetPageHeightF(page->get());
  if (rotate % 2 == 1) {
    std::swap(page_w, page_h);
  }
  const int width = std::max(1, static_cast<int>(std::lround(page_w * options.scale)));
  const int height = std::max(1, static_cast<int>(std::lround(page_h * options.scale)));

  ScopedBitmap bitmap(FPDFBitmap_CreateEx(width, height, FPDFBitmap_BGRA, nullptr, 0));
  if (!bitmap) {
    return make_error(ErrorCode::kRenderFailed, "FPDFBitmap_CreateEx failed");
  }
  FPDFBitmap_FillRect(bitmap.get(), 0, 0, width, height, 0xFFFFFFFF);

  int flags = 0;
  if (options.render_annotations) flags |= FPDF_ANNOT;
  if (options.lcd_text) flags |= FPDF_LCD_TEXT;
  if (options.printing) flags |= FPDF_PRINTING;
  if (options.grayscale) flags |= FPDF_GRAYSCALE;
  FPDF_RenderPageBitmap(bitmap.get(), page->get(), 0, 0, width, height, rotate, flags);
  if (options.render_annotations) draw_form(bitmap.get(), page->get(), 0, 0, width, height, rotate, flags);  // 위젯 (#51)

  const int stride = FPDFBitmap_GetStride(bitmap.get());
  const auto* buffer = static_cast<const std::uint8_t*>(FPDFBitmap_GetBuffer(bitmap.get()));
  if (buffer == nullptr || stride <= 0) {
    return make_error(ErrorCode::kRenderFailed, "FPDFBitmap_GetBuffer failed");
  }

  Bitmap out;
  out.width = width;
  out.height = height;
  out.stride = width * 4;
  out.pixels.resize(static_cast<std::size_t>(out.stride) * static_cast<std::size_t>(height));
  for (int y = 0; y < height; ++y) {
    std::memcpy(out.pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(out.stride),
                buffer + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride),
                static_cast<std::size_t>(out.stride));
  }
  return out;
}

Result<Bitmap> PdfiumDocument::render_region(int page_index, const RenderOptions& options,
                                             const Rect& region_px) const {
  PdfiumLibrary::assert_current_thread();
  if (!(options.scale > 0.0f) || !std::isfinite(options.scale)) {
    return make_error(ErrorCode::kInvalidArgument, "scale must be positive");
  }
  const int width = static_cast<int>(std::ceil(region_px.w));
  const int height = static_cast<int>(std::ceil(region_px.h));
  if (width <= 0 || height <= 0) {
    return make_error(ErrorCode::kInvalidArgument, "region must be non-empty");
  }
  auto page = load_page(page_index);
  if (!page) return std::unexpected(page.error());

  ScopedBitmap bitmap(FPDFBitmap_CreateEx(width, height, FPDFBitmap_BGRA, nullptr, 0));
  if (!bitmap) return make_error(ErrorCode::kRenderFailed, "FPDFBitmap_CreateEx failed");
  FPDFBitmap_FillRect(bitmap.get(), 0, 0, width, height, 0xFFFFFFFF);

  // 매트릭스는 "회전 0, 쪽 크기 그대로 렌더했을 때의 장치 좌표(좌상단 원점)" 에 적용된다.
  // 배율 s 로 키우고 영역 원점이 (0,0) 에 오도록 평행이동.
  const FS_MATRIX matrix{options.scale, 0.0f, 0.0f, options.scale,
                         static_cast<float>(-region_px.x), static_cast<float>(-region_px.y)};
  const FS_RECTF clip{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)};
  int flags = 0;
  if (options.render_annotations) flags |= FPDF_ANNOT;
  if (options.lcd_text) flags |= FPDF_LCD_TEXT;
  if (options.printing) flags |= FPDF_PRINTING;
  if (options.grayscale) flags |= FPDF_GRAYSCALE;
  FPDF_RenderPageBitmapWithMatrix(bitmap.get(), page->get(), &matrix, &clip, flags);
  if (options.render_annotations) {
    // 위젯은 행렬을 받지 않는다: 쪽 전체를 그린 장치 평면에서 영역 원점만큼 음수로 시작 (#51 프로브: 전체 렌더와 일치)
    const double page_w = FPDF_GetPageWidthF(page->get()) * options.scale;
    const double page_h = FPDF_GetPageHeightF(page->get()) * options.scale;
    draw_form(bitmap.get(), page->get(), static_cast<int>(std::lround(-region_px.x)), static_cast<int>(std::lround(-region_px.y)),
              std::max(1, static_cast<int>(std::lround(page_w))), std::max(1, static_cast<int>(std::lround(page_h))), 0, flags);
  }

  const int stride = FPDFBitmap_GetStride(bitmap.get());
  const auto* buffer = static_cast<const std::uint8_t*>(FPDFBitmap_GetBuffer(bitmap.get()));
  if (buffer == nullptr || stride <= 0) {
    return make_error(ErrorCode::kRenderFailed, "FPDFBitmap_GetBuffer failed");
  }
  Bitmap out;
  out.width = width;
  out.height = height;
  out.stride = width * 4;
  out.pixels.resize(static_cast<std::size_t>(out.stride) * static_cast<std::size_t>(height));
  for (int y = 0; y < height; ++y) {
    std::memcpy(out.pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(out.stride),
                buffer + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride),
                static_cast<std::size_t>(out.stride));
  }
  return out;
}

Result<view::PageTextIndex> PdfiumDocument::load_text(int page_index) const {
  PdfiumLibrary::assert_current_thread();
  auto page = load_page(page_index);
  if (!page) return std::unexpected(page.error());
  auto text_page = TextPage::load(page->get());
  if (!text_page) return std::unexpected(text_page.error());
  return text_page->extract(doc_.get(), page->get(), page_index);
}

Result<view::Outline> PdfiumDocument::load_outline() const {
  PdfiumLibrary::assert_current_thread();
  view::Outline outline;
  FPDF_DOCUMENT doc = doc_.get();
  const int page_count = FPDF_GetPageCount(doc);

  // 명시적 스택 전위 순회: 프레임은 "아직 처리하지 않은 형제 체인의 다음 노드".
  // 자식 체인 프레임을 위에 쌓아 자식을 형제보다 먼저 처리한다 (재귀 없음 → C++ 스택 깊이와 무관)
  struct Frame {
    FPDF_BOOKMARK next;
    int parent;
    int depth;
  };
  std::vector<Frame> stack;
  std::unordered_set<FPDF_BOOKMARK> visited;
  stack.push_back({FPDFBookmark_GetFirstChild(doc, nullptr), -1, 0});

  while (!stack.empty()) {
    const std::size_t top = stack.size() - 1;
    FPDF_BOOKMARK node = stack[top].next;
    if (node == nullptr) {
      stack.pop_back();
      continue;
    }
    if (!visited.insert(node).second) {
      // 이미 본 북마크: 형제·자식 순환. 이 체인을 끊는다
      outline.cycle_detected = true;
      stack.pop_back();
      continue;
    }
    if (outline.size() >= view::Outline::kMaxItems) {
      outline.truncated = true;
      break;
    }
    const int parent = stack[top].parent;
    const int depth = stack[top].depth;
    stack[top].next = FPDFBookmark_GetNextSibling(doc, node);

    view::OutlineItem item;
    item.depth = depth;
    item.parent = parent;
    item.open = FPDFBookmark_GetCount(node) > 0;
    // 제목: UTF-16LE, 바이트 수(NUL 포함) 반환
    const unsigned long title_bytes = FPDFBookmark_GetTitle(node, nullptr, 0);
    if (title_bytes > 2) {
      std::vector<unsigned char> buf(title_bytes);
      FPDFBookmark_GetTitle(node, buf.data(), title_bytes);
      item.title = detail::utf16le_to_utf8(buf.data(), buf.size());
    }
    // 대상: /Dest 우선, 없으면 액션(GoTo/URI). Launch·RemoteGoTo 는 무시 (보안)
    view::Link link;
    bool has_target = false;
    if (FPDF_DEST dest = FPDFBookmark_GetDest(doc, node); dest != nullptr) {
      has_target = detail::fill_target_lazy(doc, dest, link);
    } else if (FPDF_ACTION action = FPDFBookmark_GetAction(node); action != nullptr) {
      const unsigned long type = FPDFAction_GetType(action);
      if (type == PDFACTION_GOTO) {
        if (FPDF_DEST d = FPDFAction_GetDest(doc, action); d != nullptr) has_target = detail::fill_target_lazy(doc, d, link);
      } else if (type == PDFACTION_URI) {
        link.kind = view::LinkKind::kUri;
        link.uri = detail::uri_of(doc, action);
        has_target = !link.uri.empty();
      }
    }
    if (has_target) {
      if (link.kind == view::LinkKind::kPage && link.user_location && link.page < page_count) {
        // 근사 쪽 포인트 y: 쪽을 열지 않고 높이로 뒤집기 (회전 없는 쪽에서 정확)
        FS_SIZEF size{};
        if (FPDF_GetPageSizeByIndexF(doc, link.page, &size)) {
          item.approx_y = std::clamp(static_cast<double>(size.height) - link.user_location->y, 0.0,
                                     static_cast<double>(size.height));
        }
      }
      item.target = std::move(link);
    }

    const int index = outline.size();
    outline.items.push_back(std::move(item));
    if (parent < 0) {
      outline.roots.push_back(index);
    } else {
      outline.items[static_cast<std::size_t>(parent)].children.push_back(index);
    }

    if (FPDF_BOOKMARK child = FPDFBookmark_GetFirstChild(doc, node); child != nullptr) {
      if (depth + 1 >= view::Outline::kMaxDepth) {
        outline.truncated = true;
      } else {
        stack.push_back({child, index, depth + 1});
      }
    }
  }
  outline.build_position_index();
  return outline;
}

Result<Point> PdfiumDocument::resolve_location(int page_index, Point user_pt) const {
  PdfiumLibrary::assert_current_thread();
  auto page = load_page(page_index);
  if (!page) return std::unexpected(page.error());
  return detail::PageSpace::of(page->get()).to_pt(user_pt.x, user_pt.y);
}

Result<view::PrintPreferences> PdfiumDocument::print_preferences() const {
  PdfiumLibrary::assert_current_thread();
  FPDF_DOCUMENT doc = doc_.get();
  view::PrintPreferences prefs;
  // 권한: 암호화되지 않았거나 소유자 암호로 열면 모든 비트가 켜져 있다
  const unsigned long perm = FPDF_GetDocPermissions(doc);
  prefs.allowed = (perm & (1UL << 2)) != 0;
  prefs.high_quality = prefs.allowed && (perm & (1UL << 11)) != 0;

  prefs.no_scaling = !FPDF_VIEWERREF_GetPrintScaling(doc);
  prefs.copies = std::clamp(FPDF_VIEWERREF_GetNumCopies(doc), 1, 999);

  const int count = page_count();
  if (FPDF_PAGERANGE range = FPDF_VIEWERREF_GetPrintPageRange(doc); range != nullptr) {
    const std::size_t n = FPDF_VIEWERREF_GetPrintPageRangeCount(range);
    // 쌍으로만 해석한다. PDF 명세상 1 기준 쪽 번호이고 PDFium 은 값을 그대로 돌려준다 → 0 기준으로 변환
    for (std::size_t i = 0; i + 1 < n && prefs.page_ranges.size() < 1000; i += 2) {
      const int first = FPDF_VIEWERREF_GetPrintPageRangeElement(range, i) - 1;
      const int last = FPDF_VIEWERREF_GetPrintPageRangeElement(range, i + 1) - 1;
      if (first < 0 || last < first || first >= count) continue;
      prefs.page_ranges.emplace_back(first, std::min(last, count - 1));
    }
  }
  switch (FPDF_VIEWERREF_GetDuplex(doc)) {
    case Simplex: prefs.duplex = view::Duplex::kSimplex; break;
    case DuplexFlipShortEdge: prefs.duplex = view::Duplex::kFlipShortEdge; break;
    case DuplexFlipLongEdge: prefs.duplex = view::Duplex::kFlipLongEdge; break;
    default: prefs.duplex = view::Duplex::kUndefined; break;
  }
  return prefs;
}

Result<view::PageSearchResult> PdfiumDocument::search_page(int page_index, const std::string& query_utf8,
                                                          view::SearchOptions options) const {
  PdfiumLibrary::assert_current_thread();
  auto page = load_page(page_index);
  if (!page) return std::unexpected(page.error());
  auto text_page = TextPage::load(page->get());
  if (!text_page) return std::unexpected(text_page.error());

  view::PageSearchResult result;
  result.page = page_index;
  result.char_count = text_page->raw_char_count();
  // 먼저 찾고, 결과가 있을 때만 인덱스를 만든다 (검색 비용의 대부분이 인덱스 추출)
  const std::vector<view::SourceRange> found = text_page->find(query_utf8, options);
  if (found.empty()) return result;
  auto index = text_page->extract(doc_.get(), page->get(), page_index);
  if (!index) return std::unexpected(index.error());
  // PDFium 검색기는 매핑 오류 글자도 PDFium 값(글자 코드)으로 맞춘다. 인덱스는 그 글자를 U+FFFD 로 두므로
  // (복사·선택과 같은 기준으로) U+FFFD 글자를 포함한 결과는 버린다. 검색어에 U+FFFD 가 있으면 그대로 둔다
  const bool query_has_replacement = query_utf8.find("\xEF\xBF\xBD") != std::string::npos;
  for (const view::SourceRange& src : found) {
    const view::TextRange range = view::map_source_range(*index, src);
    if (range.empty()) continue;
    if (!query_has_replacement) {
      const auto first = index->chars.begin() + range.start;
      const auto last = index->chars.begin() + range.end;
      if (std::any_of(first, last, [](const view::TextChar& ch) { return ch.code == U'\uFFFD'; })) continue;
    }
    result.ranges.push_back(range);
  }
  std::sort(result.ranges.begin(), result.ranges.end(),
            [](const view::TextRange& a, const view::TextRange& b) { return a.start < b.start; });
  result.rects.reserve(result.ranges.size());
  for (const view::TextRange& range : result.ranges) result.rects.push_back(index->selection_rects(range));
  return result;
}

// ---------------------------------------------------------------- 폼 채우기 (#51)

namespace {

using FormStringFn = unsigned long (*)(FPDF_FORMHANDLE, FPDF_ANNOTATION, FPDF_WCHAR*, unsigned long);

std::string form_string(FormStringFn fn, FPDF_FORMHANDLE form, FPDF_ANNOTATION annot) {
  const unsigned long bytes = fn(form, annot, nullptr, 0);
  if (bytes <= 2) return {};
  std::vector<FPDF_WCHAR> buf(bytes / 2 + 1, 0);
  if (fn(form, annot, buf.data(), static_cast<unsigned long>(buf.size() * 2)) == 0) return {};
  return detail::utf16le_to_utf8(reinterpret_cast<const unsigned char*>(buf.data()), bytes);
}

std::string option_label(FPDF_FORMHANDLE form, FPDF_ANNOTATION annot, int index) {
  const unsigned long bytes = FPDFAnnot_GetOptionLabel(form, annot, index, nullptr, 0);
  if (bytes <= 2) return {};
  std::vector<FPDF_WCHAR> buf(bytes / 2 + 1, 0);
  if (FPDFAnnot_GetOptionLabel(form, annot, index, buf.data(), static_cast<unsigned long>(buf.size() * 2)) == 0) return {};
  return detail::utf16le_to_utf8(reinterpret_cast<const unsigned char*>(buf.data()), bytes);
}

FieldType field_type_of(int type) {
  switch (type) {
    case FPDF_FORMFIELD_PUSHBUTTON: return FieldType::kPushButton;
    case FPDF_FORMFIELD_CHECKBOX: return FieldType::kCheckBox;
    case FPDF_FORMFIELD_RADIOBUTTON: return FieldType::kRadio;
    case FPDF_FORMFIELD_COMBOBOX: return FieldType::kComboBox;
    case FPDF_FORMFIELD_LISTBOX: return FieldType::kListBox;
    case FPDF_FORMFIELD_TEXTFIELD: return FieldType::kText;
    case FPDF_FORMFIELD_SIGNATURE: return FieldType::kSignature;
    default: return FieldType::kUnknown;
  }
}

constexpr int kFormFlagComb = 1 << 24;

// 위젯 하나의 스냅샷. 이름이 없거나 타입을 모르면 nullopt (손상 필드는 건너뛴다)
std::optional<FormWidgetInfo> widget_info(FPDF_FORMHANDLE form, FPDF_ANNOTATION annot, const detail::PageSpace& space,
                                          int annot_index, int nth = 0) {
  FormWidgetInfo info;
  info.type = field_type_of(FPDFAnnot_GetFormFieldType(form, annot));
  info.ref.name = form_string(FPDFAnnot_GetFormFieldName, form, annot);
  if (info.type == FieldType::kUnknown || info.ref.name.empty()) return std::nullopt;
  info.ref.nth = nth;
  info.control_count = std::max(1, FPDFAnnot_GetFormControlCount(form, annot));
  info.annot_index = annot_index;
  FS_RECTF r{};
  if (FPDFAnnot_GetRect(annot, &r)) info.rect = space.to_rect(std::min(r.left, r.right), std::max(r.left, r.right),
                                                               std::min(r.bottom, r.top), std::max(r.bottom, r.top));
  info.value = form_string(FPDFAnnot_GetFormFieldValue, form, annot);
  if (info.type == FieldType::kText || info.type == FieldType::kComboBox) {
    // 여러 줄 값은 PDFium 이 "\r\n"·"\r" 로 둔다 → 내부 문자열은 "\n" 하나로 (Command·앱이 같은 값을 비교한다)
    std::string normalized;
    normalized.reserve(info.value.size());
    for (std::size_t i = 0; i < info.value.size(); ++i) {
      if (info.value[i] == '\r') {
        normalized.push_back('\n');
        if (i + 1 < info.value.size() && info.value[i + 1] == '\n') ++i;
      } else {
        normalized.push_back(info.value[i]);
      }
    }
    info.value = std::move(normalized);
  }
  info.export_value = form_string(FPDFAnnot_GetFormFieldExportValue, form, annot);
  info.checked = FPDFAnnot_IsChecked(form, annot);
  info.flags = std::max(0, FPDFAnnot_GetFormFieldFlags(form, annot));
  info.read_only = (info.flags & FPDF_FORMFLAG_READONLY) != 0;
  info.multiline = info.type == FieldType::kText && (info.flags & FPDF_FORMFLAG_TEXT_MULTILINE) != 0;
  info.password = info.type == FieldType::kText && (info.flags & FPDF_FORMFLAG_TEXT_PASSWORD) != 0;
  info.comb = info.type == FieldType::kText && (info.flags & kFormFlagComb) != 0;
  info.editable_combo = info.type == FieldType::kComboBox && (info.flags & FPDF_FORMFLAG_CHOICE_EDIT) != 0;
  info.multi_select = info.type == FieldType::kListBox && (info.flags & FPDF_FORMFLAG_CHOICE_MULTI_SELECT) != 0;
  if (info.type == FieldType::kComboBox || info.type == FieldType::kListBox) {
    const int count = std::max(0, FPDFAnnot_GetOptionCount(form, annot));
    for (int i = 0; i < count; ++i) {
      info.options.push_back(option_label(form, annot, i));
      if (FPDFAnnot_IsOptionSelected(form, annot, i)) info.selected.push_back(i);
    }
  }
  float size = 0;
  if (FPDFAnnot_GetFontSize(form, annot, &size) && size > 0) info.font_size = size;
  float max_len = 0;
  if (FPDFAnnot_GetNumberValue(annot, "MaxLen", &max_len) && max_len > 0) info.max_len = static_cast<int>(max_len);
  return info;
}

}  // namespace

std::vector<int> PdfiumDocument::pages_of_field(const std::string& name, int control_count, int page_index) const {
  if (control_count <= 1) return {page_index};
  if (!field_pages_ready_) {
    field_pages_.clear();
    FPDF_FORMHANDLE form = form_handle();
    for (int i = 0; form != nullptr && i < page_count(); ++i) {
      auto page = load_page(i);
      if (!page) continue;
      const int count = FPDFPage_GetAnnotCount(page->get());
      for (int a = 0; a < count; ++a) {
        ScopedAnnotation annot(FPDFPage_GetAnnot(page->get(), a));
        if (!annot || FPDFAnnot_GetSubtype(annot.get()) != FPDF_ANNOT_WIDGET) continue;
        auto& pages = field_pages_[form_string(FPDFAnnot_GetFormFieldName, form, annot.get())];
        if (pages.empty() || pages.back() != i) pages.push_back(i);
      }
    }
    field_pages_ready_ = true;
  }
  auto it = field_pages_.find(name);
  if (it == field_pages_.end() || it->second.empty()) return {page_index};
  return it->second;
}

Result<std::vector<FormWidgetInfo>> PdfiumDocument::form_widgets(int page_index) const {
  PdfiumLibrary::assert_current_thread();
  auto page = load_page(page_index);
  if (!page) return std::unexpected(page.error());
  std::vector<FormWidgetInfo> out;
  FPDF_FORMHANDLE form = form_handle();
  if (form == nullptr) return out;
  detail::FormPageView view(page->get(), form);
  const detail::PageSpace space = detail::PageSpace::of(page->get());
  const int count = FPDFPage_GetAnnotCount(page->get());
  std::unordered_map<std::string, int> seen;  // 이름 → 이 쪽에서 지금까지 나온 수 (find_widget 과 같은 순서)
  for (int i = 0; i < count; ++i) {
    ScopedAnnotation annot(FPDFPage_GetAnnot(page->get(), i));
    if (!annot || FPDFAnnot_GetSubtype(annot.get()) != FPDF_ANNOT_WIDGET) continue;
    const std::string name = form_string(FPDFAnnot_GetFormFieldName, form, annot.get());
    const int nth = seen[name]++;
    if (auto info = widget_info(form, annot.get(), space, i, nth)) out.push_back(std::move(*info));
  }
  return out;
}

namespace {

// 쪽에서 필드 이름 + 그 쪽 안의 순서로 위젯을 찾는다 (form_widgets 와 같은 주석 순서)
ScopedAnnotation find_widget(FPDF_PAGE page, FPDF_FORMHANDLE form, const FieldRef& ref) {
  const int count = FPDFPage_GetAnnotCount(page);
  int seen = 0;
  for (int i = 0; i < count; ++i) {
    ScopedAnnotation annot(FPDFPage_GetAnnot(page, i));
    if (!annot || FPDFAnnot_GetSubtype(annot.get()) != FPDF_ANNOT_WIDGET) continue;
    if (form_string(FPDFAnnot_GetFormFieldName, form, annot.get()) != ref.name) continue;
    if (seen++ == ref.nth) return annot;
  }
  return {};
}

}  // namespace

Result<std::vector<int>> PdfiumDocument::set_form_text(int page_index, const FieldRef& ref, std::string_view utf8) {
  PdfiumLibrary::assert_current_thread();
  FPDF_FORMHANDLE form = form_handle();
  if (form == nullptr) return make_error(ErrorCode::kUnsupported, "document has no form");
  auto page = load_page(page_index);
  if (!page) return std::unexpected(page.error());
  detail::FormPageView view(page->get(), form);
  ScopedAnnotation annot = find_widget(page->get(), form, ref);
  if (!annot) return make_error(ErrorCode::kInvalidArgument, "form field not found: " + ref.name);
  const auto info = widget_info(form, annot.get(), detail::PageSpace::of(page->get()), -1);
  if (!info || !(info->type == FieldType::kText || info->editable_combo)) {
    return make_error(ErrorCode::kInvalidArgument, "not a text field: " + ref.name);
  }
  if (info->read_only) return make_error(ErrorCode::kPermissionDenied, "read-only field: " + ref.name);
  if (!FORM_SetFocusedAnnot(form, annot.get())) return make_error(ErrorCode::kInvalidState, "FORM_SetFocusedAnnot failed");
  // 값 설정 경로 (#51 프로브): 전체 선택 → 바꾸기 → 포커스 해제가 JS 없이 값과 AP 를 확정한다
  FORM_SelectAllText(form, page->get());
  const std::vector<unsigned short> wide = detail::to_wide(std::string(utf8));
  if (info->type == FieldType::kText) {
    FORM_ReplaceSelection(form, page->get(), wide.data());
  } else {
    // 편집 가능 콤보는 ReplaceSelection 을 무시한다: 선택을 지우고 글자를 입력한다
    constexpr int kVkBack = 0x08;
    FORM_OnKeyDown(form, page->get(), kVkBack, 0);
    FORM_OnKeyUp(form, page->get(), kVkBack, 0);
    for (std::size_t i = 0; i + 1 < wide.size(); ++i) FORM_OnChar(form, page->get(), wide[i], 0);  // 끝 NUL 제외
  }
  if (!FORM_ForceToKillFocus(form)) return make_error(ErrorCode::kInvalidState, "FORM_ForceToKillFocus failed");
  return pages_of_field(ref.name, info->control_count, page_index);
}

Result<std::vector<int>> PdfiumDocument::set_form_checked(int page_index, const FieldRef& ref, bool checked) {
  PdfiumLibrary::assert_current_thread();
  FPDF_FORMHANDLE form = form_handle();
  if (form == nullptr) return make_error(ErrorCode::kUnsupported, "document has no form");
  auto page = load_page(page_index);
  if (!page) return std::unexpected(page.error());
  detail::FormPageView view(page->get(), form);
  ScopedAnnotation annot = find_widget(page->get(), form, ref);
  if (!annot) return make_error(ErrorCode::kInvalidArgument, "form field not found: " + ref.name);
  const int type = FPDFAnnot_GetFormFieldType(form, annot.get());
  if (type != FPDF_FORMFIELD_CHECKBOX && type != FPDF_FORMFIELD_RADIOBUTTON) {
    return make_error(ErrorCode::kInvalidArgument, "not a check box or radio button: " + ref.name);
  }
  if ((FPDFAnnot_GetFormFieldFlags(form, annot.get()) & FPDF_FORMFLAG_READONLY) != 0) {
    return make_error(ErrorCode::kPermissionDenied, "read-only field: " + ref.name);
  }
  const int control_count = std::max(1, FPDFAnnot_GetFormControlCount(form, annot.get()));
  if (static_cast<bool>(FPDFAnnot_IsChecked(form, annot.get())) == checked) return pages_of_field(ref.name, control_count, page_index);
  if (type == FPDF_FORMFIELD_RADIOBUTTON && !checked) {
    return make_error(ErrorCode::kUnsupported, "a radio button group cannot be cleared");
  }
  // 위젯 가운데를 누르고 뗀다 (체크박스는 토글, 라디오는 선택). 좌표는 사용자 좌표
  FS_RECTF r{};
  if (!FPDFAnnot_GetRect(annot.get(), &r)) return make_error(ErrorCode::kInvalidState, "FPDFAnnot_GetRect failed");
  const double x = (r.left + r.right) / 2, y = (r.bottom + r.top) / 2;
  FORM_OnLButtonDown(form, page->get(), 0, x, y);
  FORM_OnLButtonUp(form, page->get(), 0, x, y);
  (void)FORM_ForceToKillFocus(form);
  if (static_cast<bool>(FPDFAnnot_IsChecked(form, annot.get())) != checked) {
    return make_error(ErrorCode::kInvalidState, "check state did not change: " + ref.name);
  }
  return pages_of_field(ref.name, control_count, page_index);
}

Result<std::vector<int>> PdfiumDocument::set_form_selection(int page_index, const FieldRef& ref, const std::vector<int>& indices) {
  PdfiumLibrary::assert_current_thread();
  FPDF_FORMHANDLE form = form_handle();
  if (form == nullptr) return make_error(ErrorCode::kUnsupported, "document has no form");
  auto page = load_page(page_index);
  if (!page) return std::unexpected(page.error());
  detail::FormPageView view(page->get(), form);
  ScopedAnnotation annot = find_widget(page->get(), form, ref);
  if (!annot) return make_error(ErrorCode::kInvalidArgument, "form field not found: " + ref.name);
  const auto info = widget_info(form, annot.get(), detail::PageSpace::of(page->get()), -1);
  if (!info || (info->type != FieldType::kComboBox && info->type != FieldType::kListBox)) {
    return make_error(ErrorCode::kInvalidArgument, "not a choice field: " + ref.name);
  }
  if (info->read_only) return make_error(ErrorCode::kPermissionDenied, "read-only field: " + ref.name);
  const int count = static_cast<int>(info->options.size());
  std::vector<int> wanted(indices);
  std::sort(wanted.begin(), wanted.end());
  wanted.erase(std::unique(wanted.begin(), wanted.end()), wanted.end());
  if (std::any_of(wanted.begin(), wanted.end(), [&](int i) { return i < 0 || i >= count; })) {
    return make_error(ErrorCode::kInvalidArgument, "option index out of range");
  }
  if (!info->multi_select && wanted.size() > 1) return make_error(ErrorCode::kInvalidArgument, "field allows one selection");
  if (info->type == FieldType::kComboBox && wanted.empty()) return make_error(ErrorCode::kInvalidArgument, "combo box needs a selection");
  if (!FORM_SetFocusedAnnot(form, annot.get())) return make_error(ErrorCode::kInvalidState, "FORM_SetFocusedAnnot failed");
  if (info->type == FieldType::kComboBox || !info->multi_select) {
    // 하나만: 원하는 것 선택 (목록에서 비우기는 현재 선택 해제)
    if (wanted.empty()) {
      for (int i : info->selected) (void)FORM_SetIndexSelected(form, page->get(), i, false);
    } else if (!FORM_SetIndexSelected(form, page->get(), wanted.front(), true)) {
      (void)FORM_ForceToKillFocus(form);
      return make_error(ErrorCode::kInvalidState, "FORM_SetIndexSelected failed");
    }
  } else {
    for (int i = 0; i < count; ++i) {
      const bool want = std::binary_search(wanted.begin(), wanted.end(), i);
      if (static_cast<bool>(FPDFAnnot_IsOptionSelected(form, annot.get(), i)) != want) {
        (void)FORM_SetIndexSelected(form, page->get(), i, want);
      }
    }
  }
  if (!FORM_ForceToKillFocus(form)) return make_error(ErrorCode::kInvalidState, "FORM_ForceToKillFocus failed");
  return pages_of_field(ref.name, info->control_count, page_index);
}

Result<std::map<std::string, std::string>> PdfiumDocument::form_values() const {
  PdfiumLibrary::assert_current_thread();
  std::map<std::string, std::string> out;
  if (form_handle() == nullptr) return out;
  for (int i = 0; i < page_count(); ++i) {
    auto widgets = form_widgets(i);
    if (!widgets) return std::unexpected(widgets.error());
    for (const auto& w : *widgets) out[w.ref.name] = w.value;
  }
  return out;
}

// ---- 기존 텍스트 줄 수정 (#65) ----

namespace {

// FPDF_FILEWRITE → 메모리
struct MemoryWriter : FPDF_FILEWRITE {
  std::vector<std::uint8_t> bytes;
  static int write_block(FPDF_FILEWRITE* self, const void* data, unsigned long size) {
    auto* writer = static_cast<MemoryWriter*>(self);
    const auto* p = static_cast<const std::uint8_t*>(data);
    writer->bytes.insert(writer->bytes.end(), p, p + size);
    return 1;
  }
};

constexpr std::string_view kBoxPrefix = "onpdf-box-";

std::string base_font_name(FPDF_FONT font) {
  if (font == nullptr) return {};
  const std::size_t n = FPDFFont_GetBaseFontName(font, nullptr, 0);
  if (n == 0) return {};
  std::string name(n, '\0');
  FPDFFont_GetBaseFontName(font, name.data(), n);
  if (!name.empty() && name.back() == '\0') name.pop_back();
  if (name.size() > 7 && name[6] == '+') name.erase(0, 7);  // 서브셋 태그 ABCDEF+
  return name;
}

// Type3 는 글리프가 콘텐츠 스트림이라 글꼴 데이터가 없다 (비임베딩 Type1·TrueType 은 대체 글꼴 데이터라도 돌려준다).
// /BaseFont 가 있는 Type3 도 잡는다 (리뷰 I7)
bool is_type3(FPDF_FONT font) {
  if (font == nullptr) return false;
  std::size_t bytes = 0;
  const bool has_data = FPDFFont_GetFontData(font, nullptr, 0, &bytes) && bytes > 0;
  return !has_data;
}

TextLineInfo line_info_of(const detail::PageAnalysis& analysis, const detail::AnalyzedLine& line, const detail::PageSpace& space) {
  TextLineInfo info;
  info.id = detail::line_id_of(analysis, line);
  info.bbox = space.to_rect(line.left, line.right, line.bottom, line.top);
  info.text = detail::utf32_to_utf8(line.text);
  const auto& first = analysis.objects[static_cast<std::size_t>(line.members.front())];
  info.font_name = base_font_name(first.font);
  info.font_size = first.eff_size;
  info.color = {static_cast<std::uint8_t>(first.r), static_cast<std::uint8_t>(first.g), static_cast<std::uint8_t>(first.b), 255};
  info.font_embedded = first.font != nullptr && FPDFFont_GetIsEmbedded(first.font) == 1;
  info.font_type3 = is_type3(first.font);
  info.mixed_fonts = line.mixed_fonts;
  info.object_count = static_cast<int>(line.members.size());
  if (line.invisible) {
    info.editable = false;
    info.reason = "invisible";
  } else if (line.multi_line_object) {
    info.editable = false;
    info.reason = "multi-line-object";
  } else if (std::abs(line.dir_y) > 0.01 || line.dir_x < 0) {
    info.editable = false;
    info.reason = "vertical";
  }
  return info;
}

}  // namespace

Result<void> PdfiumDocument::prepare_for_save() const {
  if (FPDF_FORMHANDLE form = form_handle()) (void)FORM_ForceToKillFocus(form);  // 편집 중 값이 남지 않게 (#51)
  if (auto ensured = ensure_annotation_appearances(); !ensured) return std::unexpected(ensured.error());  // 주석 AP (#53)
  return {};
}

Result<std::vector<std::uint8_t>> PdfiumDocument::save_to_memory() const {
  PdfiumLibrary::assert_current_thread();
  if (auto prepared = prepare_for_save(); !prepared) return std::unexpected(prepared.error());
  MemoryWriter writer{};
  writer.version = 1;
  writer.WriteBlock = &MemoryWriter::write_block;
  if (!FPDF_SaveAsCopy(doc_.get(), &writer, FPDF_NO_INCREMENTAL)) return make_error(ErrorCode::kIoError, "FPDF_SaveAsCopy failed");
  return std::move(writer.bytes);
}

Result<std::vector<TextLineInfo>> PdfiumDocument::text_lines(int page_index) const {
  PdfiumLibrary::assert_current_thread();
  auto page = load_page(page_index);
  if (!page) return std::unexpected(page.error());
  ScopedTextPage text_page(FPDFText_LoadPage(page->get()));
  if (!text_page) return make_error(ErrorCode::kInvalidState, "FPDFText_LoadPage failed");
  const detail::PageAnalysis analysis = detail::analyze_page(page->get(), text_page.get());
  const detail::PageSpace space = detail::PageSpace::of(page->get());
  std::vector<TextLineInfo> out;
  for (const auto& line : analysis.lines) {
    if (line.members.empty()) continue;
    out.push_back(line_info_of(analysis, line, space));
  }
  return out;
}

Result<bool> PdfiumDocument::line_font_covers(int page_index, std::string_view line_id, std::string_view utf8) const {
  PdfiumLibrary::assert_current_thread();
  auto page = load_page(page_index);
  if (!page) return std::unexpected(page.error());
  FPDF_FONT font = nullptr;
  if (line_id.starts_with(kBoxPrefix)) {
    const int count = FPDFPage_CountObjects(page->get());
    for (int i = 0; i < count && font == nullptr; ++i) {
      FPDF_PAGEOBJECT obj = FPDFPage_GetObject(page->get(), i);
      if (obj != nullptr && FPDFPageObj_GetType(obj) == FPDF_PAGEOBJ_TEXT && mark_string(onpdf_mark_of(obj), "box") == line_id) {
        font = FPDFTextObj_GetFont(obj);
      }
    }
  } else {
    ScopedTextPage text_page(FPDFText_LoadPage(page->get()));
    if (!text_page) return make_error(ErrorCode::kInvalidState, "FPDFText_LoadPage failed");
    const detail::PageAnalysis analysis = detail::analyze_page(page->get(), text_page.get());
    for (const auto& line : analysis.lines) {
      if (!line.members.empty() && detail::line_id_of(analysis, line) == line_id) {
        font = analysis.objects[static_cast<std::size_t>(line.members.front())].font;
        break;
      }
    }
  }
  if (font == nullptr) return make_error(ErrorCode::kInvalidArgument, "line not found");
  if (FPDFFont_GetIsEmbedded(font) != 1 || is_type3(font)) return false;
  return detail::font_covers(font, detail::utf8_to_utf32(utf8));
}

Result<LineEditResult> PdfiumDocument::remove_text_line(int page_index, std::string_view line_id) {
  return edit_line(page_index, line_id, nullptr);
}

Result<LineEditResult> PdfiumDocument::replace_text_line(int page_index, std::string_view line_id, const LineReplacement& replacement) {
  if (replacement.utf8.empty()) return make_error(ErrorCode::kInvalidArgument, "empty replacement");
  return edit_line(page_index, line_id, &replacement);
}

Result<LineEditResult> PdfiumDocument::edit_line(int page_index, std::string_view line_id, const LineReplacement* replacement) {
  PdfiumLibrary::assert_current_thread();
  if (signature_count() > 0) return make_error(ErrorCode::kUnsupported, "signed document");
  for (const auto& [id, d] : detached_) {
    if (d.foreign) return make_error(ErrorCode::kInvalidState, "foreign detached object");
  }
  // ---- 1. 수술 전 바이트(떼어 둔 객체를 맨 뒤에 되돌려 포함) → 상태 토큰. 새 텍스트를 넣기 전 상태다.
  //         되돌린 객체가 분석에 보이도록 쪽 핸들은 그 뒤에 연다 (리뷰 W1) ----
  const bool onpdf_box = line_id.starts_with(kBoxPrefix);
  const bool needs_surgery = !onpdf_box;
  std::vector<ParkedDetached> parked;
  std::vector<std::uint8_t> token_bytes;
  const auto abort = [&](Result<LineEditResult> error) -> Result<LineEditResult> {
    if (needs_surgery) (void)unpark_detached(parked);
    return error;
  };
  if (needs_surgery) {
    auto p = park_detached();
    if (!p) return std::unexpected(p.error());
    parked = std::move(*p);
    auto bytes = save_to_memory();
    if (!bytes) return abort(std::unexpected(bytes.error()));
    token_bytes = std::move(*bytes);
  }
  auto page = load_page(page_index);
  if (!page) return abort(std::unexpected(page.error()));
  const detail::PageSpace space = detail::PageSpace::of(page->get());

  // ---- 2. 원래 줄 해석: OnPdf 상자(수술 없음) 또는 분석 줄(수술) ----
  std::vector<FPDF_PAGEOBJECT> old_objects;  // 지울 객체 (쪽 또는 Form 안)
  FPDF_FONT font = nullptr;
  float size = 0;
  unsigned r = 0, g = 0, b = 0, a = 255;
  std::array<double, 6> matrix{1, 0, 0, 1, 0, 0};
  Rect line_bbox;
  double line_eff_size = 0;
  if (onpdf_box) {
    const int count = FPDFPage_CountObjects(page->get());
    for (int i = 0; i < count; ++i) {
      FPDF_PAGEOBJECT obj = FPDFPage_GetObject(page->get(), i);
      if (obj == nullptr || FPDFPageObj_GetType(obj) != FPDF_PAGEOBJ_TEXT) continue;
      if (mark_string(onpdf_mark_of(obj), "box") != line_id) continue;
      old_objects.push_back(obj);
      if (font == nullptr) {
        font = FPDFTextObj_GetFont(obj);
        FPDFTextObj_GetFontSize(obj, &size);
        FPDFPageObj_GetFillColor(obj, &r, &g, &b, &a);
        FS_MATRIX m{};
        if (FPDFPageObj_GetMatrix(obj, &m)) matrix = {m.a, m.b, m.c, m.d, m.e, m.f};
        line_eff_size = size * std::hypot(matrix[2], matrix[3]);  // 유효 크기 (서식 변경의 환산 기준)
      }
    }
    if (old_objects.empty()) return make_error(ErrorCode::kInvalidArgument, "line not found");
  } else {
    ScopedTextPage text_page(FPDFText_LoadPage(page->get()));
    if (!text_page) return abort(make_error(ErrorCode::kInvalidState, "FPDFText_LoadPage failed"));
    const detail::PageAnalysis analysis = detail::analyze_page(page->get(), text_page.get());
    const detail::AnalyzedLine* target = nullptr;
    for (const auto& line : analysis.lines) {
      if (!line.members.empty() && detail::line_id_of(analysis, line) == line_id) {
        target = &line;
        break;
      }
    }
    if (target == nullptr) return abort(make_error(ErrorCode::kInvalidArgument, "line not found"));
    const TextLineInfo info = line_info_of(analysis, *target, space);
    if (!info.editable) return abort(make_error(ErrorCode::kUnsupported, info.reason));
    const auto& first = analysis.objects[static_cast<std::size_t>(target->members.front())];
    for (int k : target->members) old_objects.push_back(analysis.objects[static_cast<std::size_t>(k)].handle);
    font = first.font;
    size = first.size;
    r = first.r;
    g = first.g;
    b = first.b;
    a = first.a;
    matrix = first.matrix;
    line_bbox = info.bbox;
    line_eff_size = first.eff_size;
  }

  // ---- 3. 새 텍스트 (덧붙인 스트림에만 쓴다 — 원본 스트림을 더럽히지 않는다) ----
  LineEditResult result;
  std::vector<FPDF_PAGEOBJECT> inserted;
  const auto remove_inserted = [&] {
    for (FPDF_PAGEOBJECT obj : inserted) {
      if (FPDFPage_RemoveObject(page->get(), obj)) FPDFPageObj_Destroy(obj);
    }
    if (!inserted.empty()) (void)FPDFPage_GenerateContent(page->get());
    inserted.clear();
  };
  const auto fail = [&](Result<LineEditResult> error) -> Result<LineEditResult> {
    remove_inserted();
    if (needs_surgery) (void)unpark_detached(parked);
    return error;
  };

  if (replacement != nullptr) {
    const std::u32string chars = detail::utf8_to_utf32(replacement->utf8);
    const bool covers = font != nullptr && FPDFFont_GetIsEmbedded(font) == 1 && !is_type3(font) && detail::font_covers(font, chars);
    if (covers) {
      // 크기·색 변경(#69): 행렬(기준선·스케일)은 그대로 두고 객체 크기를 유효 크기 비율로 환산한다
      float new_size = size;
      if (replacement->size) {
        if (!(*replacement->size > 0.0)) return fail(make_error(ErrorCode::kInvalidArgument, "font size must be positive"));
        if (line_eff_size > 0.0) new_size = static_cast<float>(size * (*replacement->size / line_eff_size));
      }
      if (replacement->color) {
        r = replacement->color->r;
        g = replacement->color->g;
        b = replacement->color->b;
      }
      ScopedFPDFPageObject obj(FPDFPageObj_CreateTextObj(doc_.get(), font, new_size));
      if (!obj) return fail(make_error(ErrorCode::kInvalidState, "FPDFPageObj_CreateTextObj failed"));
      const std::vector<unsigned short> wide = detail::to_wide(replacement->utf8);
      if (!FPDFText_SetText(obj.get(), reinterpret_cast<FPDF_WIDESTRING>(wide.data()))) {
        return fail(make_error(ErrorCode::kInvalidState, "FPDFText_SetText failed"));
      }
      if (!FPDFPageObj_SetFillColor(obj.get(), r, g, b, a)) return fail(make_error(ErrorCode::kInvalidState, "FPDFPageObj_SetFillColor failed"));
      FPDFPageObj_Transform(obj.get(), matrix[0], matrix[1], matrix[2], matrix[3], matrix[4], matrix[5]);
      const std::string id = new_text_id();
      const std::string box = onpdf_box ? std::string(line_id) : new_box_id();
      FPDF_PAGEOBJECTMARK mark = FPDFPageObj_AddMark(obj.get(), "OnPdf");
      if (mark == nullptr) return fail(make_error(ErrorCode::kInvalidState, "FPDFPageObj_AddMark failed"));
      const auto* text_bytes = reinterpret_cast<const unsigned char*>(replacement->utf8.data());
      const auto text_len = static_cast<unsigned long>(replacement->utf8.size());
      if (!FPDFPageObjMark_SetStringParam(doc_.get(), obj.get(), mark, "id", id.c_str()) ||
          !FPDFPageObjMark_SetStringParam(doc_.get(), obj.get(), mark, "box", box.c_str()) ||
          !FPDFPageObjMark_SetStringParam(doc_.get(), obj.get(), mark, "ofont", "1") ||
          !FPDFPageObjMark_SetBlobParam(doc_.get(), obj.get(), mark, "text", text_bytes, text_len) ||
          !FPDFPageObjMark_SetBlobParam(doc_.get(), obj.get(), mark, "box_text", text_bytes, text_len)) {
        return fail(make_error(ErrorCode::kInvalidState, "cannot set marks"));
      }
      FPDF_PAGEOBJECT raw = obj.release();
      FPDFPage_InsertObject(page->get(), raw);
      inserted.push_back(raw);
      if (!FPDFPage_GenerateContent(page->get())) return fail(make_error(ErrorCode::kInvalidState, "FPDFPage_GenerateContent failed"));
      result.box = box;
      result.ids = {id};
    } else {
      if (!replacement->fallback) return fail(make_error(ErrorCode::kInvalidArgument, "original font cannot draw the text"));
      TextObjectsSpec spec = *replacement->fallback;
      if (onpdf_box) spec.box = std::string(line_id);
      auto added = add_text_objects_to(page->get(), spec);
      if (!added) return fail(std::unexpected(added.error()));
      result.used_fallback = true;
      result.box = added->box;
      result.ids = added->ids;
      for (const std::string& id : result.ids) {
        if (auto [obj, index] = find_page_object(page->get(), id); obj != nullptr) inserted.push_back(obj);
      }
    }
  }

  // ---- 4. OnPdf 상자: 수술 없이 옛 객체만 지운다 ----
  if (onpdf_box) {
    for (FPDF_PAGEOBJECT obj : old_objects) {
      if (FPDFPage_RemoveObject(page->get(), obj)) FPDFPageObj_Destroy(obj);
    }
    if (!FPDFPage_GenerateContent(page->get())) return make_error(ErrorCode::kInvalidState, "FPDFPage_GenerateContent failed");
    return result;
  }

  // ---- 5. 수술할 바이트(새 텍스트 포함)와 수술 전 렌더 (되돌린 객체가 보이는 상태 — 수술 뒤 렌더와 같은 조건) ----
  RenderOptions render;
  render.scale = 1.0f;
  render.render_annotations = false;
  auto before_render = render_page(page_index, render);
  if (!before_render) return fail(std::unexpected(before_render.error()));
  ShowOpSurgery ops;
  int page_text_objects = 0;
  std::vector<int> form_text_objects;
  {
    ScopedTextPage text_page(FPDFText_LoadPage(page->get()));
    if (!text_page) return fail(make_error(ErrorCode::kInvalidState, "FPDFText_LoadPage failed"));
    const detail::PageAnalysis analysis = detail::analyze_page(page->get(), text_page.get());
    page_text_objects = analysis.page_text_objects;
    form_text_objects = analysis.form_text_objects;
    for (FPDF_PAGEOBJECT old : old_objects) {
      const auto it = std::find_if(analysis.objects.begin(), analysis.objects.end(), [&](const auto& o) { return o.handle == old; });
      if (it == analysis.objects.end() || it->ordinal < 0) return fail(make_error(ErrorCode::kUnsupported, "line object has no show operator"));
      if (it->container < 0) ops.page_ordinals.insert(it->ordinal);
      else ops.form_ordinals[it->container].insert(it->ordinal);
    }
  }
  Result<SurgeryReport> report = [&]() -> Result<SurgeryReport> {  // 수술할 바이트(새 텍스트 포함)는 이 범위에서만 (리뷰 I6)
    auto current_bytes = save_to_memory();
    if (!current_bytes) return std::unexpected(current_bytes.error());
    return QpdfTools::remove_text_show_ops(*current_bytes, password_, page_index, ops);
  }();
  if (!report) return fail(std::unexpected(report.error()));
  bool consistent = report->page_show_ops == page_text_objects;
  std::string mismatch;
  if (!consistent) mismatch = " (page: qpdf " + std::to_string(report->page_show_ops) + " vs pdfium " + std::to_string(page_text_objects) + ")";
  for (const auto& [form, count] : report->form_show_ops) {
    if (form < 0 || form >= static_cast<int>(form_text_objects.size()) || form_text_objects[static_cast<std::size_t>(form)] != count) {
      consistent = false;
      mismatch += " (form " + std::to_string(form) + ")";
    }
  }
  if (!consistent || report->needs_displacement > 0 || report->shared_forms > 0) {
    return fail(make_error(ErrorCode::kUnsupported, !consistent        ? "content stream does not match page objects" + mismatch
                                                     : report->shared_forms ? "shared-form"
                                                                            : "displacement"));
  }

  // ---- 6. 다시 열기 → 줄 밖 보존 검증(되돌린 객체가 보이는 상태끼리) → 떼어 둔 객체 재분리 ----
  const DocumentStateId before = store_state(std::move(token_bytes));
  page->reset();  // 옛 문서의 쪽 핸들은 먼저 닫는다
  if (auto replaced = replace_bytes(std::move(report->bytes)); !replaced) {
    // 옛 문서가 그대로 살아 있다: 새 객체만 걷어 내고 떼어 둔 객체를 다시 뗀다
    if (auto p = load_page(page_index)) {
      for (const std::string& id : result.ids) {
        if (auto [obj, index] = find_page_object(p->get(), id); obj != nullptr && FPDFPage_RemoveObject(p->get(), obj)) FPDFPageObj_Destroy(obj);
      }
      (void)FPDFPage_GenerateContent(p->get());
    }
    (void)unpark_detached(parked);
    drop_document_state(before);
    return std::unexpected(replaced.error());
  }
  const auto rollback = [&](Result<LineEditResult> error) -> Result<LineEditResult> {
    // 토큰(되돌린 객체 포함)으로 복원한 뒤 다시 뗀다
    if (auto restored = replace_bytes(states_[before]); restored) (void)unpark_detached(parked);
    drop_document_state(before);
    return error;
  };
  auto after_render = render_page(page_index, render);
  if (!after_render) return rollback(std::unexpected(after_render.error()));
  // 줄 영역(여유 0.35em) 밖은 픽셀 하나도 달라지면 안 된다
  const double pad = std::max(2.0, 0.35 * line_eff_size);
  const int x0 = static_cast<int>(std::floor(line_bbox.x - pad)), y0 = static_cast<int>(std::floor(line_bbox.y - pad));
  const int x1 = static_cast<int>(std::ceil(line_bbox.right() + pad)), y1 = static_cast<int>(std::ceil(line_bbox.bottom() + pad));
  const Bitmap& A = *before_render;
  const Bitmap& B = *after_render;
  bool same = A.width == B.width && A.height == B.height;
  for (int y = 0; same && y < A.height; ++y) {
    for (int x = 0; x < A.width; ++x) {
      if (x >= x0 && x < x1 && y >= y0 && y < y1) continue;
      const std::uint8_t* pa = A.row(y) + x * 4;
      const std::uint8_t* pb = B.row(y) + x * 4;
      if (pa[0] != pb[0] || pa[1] != pb[1] || pa[2] != pb[2]) {
        same = false;
        break;
      }
    }
  }
  if (!same) return rollback(make_error(ErrorCode::kSaveVerifyFailed, "line edit changed other content"));
  if (auto unparked = unpark_detached(parked); !unparked) return rollback(std::unexpected(unparked.error()));
  result.before = before;
  return result;
}

Result<void> PdfiumDocument::replace_bytes(std::vector<std::uint8_t> bytes) {
  auto doc = load_document(bytes, password_);
  if (!doc) return std::unexpected(doc.error());
  // 옛 문서에 묶인 것부터 정리: 폼 환경 → 폰트 → 필드 지도 (떼어 둔 객체는 호출자가 park 로 비웠다)
  form_.reset();
  fonts_.clear();
  field_pages_.clear();
  field_pages_ready_ = false;
  for (auto& [id, d] : detached_) d.objects.clear();  // 혹시 남은 것은 옛 문서와 함께 버린다
  doc_.reset();
  data_ = std::move(bytes);
  doc_ = std::move(*doc);
  switch (FPDF_GetFormType(doc_.get())) {
    case FORMTYPE_ACRO_FORM: form_type_ = FormType::kAcroForm; break;
    case FORMTYPE_XFA_FULL: form_type_ = FormType::kXfaFull; break;
    case FORMTYPE_XFA_FOREGROUND: form_type_ = FormType::kXfaForeground; break;
    default: form_type_ = FormType::kNone; break;
  }
  form_ = detail::FormEnvironment::create(doc_.get());
  return {};
}

Result<std::vector<PdfiumDocument::ParkedDetached>> PdfiumDocument::park_detached() {
  std::vector<ParkedDetached> parked;
  std::vector<DetachedId> ids;
  for (const auto& [id, d] : detached_) ids.push_back(id);
  std::sort(ids.begin(), ids.end());
  for (DetachedId id : ids) {
    Detached& d = detached_[id];
    if (d.lost || d.objects.empty()) continue;  // 잃은 항목은 되돌릴 객체가 없다
    if (d.foreign) {
      (void)unpark_detached(parked);
      return make_error(ErrorCode::kInvalidState, "foreign detached object");
    }
    auto page = load_page(d.page);
    if (!page) {
      (void)unpark_detached(parked);
      return std::unexpected(page.error());
    }
    ParkedDetached record;
    record.id = id;
    record.page = d.page;
    // 맨 뒤에 덧붙인다: 중간에 끼우면(InsertObjectAtIndex) 이웃 객체의 원본 스트림을 물려받아 그 스트림이 재생성된다(손실).
    // 원래 index 는 기록해 두고 다시 뗄 때 되돌린다
    for (auto& [index, obj] : d.objects) {
      record.mark_ids.push_back(text_object_id_of(obj.get()));
      record.indices.push_back(index);
      FPDFPage_InsertObject(page->get(), obj.release());
    }
    d.objects.clear();  // 소유권은 쪽으로 갔다 — 실패해도 null 핸들을 남기지 않는다
    parked.push_back(std::move(record));
    if (!FPDFPage_GenerateContent(page->get())) {
      (void)unpark_detached(parked);
      return make_error(ErrorCode::kInvalidState, "FPDFPage_GenerateContent failed");
    }
  }
  return parked;
}

Result<void> PdfiumDocument::unpark_detached(const std::vector<ParkedDetached>& parked) {
  for (const ParkedDetached& record : parked) {
    auto page = load_page(record.page);
    if (!page) return std::unexpected(page.error());
    struct Found { int current = 0; int original = 0; FPDF_PAGEOBJECT obj = nullptr; };
    std::vector<Found> found;
    for (std::size_t k = 0; k < record.mark_ids.size(); ++k) {
      auto [obj, index] = find_page_object(page->get(), record.mark_ids[k]);
      if (obj == nullptr) return make_error(ErrorCode::kInvalidState, "detached object lost: " + record.mark_ids[k]);
      found.push_back({index, record.indices[k], obj});
    }
    std::sort(found.begin(), found.end(), [](const Found& x, const Found& y) { return x.current > y.current; });
    Detached d;
    d.page = record.page;
    for (const Found& f : found) {
      if (!FPDFPage_RemoveObject(page->get(), f.obj)) return make_error(ErrorCode::kInvalidState, "FPDFPage_RemoveObject failed");
      d.objects.emplace_back(f.original, ScopedFPDFPageObject(f.obj));
    }
    std::sort(d.objects.begin(), d.objects.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
    if (!FPDFPage_GenerateContent(page->get())) return make_error(ErrorCode::kInvalidState, "FPDFPage_GenerateContent failed");
    detached_[record.id] = std::move(d);
  }
  return {};
}

DocumentStateId PdfiumDocument::store_state(std::vector<std::uint8_t> bytes) {
  const DocumentStateId id = next_state_++;
  state_bytes_ += bytes.size();
  states_.emplace(id, std::move(bytes));
  state_order_.push_back(id);
  while (state_bytes_ > state_byte_limit_ && state_order_.size() > 1) {  // 방금 넣은 것은 남긴다
    const DocumentStateId oldest = state_order_.front();
    state_order_.pop_front();
    if (auto it = states_.find(oldest); it != states_.end()) {
      state_bytes_ -= it->second.size();
      states_.erase(it);
    }
  }
  return id;
}

Result<void> PdfiumDocument::restore_document_state(DocumentStateId id) {
  PdfiumLibrary::assert_current_thread();
  auto it = states_.find(id);
  if (it == states_.end()) return make_error(ErrorCode::kInvalidState, "undo history evicted");
  // 토큰 바이트에 없는 떼어 둔 객체(줄 수정 뒤에 추가했다가 뗀 것)는 되돌릴 곳이 없다 → lost 로 표시하고 되돌리지 않는다.
  // 그 객체의 reattach 는 거절된다 (리뷰 C1: 비어 있는 채로 성공을 보고하면 안 된다)
  std::vector<DetachedId> lost_ids;
  {
    auto scratch = load_document(it->second, password_);
    if (!scratch) return std::unexpected(scratch.error());
    for (auto& [detached_id, d] : detached_) {
      if (d.lost || d.objects.empty()) continue;
      ScopedPage page(FPDF_LoadPage(scratch->get(), d.page));
      bool present = page != nullptr;
      for (auto& [index, obj] : d.objects) {
        if (!present) break;
        const std::string mark = text_object_id_of(obj.get());
        present = !mark.empty() && find_page_object(page.get(), mark).first != nullptr;
      }
      if (!present) {
        d.lost = true;  // park 가 건너뛴다. 객체는 replace_bytes 가 옛 문서와 함께 버린다; 복원이 실패하면 되돌린다
        lost_ids.push_back(detached_id);
      }
    }
  }
  const auto revert_lost = [&] {
    for (DetachedId lost_id : lost_ids) {
      if (auto d = detached_.find(lost_id); d != detached_.end()) d->second.lost = false;
    }
  };
  auto parked = park_detached();
  if (!parked) {
    revert_lost();
    return std::unexpected(parked.error());
  }
  if (auto replaced = replace_bytes(it->second); !replaced) {
    (void)unpark_detached(*parked);
    revert_lost();
    return std::unexpected(replaced.error());
  }
  if (auto unparked = unpark_detached(*parked); !unparked) return std::unexpected(unparked.error());
  // 토큰 이후 버린(drop) 객체가 바이트에 살아 있으면 지운다 — 버린 쪽만 연다 (리뷰 I6)
  for (auto& [page_index, marks] : dropped_marks_) {
    if (page_index < 0 || page_index >= page_count()) continue;
    auto page = load_page(page_index);
    if (!page) continue;
    bool changed = false;
    for (int i = FPDFPage_CountObjects(page->get()) - 1; i >= 0; --i) {
      FPDF_PAGEOBJECT obj = FPDFPage_GetObject(page->get(), i);
      if (obj == nullptr) continue;
      const std::string mark = text_object_id_of(obj);
      if (!mark.empty() && marks.count(mark) != 0 && FPDFPage_RemoveObject(page->get(), obj)) {
        FPDFPageObj_Destroy(obj);
        changed = true;
      }
    }
    if (changed) (void)FPDFPage_GenerateContent(page->get());
  }
  return {};
}

void PdfiumDocument::drop_document_state(DocumentStateId id) noexcept {
  auto it = states_.find(id);
  if (it == states_.end()) return;
  state_bytes_ -= it->second.size();
  states_.erase(it);
  if (auto pos = std::find(state_order_.begin(), state_order_.end(), id); pos != state_order_.end()) state_order_.erase(pos);
}

}  // namespace onpdf::engine
