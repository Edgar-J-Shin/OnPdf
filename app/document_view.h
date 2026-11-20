#pragma once

// 연속 스크롤 뷰. 모든 쪽을 세로로 이어 붙여 타일 단위로 그린다.
// 레이아웃·캐시·스케줄러(core/view)를 소유하고, 타일은 DocumentTileSource 를 통해 워커에서 렌더된다.
//
// 좌표계
//  - 논리 px: Qt 위젯·스크롤바 (dpr 제외)
//  - 문서 px: core::PageLayout (dpr 포함 물리 px). 변환: 문서 = 논리 × dpr
//  - 타일 배율 scale_q = quantize(zoom × dpr)

#include "onpdf/core/engine/annotation.h"
#include "onpdf/core/engine/document_handle.h"
#include "onpdf/core/engine/form.h"
#include "onpdf/core/view/page_layout.h"
#include "onpdf/core/view/tile_cache.h"
#include "onpdf/core/view/search.h"
#include "onpdf/core/view/text_index.h"
#include "onpdf/core/view/text_selection.h"
#include "onpdf/core/view/tile_scheduler.h"
#include "ui_dispatcher.h"

#include <QAbstractScrollArea>
#include <QRegion>
#include <QElapsedTimer>
#include <QImage>
#include <QPoint>
#include <QPointer>
#include <QPointF>
#include <QString>
#include <QTimer>

#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace onpdf::app {

class DocumentTileSource;
class FormFieldEditor;

enum class ZoomMode { kFitWidth, kFitPage, kCustom };

// 주석 도구. kSelect 는 기존 동작(텍스트 선택·링크) + 주석 선택
enum class AnnotationTool { kSelect, kHighlight, kUnderline, kStrikeOut, kNote, kPen, kSquare, kCircle, kText, kImage };
constexpr int kAnnotationToolCount = 10;
static_assert(kAnnotationToolCount == static_cast<int>(AnnotationTool::kImage) + 1, "도구를 추가하면 수를 맞춘다");
// 마크업 요청: (쪽, 줄별 사각형(쪽 포인트))
using MarkupItems = std::vector<std::pair<int, std::vector<onpdf::Rect>>>;

class DocumentView : public QAbstractScrollArea {
  Q_OBJECT
 public:
  explicit DocumentView(QWidget* parent = nullptr);
  ~DocumentView() override;

  void set_document(const engine::DocumentHandle& handle, UiDispatcher& dispatcher);
  void clear_document(const QString& placeholder);
  // 편집으로 쪽 내용·크기가 바뀌었다: 그 쪽의 타일·텍스트를 버리고, 크기가 바뀌었으면 레이아웃을 새 쪽 크기로
  // 다시 만든다(보던 위치 유지). 진행 중인 이전 모양의 타일 요청은 스케줄러를 새로 만들어 버린다
  void invalidate_pages(const std::vector<int>& pages, bool sizes_changed);
  // 쪽 수·순서가 바뀌었다(삭제·이동·삽입): 타일·텍스트·선택을 전부 버리고 문서 정보의 새 쪽 크기로 레이아웃을
  // 다시 만든다. 배율은 유지하고 keep_page(새 범위로 클램프)의 맨 위로 간다. current_page_changed 를 낸다
  void rebuild_pages(int keep_page);
  [[nodiscard]] bool has_document() const noexcept { return layout_ != nullptr; }

