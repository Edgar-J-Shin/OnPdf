#include "onpdf/core/engine/pdfium_document.h"

#include "onpdf/core/engine/pdfium_library.h"
#include "dest.h"
#include "onpdf/core/engine/text_page.h"
#include "onpdf/core/platform/path.h"

#include <fpdf_doc.h>
#include <fpdf_signature.h>
#include <fpdf_save.h>
#include <fpdf_edit.h>
#include <fpdf_ppo.h>
#include <fpdfview.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
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
    : data_(std::move(data)), doc_(std::move(doc)) {}

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
  return {};
}

void PdfiumDocument::drop_snapshot(SnapshotId id) noexcept {
  PdfiumLibrary::assert_current_thread();
  snapshots_.erase(id);
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

Result<void> PdfiumDocument::save_to(const std::filesystem::path& path, bool incremental) const {
  PdfiumLibrary::assert_current_thread();
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return make_error(ErrorCode::kIoError, "cannot create: " + path_to_utf8(path));
  FileWriter writer{};
  writer.version = 1;
  writer.WriteBlock = &FileWriter::write_block;
  writer.out = &out;
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

}  // namespace onpdf::engine
