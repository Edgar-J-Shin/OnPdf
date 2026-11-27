#pragma once

// 설정 대화상자 (#129): 일반·편집·고급 탭. 확인·적용 때만 Settings 에 쓴다. "기본값으로" 는 위젯만
// 기본값으로 되돌린다(확인해야 저장). 언어가 바뀌면 language_changed() — 실행 중 재번역은 하지
// 않으므로 창이 재시작 안내를 띄운다. objectName 은 테스트용(settings_*)

#include "settings.h"
#include <QDialog>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QSpinBox;

namespace onpdf::app {

class SettingsDialog : public QDialog {
  Q_OBJECT

 public:
  SettingsDialog(Settings& settings, QWidget* parent = nullptr);

  void load();           // Settings → 위젯
  void load_defaults();  // 기본값 → 위젯
  void apply();          // 위젯 → Settings (+ sync)

 signals:
  void language_changed();

 private:
  void fill_color_combo(QComboBox* combo, engine::Color current, bool offer_black);
  [[nodiscard]] static engine::Color combo_color(const QComboBox* combo, engine::Color fallback);
  static void select_data(QComboBox* combo, const QVariant& data);

  Settings& settings_;
  QComboBox* language_ = nullptr;
  QComboBox* start_zoom_ = nullptr;
  QComboBox* sidebar_ = nullptr;
  QComboBox* border_width_ = nullptr;
  QComboBox* opacity_ = nullptr;
  QComboBox* annotation_color_ = nullptr;
  QComboBox* text_size_ = nullptr;
  QComboBox* text_color_ = nullptr;
  QLineEdit* author_ = nullptr;
  QSpinBox* cache_mb_ = nullptr;
  QCheckBox* hazard_notice_ = nullptr;
  QSpinBox* stall_seconds_ = nullptr;
};

}  // namespace onpdf::app