  // ---- 주석 ----
  void set_tool(AnnotationTool tool);
  [[nodiscard]] AnnotationTool tool() const noexcept { return tool_; }
  // 현재 텍스트 선택을 마크업 항목으로 (인덱스가 없는 쪽은 빠진다). 비면 선택 없음
  [[nodiscard]] MarkupItems selection_markup_items() const;
  [[nodiscard]] const std::vector<engine::AnnotationInfo>* annotations(int page) const;  // 캐시에 있을 때만
  [[nodiscard]] std::optional<engine::AnnotationInfo> selected_annotation() const;
  [[nodiscard]] int selected_annotation_page() const noexcept { return selected_annot_ ? selected_annot_->page : -1; }
  void clear_annotation_selection();
  // ---- 텍스트 상자 (#47): OnPdf 가 넣은 텍스트 객체를 상자(마크 box) 단위로 고르고 끌어 옮긴다 ----
  // 선택한 쪽 객체 (#47 텍스트 상자 · #49 이미지). 텍스트는 box(상자 id) 로, 이미지는 id 로 가리킨다 (box = id)
  struct SelectedObject {
    engine::ObjectKind kind = engine::ObjectKind::kText;
    int page = -1;
    std::string box;               // 텍스트: 상자 id / 이미지: 객체 id (마크 없는 기존 이미지는 "#obj<n>")
    std::vector<std::string> ids;  // 텍스트: 줄 순서 / 이미지: {id}
    onpdf::Rect bbox;              // 쪽 포인트 (텍스트는 줄 bbox 합집합)
    double font_size = 0.0;        // 텍스트
    engine::Color color;
    int lines = 0;
    std::string text;              // 줄을 '\n' 으로 이은 원문
    int pixel_width = 0;           // 이미지
    int pixel_height = 0;
    std::string filter;
    bool marked = true;            // 이미지: 기존(다른 프로그램) 이미지면 false
  };
  using SelectedTextBox = SelectedObject;
  [[nodiscard]] const std::vector<engine::TextObjectInfo>* text_objects(int page) const;  // 캐시에 있을 때만
  [[nodiscard]] const std::vector<engine::PageObjectInfo>* page_objects(int page) const;   // 캐시에 있을 때만
  // 텍스트 상자가 선택돼 있을 때만 (이미지 선택은 selected_object)
  [[nodiscard]] std::optional<SelectedObject> selected_text_box() const {
    if (selected_text_ && selected_text_->kind == engine::ObjectKind::kText) return selected_text_;
    return std::nullopt;
  }
  [[nodiscard]] std::optional<SelectedObject> selected_object() const { return selected_text_; }
  // 문서에 원래 있던(마크 없는) 이미지는 선택만 된다 — 이동·크기·삭제는 그 스트림 전체를 PDFium 이 다시 써
  // Type3·자간·선 폭·CMYK 등을 잃으므로 막는다 (#57, qpdf 수술로 바꿀 때까지)
  [[nodiscard]] bool selected_object_locked() const {
    return selected_text_ && selected_text_->kind == engine::ObjectKind::kImage && !selected_text_->marked;
  }
  // 편집(추가) 뒤 새 목록이 오면 이 객체를 고른다 (이미지 추가 직후 선택)
  void select_after_refresh(int page, const std::string& key);
  void clear_text_box_selection();

  // ---- 폼 채우기 (#51) ----
  [[nodiscard]] const std::vector<engine::FormWidgetInfo>* form_widgets(int page) const;
  // 채울 수 있을 때(권한) 칸을 옅게 강조한다. 클릭 신호는 권한과 무관하게 보낸다 (수신자가 안내)
  void set_form_fillable(bool fillable);
  [[nodiscard]] bool form_fillable() const noexcept { return form_fillable_; }
  // 텍스트 칸 위에 오버레이 편집기를 연다 (열려 있던 편집기는 먼저 확정)
  void open_form_editor(int page, const engine::FormWidgetInfo& widget);
  void commit_form_editor();  // 열려 있으면 확정 (저장·다른 문서 열기·인쇄 전에)
  void cancel_form_editor();
  [[nodiscard]] FormFieldEditor* form_editor() const;
  [[nodiscard]] QRectF page_rect_to_viewport(int page, const onpdf::Rect& r) const;
  // 내용 수정 권한·바쁨에 따라 상자 이동 드래그·커서를 막는다 (선택·정보 표시는 그대로). 썸네일 set_editable 과 같은 역할
  void set_text_editable(bool editable);
  // 화살표 이동은 권한만 본다: 바쁜 동안 누른 키는 MainWindow 가 모아 두었다가 실행한다 (#49)
  void set_nudge_enabled(bool enabled) { nudge_enabled_ = enabled; }
  [[nodiscard]] bool text_editable() const noexcept { return text_editable_; }

