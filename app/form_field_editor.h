#pragma once

// 폼 텍스트 칸 오버레이 편집기 (#51). 칸 위에 Qt 입력 위젯을 띄우고 확정 때 한 번만 문서를 바꾼다 —
// IME(한글 조합)·커서·선택·복사는 Qt 가 맡는다. 확정: Enter(한 줄)·Ctrl/Cmd+Enter(여러
// 줄)·Tab·포커스 잃음, 취소: Esc

#include "onpdf/core/engine/form.h"

#include <QFrame>

class QLineEdit;
class QPlainTextEdit;

namespace onpdf::app {

class FormFieldEditor final : public QFrame {
  Q_OBJECT
 public:
  FormFieldEditor(int page, engine::FormWidgetInfo widget, QWidget* parent);

  // 칸 사각형(뷰포트 논리 px)과 포인트당 논리 px 로 위치·글꼴 크기를 맞춘다
  void place(const QRectF& viewport_rect, double px_per_pt);
  void focus_editor();
  [[nodiscard]] QString text() const;
  void set_text(const QString& text);  // 테스트·프로그램 입력
  [[nodiscard]] int page() const noexcept { return page_; }
  [[nodiscard]] const engine::FormWidgetInfo& widget() const noexcept { return widget_; }
  [[nodiscard]] bool finished() const noexcept { return finished_; }
  // 확정 (advance: 0 = 머무름, 1 = 다음 칸, -1 = 이전 칸). 한 번만 신호를 낸다
  void commit(int advance = 0);
  void cancel();

 signals:
  void committed(int advance);
  void cancelled();

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  int page_;
  engine::FormWidgetInfo widget_;
  QLineEdit* line_ = nullptr;
  QPlainTextEdit* multi_ = nullptr;
  bool finished_ = false;
};

}  // namespace onpdf::app