  void set_zoom_mode(ZoomMode mode);
  [[nodiscard]] ZoomMode zoom_mode() const noexcept { return mode_; }
  // 논리 배율(px/pt). anchor 는 뷰포트 논리 좌표; 그 아래 문서 지점이 고정된다
  void set_zoom(double zoom, QPointF anchor_viewport);
  void set_zoom(double zoom);  // 뷰포트 중앙 앵커
  void zoom_in();
  void zoom_out();
  [[nodiscard]] double zoom() const noexcept { return zoom_; }

  void go_to_page(int page);
  // 쪽 포인트 좌표(좌상단 원점)가 뷰포트 상단에 오도록 스크롤 (링크 /XYZ 이동)
  void scroll_to_page_point(int page, onpdf::Point pt);
  [[nodiscard]] int current_page() const;  // 뷰포트 세로 중앙이 속한 쪽, 문서 없으면 -1
  [[nodiscard]] int page_count() const;

  // ---- 텍스트 선택·링크 ----
  [[nodiscard]] bool has_selection() const noexcept { return !selection_.empty(); }
  // 미로드 쪽은 동기 로드(상한 200쪽). 상한을 넘겨 잘렸으면 *truncated = true
  [[nodiscard]] QString selected_text(bool* truncated = nullptr) const;
  void select_all_on_current_page();
  void clear_selection();
  [[nodiscard]] const view::PageTextIndex* text_index(int page) const;  // 캐시에 있을 때만
  [[nodiscard]] const view::TextSelection& selection() const noexcept { return selection_; }

  // ---- 검색 ----
  // 하이라이트할 검색 세션 (비소유). nullptr 이면 검색 하이라이트 없음
  void set_search(const view::SearchSession* search);
  // 사각형(쪽 포인트)이 뷰포트 밖이면 뷰포트 세로 1/3 지점에 오도록 스크롤
  void reveal(int page, const std::vector<onpdf::Rect>& rects);
  // 테스트·도구용: 쪽 포인트 → 뷰포트 논리 좌표 (쪽이 뷰포트 밖이어도 계산)
  [[nodiscard]] QPointF page_point_to_viewport(int page, onpdf::Point pt) const;
  [[nodiscard]] std::optional<std::pair<int, onpdf::Point>> page_point_at(QPointF viewport_pos) const;
  // 읽는 위치: 뷰포트 상단 여백 바로 아래의 (쪽, 쪽 포인트). 쪽 사이 간격이면 다음 쪽 맨 위.
  // go_to_page·scroll_to_page_point 직후에는 그 대상 지점이 된다 (목차 현재 항목 판정용)
  [[nodiscard]] std::optional<std::pair<int, onpdf::Point>> reading_position() const;

  [[nodiscard]] view::TileCache::Stats cache_stats() const { return cache_.stats(); }
  [[nodiscard]] std::vector<view::TileKey> cache_keys() const { return cache_.keys(); }
  [[nodiscard]] view::TileScheduler::Stats scheduler_stats() const;
  [[nodiscard]] int pending_visible_tiles() const;

  static constexpr double kMinZoom = 0.25;
  static constexpr double kMaxZoom = 8.0;

 signals:
  void current_page_changed(int page);
  void zoom_changed(double zoom);
  // 보이는 타일이 전부 도착한 최초 시점 (문서마다 한 번). 스크린샷·테스트용
  void first_view_rendered();
  void tiles_changed();  // 타일이 캐시에 들어왔다 (통계 갱신용, 자주 온다)
  void selection_changed(bool has_selection);
  void text_index_ready(int page);
  void link_activated(const onpdf::view::Link& link);   // 안전 검사 전. 수신자가 is_safe_uri 로 판단
  void link_hovered(const QString& target);             // 빈 문자열 = 호버 해제
  // ---- 주석 (MainWindow 가 Command 로 바꾼다) ----
  void markup_requested(onpdf::engine::AnnotationKind kind, const onpdf::app::MarkupItems& items);
  void note_requested(int page, onpdf::Point pt);  // 쪽 포인트 (쪽 안으로 클램프됨)
  void text_requested(int page, onpdf::Point pt);  // 텍스트 상자 왼쪽 위 (쪽 포인트, 클램프됨) — #45
  void ink_requested(int page, const std::vector<onpdf::Point>& points, onpdf::Rect bbox);
  void shape_requested(int page, onpdf::engine::AnnotationKind kind, onpdf::Rect rect);  // 쪽 포인트, 4pt 이상
  void annotation_selected(bool has, const onpdf::engine::AnnotationInfo& info);
  // 편집 뒤 같은 NM 을 다시 골랐다 (사용자 조작이 아니므로 상태 표시줄 안내를 덮지 않는다)
  void annotation_reselected(const onpdf::engine::AnnotationInfo& info);
  void annotation_delete_requested(int page, const QString& id);
  // ---- 텍스트 상자 (#47) ----
  void text_box_selected(bool has, const SelectedTextBox& box);
  void text_box_reselected(const SelectedTextBox& box);  // 편집 뒤 같은 상자를 조용히 다시 골랐다
  void text_move_requested(int page, const std::vector<std::string>& ids, onpdf::Point delta);  // 쪽 포인트
  void text_delete_requested(int page, const QString& box);
  void text_edit_requested(int page, const SelectedTextBox& box);  // 두 번 클릭
  void image_requested(int page, onpdf::Point pt);                   // 이미지 도구 클릭 (쪽 포인트, 클램프됨) — #49
  void object_resize_requested(int page, const QString& id, onpdf::Rect before, onpdf::Rect after);  // 이미지 핸들
  void text_width_requested(int page, const QString& box, double left_x, double width);              // 텍스트 상자 폭 핸들
  void object_nudge_requested(int page, const std::vector<std::string>& ids, onpdf::Point delta);    // 화살표 키 (쪽 포인트)
  void locked_object_edit_attempted();  // 기존 이미지를 옮기거나 지우려 했다 (#57) — 수신자가 안내
  void annotation_edit_requested(int page, const onpdf::engine::AnnotationInfo& info);
  void form_field_clicked(int page, const onpdf::engine::FormWidgetInfo& widget);              // 선택 도구로 칸 클릭 (#51)
  void form_text_committed(int page, const onpdf::engine::FormWidgetInfo& widget, const QString& text);  // 오버레이 확정

 protected:
  void paintEvent(QPaintEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
  void scrollContentsBy(int dx, int dy) override;
  void wheelEvent(QWheelEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void mouseDoubleClickEvent(QMouseEvent* event) override;
  void leaveEvent(QEvent* event) override;
  bool viewportEvent(QEvent* event) override;  // 핀치(NativeGesture)는 viewport 로 온다
  bool event(QEvent* event) override;          // DPR 변경

 private:
  [[nodiscard]] double dpr() const;
  [[nodiscard]] onpdf::Rect viewport_doc() const;      // 문서 px
  [[nodiscard]] double horizontal_offset_logical() const;  // 문서가 뷰보다 좁을 때 가운데 정렬 오프셋
  void relayout();                                       // 배율 계산 → 스크롤바 → 스케줄러
  void apply_scale_q(int scale_q);
  void update_scrollbars();
  void schedule_update();
  void paint_page(QPainter& painter, int page, const QRectF& page_logical);
  void on_tile_ready(const view::TileKey& key);
  void emit_page_if_changed();
  // 쪽 이동 명령의 공통 끝: 목표 스크롤 값을 자르고 "이동 고정 쪽" 으로 기억한 뒤 스크롤한다
  void scroll_to_anchored(int page, double value_logical);
  // 드래그 위치 → 글자 위치. 그 쪽 텍스트 인덱스가 아직 없으면 pending 에 쪽 포인트를 보관한다
  view::TextPos drag_position(QPointF viewport_pos, std::optional<std::pair<int, onpdf::Point>>& pending);
  // 인덱스가 도착한 쪽의 보류된 드래그 지점을 실제 글자 위치로 바꾼다
  void resolve_pending_drag(int page, const view::PageTextIndex& index);
  // 보이는 쪽의 선택 사각형이 차지하는 뷰포트 영역 (부분 다시 그리기용)
  [[nodiscard]] QRegion selection_region() const;
  // 텍스트
  void request_text_indexes();
  void on_text_index(int page, int generation, std::shared_ptr<const view::PageTextIndex> index, bool cancelled);
  void paint_selection(QPainter& painter, int page, const QRectF& page_logical);
  void paint_search(QPainter& painter, int page, const QRectF& page_logical);
  void paint_annotations(QPainter& painter, int page, const QRectF& page_logical);  // 선택 테두리·펜 미리보기
  void request_annotations();  // 텍스트 인덱스와 같은 범위의 쪽 주석 목록
  void on_annotations(int page, int generation, std::shared_ptr<const std::vector<engine::AnnotationInfo>> list,
                      bool cancelled);
  [[nodiscard]] const engine::AnnotationInfo* annotation_at(int page, onpdf::Point pt) const;  // 위에 그려진 것 먼저
  void request_text_boxes();  // 주석과 같은 범위의 텍스트 상자 목록
  void on_text_boxes(int page, int generation, std::shared_ptr<const std::vector<engine::PageObjectInfo>> list, bool cancelled);
  [[nodiscard]] std::string text_box_id_at(int page, onpdf::Point pt) const;  // 뒤(위)에 있는 것 먼저. 없으면 빈 문자열
  [[nodiscard]] std::optional<SelectedTextBox> text_box_at(int page, onpdf::Point pt) const;
  [[nodiscard]] std::optional<SelectedTextBox> text_box_by_id(int page, const std::string& box) const;
  void cancel_text_drag();
  [[nodiscard]] onpdf::Point clamp_to_page(int page, onpdf::Point pt) const;
  [[nodiscard]] onpdf::Point page_point_on(int page, QPointF viewport_pos) const;  // 특정 쪽 좌표로 투영 (쪽 밖도 음수·초과로)
  void update_page(int page);  // 그 쪽 영역만 다시 그린다
  [[nodiscard]] view::TextPos position_at(QPointF viewport_pos) const;  // 가장 가까운 선택 위치 (쪽 밖은 clamp)
  void update_hover(QPointF viewport_pos);
  void set_selection(view::TextPos anchor, view::TextPos focus);
  void autoscroll_step();

  std::unique_ptr<view::PageLayout> layout_;
  view::TileCache cache_;
  std::unique_ptr<DocumentTileSource> source_;
  std::unique_ptr<view::TileScheduler> scheduler_;
  ZoomMode mode_ = ZoomMode::kFitWidth;
  double zoom_ = 1.0;
  int last_page_ = -1;
  // 이동 고정 쪽: go_to_page·scroll_to_page_point 직후 스크롤 값이 그대로인 동안 현재 쪽으로 쓴다.
  // 짧은 쪽(명함)이나 문서 끝처럼 뷰포트 중앙이 다른 쪽에 걸려도 사용자가 고른 쪽이 유지된다
  struct NavAnchor {
    int page = 0;
    int scroll = 0;
  };
  std::optional<NavAnchor> nav_anchor_;
  // 텍스트 인덱스가 없는 쪽에서 시작·이동한 드래그 지점 (쪽, 쪽 포인트)
  std::optional<std::pair<int, onpdf::Point>> pending_anchor_;
  std::optional<std::pair<int, onpdf::Point>> pending_focus_;
  bool first_view_pending_ = false;
  QString placeholder_;
  QTimer resize_timer_;

  // 텍스트 인덱스·선택 (메인 스레드 전용)
  engine::DocumentHandle handle_;
  UiDispatcher* dispatcher_ = nullptr;
  int doc_generation_ = 0;
  std::unordered_map<int, std::shared_ptr<const view::PageTextIndex>> text_cache_;
  std::unordered_map<int, engine::TaskHandle> text_requested_;  // 쪽 → 취소 핸들
  view::TextSelection selection_;
  enum class Drag { kNone, kText, kMaybeLink, kInk, kShape, kMoveText, kResize } drag_ = Drag::kNone;
  // 주석 (메인 스레드 전용)
  AnnotationTool tool_ = AnnotationTool::kSelect;
  std::unordered_map<int, std::shared_ptr<const std::vector<engine::AnnotationInfo>>> annot_cache_;
  std::unordered_map<int, engine::TaskHandle> annot_requested_;
  struct SelectedAnnotation {
    int page = -1;
    engine::AnnotationInfo info;
  };
  std::optional<SelectedAnnotation> selected_annot_;
  int reselect_page_ = -1;    // 편집으로 목록이 바뀐 뒤 같은 NM 을 다시 고른다 (속성을 연달아 바꿀 수 있게)
  std::string reselect_id_;
  int ink_page_ = -1;
  std::vector<onpdf::Point> ink_points_;  // 그리는 중인 펜 스트로크 (쪽 포인트)
  // 텍스트 상자 (#47)
  struct ObjectCache {
    std::vector<engine::PageObjectInfo> objects;  // 쪽 객체 순서 (z)
    std::vector<engine::TextObjectInfo> texts;    // 텍스트만 (기존 접근자 text_objects 용)
  };
  std::unordered_map<int, std::shared_ptr<const ObjectCache>> text_box_cache_;
  int resize_handle_ = -1;           // 이미지: 0 좌상 · 1 우상 · 2 좌하 · 3 우하 / 텍스트 상자: 4 왼쪽 · 5 오른쪽 (#49)
  onpdf::Rect resize_preview_{};     // 크기 조절 미리보기 (쪽 포인트)
  [[nodiscard]] int handle_at(QPointF viewport_pos) const;  // 선택한 이미지의 핸들 (없으면 -1)
  [[nodiscard]] onpdf::Rect resized_rect(onpdf::Point pt, bool keep_aspect) const;
  std::unordered_map<int, engine::TaskHandle> text_box_requested_;
  std::optional<SelectedTextBox> selected_text_;
  bool text_editable_ = false;
  bool nudge_enabled_ = false;
  int reselect_text_page_ = -1;
  std::string reselect_text_box_;
  bool reselect_announce_ = false;  // select_after_refresh: 조용한 재선택이 아니라 새 선택으로 알린다 (상태 표시줄)
  onpdf::Point move_start_pt_{};  // 이동 드래그 시작점 (쪽 포인트)
  onpdf::Point move_delta_{};
  int shape_page_ = -1;                   // 그리는 중인 도형 (쪽 포인트)
  onpdf::Point shape_anchor_{}, shape_corner_{};
  [[nodiscard]] onpdf::Rect shape_rect() const noexcept;
  void cancel_shape_drag();
  QPointF press_pos_;
  std::optional<view::Link> pressed_link_;
  QString hovered_target_;
  QTimer autoscroll_timer_;
  QPointF last_drag_pos_;
  QElapsedTimer click_timer_;
  int click_count_ = 0;

  // 폼 (#51): 보이는 쪽 ±1 위젯 스냅샷, 오버레이 편집기, Tab 이동 대상
  void request_form_widgets();
  void on_form_widgets(int page, int generation, std::shared_ptr<const std::vector<engine::FormWidgetInfo>> list,
                       bool cancelled);
  [[nodiscard]] const engine::FormWidgetInfo* widget_at(int page, onpdf::Point pt) const;
  void paint_form(QPainter& painter, int page, const QRectF& page_logical);
  void place_form_editor();
  void on_form_editor_done(bool commit, int advance);
  void go_to_adjacent_text_widget(int page, const engine::FieldRef& from, bool backward);
  void open_pending_form_editor();
  std::unordered_map<int, std::shared_ptr<const std::vector<engine::FormWidgetInfo>>> form_cache_;
  std::unordered_map<int, engine::TaskHandle> form_requested_;
  bool form_fillable_ = false;
  QPointer<FormFieldEditor> form_editor_;
  struct PendingFormOpen {
    int page = -1;
    bool backward = false;  // 그 쪽의 마지막(true)·첫(false) 텍스트 칸
  };
  std::optional<PendingFormOpen> pending_form_open_;
  QPointF last_click_pos_;
  static constexpr int kTextKeepPages = 5;
  const view::SearchSession* search_ = nullptr;
};

}  // namespace onpdf::app
