#include "settings_dialog.h"

#include "main_window.h"
#include "palette.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QPixmap>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QVariant>
#include <QWidget>

namespace onpdf::app {

namespace {

QIcon color_swatch(engine::Color color) {
  QPixmap pm(16, 16);
  pm.fill(QColor(color.r, color.g, color.b));
  return QIcon(pm);
}

}  // namespace

SettingsDialog::SettingsDialog(Settings& settings, QWidget* parent)
    : QDialog(parent), settings_(settings) {
  setObjectName(QStringLiteral("settings_dialog"));
  setWindowTitle(tr("Preferences"));
  setModal(true);

  auto* tabs = new QTabWidget(this);

  // ---- 일반 ----
  auto* general = new QWidget(tabs);
  auto* general_form = new QFormLayout(general);
  language_ = new QComboBox(general);
  language_->setObjectName(QStringLiteral("settings_language"));
  language_->addItem(tr("System default"), static_cast<int>(LanguageChoice::kSystem));
  // 언어 이름은 그 언어로 적는다 (번역하지 않는다)
  language_->addItem(QStringLiteral("한국어"), static_cast<int>(LanguageChoice::kKorean));
  language_->addItem(QStringLiteral("English"), static_cast<int>(LanguageChoice::kEnglish));
  general_form->addRow(tr("Language:"), language_);
  start_zoom_ = new QComboBox(general);
  start_zoom_->setObjectName(QStringLiteral("settings_start_zoom"));
  start_zoom_->addItem(tr("Fit width"), static_cast<int>(StartZoom::kFitWidth));
  start_zoom_->addItem(tr("Fit page"), static_cast<int>(StartZoom::kFitPage));
  start_zoom_->addItem(tr("Actual size"), static_cast<int>(StartZoom::kActualSize));
  start_zoom_->addItem(tr("Last used"), static_cast<int>(StartZoom::kLast));
  general_form->addRow(tr("Zoom when opening:"), start_zoom_);
  sidebar_ = new QComboBox(general);
  sidebar_->setObjectName(QStringLiteral("settings_sidebar"));
  sidebar_->addItem(tr("Show"), static_cast<int>(SidebarStart::kVisible));
  sidebar_->addItem(tr("Hide"), static_cast<int>(SidebarStart::kHidden));
  sidebar_->addItem(tr("Remember"), static_cast<int>(SidebarStart::kLast));
  general_form->addRow(tr("Sidebar at startup:"), sidebar_);
  tabs->addTab(general, tr("General"));

  // ---- 편집 ----
  auto* editing = new QWidget(tabs);
  auto* editing_form = new QFormLayout(editing);
  border_width_ = new QComboBox(editing);
  border_width_->setObjectName(QStringLiteral("settings_border_width"));
  for (double w : {1.0, 2.0, 4.0, 8.0}) border_width_->addItem(tr("%1 pt").arg(w), w);
  editing_form->addRow(tr("Line width (shapes and pen):"), border_width_);
  opacity_ = new QComboBox(editing);
  opacity_->setObjectName(QStringLiteral("settings_opacity"));
  for (int p : {25, 50, 75, 100}) opacity_->addItem(tr("%1%").arg(p), p);
  editing_form->addRow(tr("Opacity:"), opacity_);
  annotation_color_ = new QComboBox(editing);
  annotation_color_->setObjectName(QStringLiteral("settings_annotation_color"));
  editing_form->addRow(tr("Annotation color:"), annotation_color_);
  text_size_ = new QDoubleSpinBox(editing);  // 직접 입력 (#141)
  text_size_->setObjectName(QStringLiteral("settings_text_size"));
  text_size_->setRange(Settings::kMinTextSize, Settings::kMaxTextSize);
  text_size_->setDecimals(1);
  text_size_->setSingleStep(1.0);
  text_size_->setSuffix(tr(" pt"));
  editing_form->addRow(tr("Text size:"), text_size_);
  text_color_ = new QComboBox(editing);
  text_color_->setObjectName(QStringLiteral("settings_text_color"));
  editing_form->addRow(tr("Text color:"), text_color_);
  author_ = new QLineEdit(editing);
  author_->setObjectName(QStringLiteral("settings_author"));
  author_->setMaxLength(256);
  author_->setPlaceholderText(tr("Written into new annotations (leave empty to omit)"));
  editing_form->addRow(tr("Author name:"), author_);
  tabs->addTab(editing, tr("Editing"));

  // ---- 고급 ----
  auto* advanced = new QWidget(tabs);
  auto* advanced_form = new QFormLayout(advanced);
  cache_mb_ = new QSpinBox(advanced);
  cache_mb_->setObjectName(QStringLiteral("settings_cache_mb"));
  cache_mb_->setRange(Settings::kMinCacheMb, Settings::kMaxCacheMb);
  cache_mb_->setSingleStep(64);
  cache_mb_->setSuffix(tr(" MB"));
  advanced_form->addRow(tr("Page cache limit:"), cache_mb_);
  hazard_notice_ = new QCheckBox(tr("Show a notice when a document has pages that cannot be "
                                    "displayed safely"),
                                 advanced);
  hazard_notice_->setObjectName(QStringLiteral("settings_hazard_notice"));
  advanced_form->addRow(QString(), hazard_notice_);
  stall_seconds_ = new QSpinBox(advanced);
  stall_seconds_->setObjectName(QStringLiteral("settings_stall_seconds"));
  stall_seconds_->setRange(Settings::kMinStallSeconds, Settings::kMaxStallSeconds);
  stall_seconds_->setSingleStep(10);
  stall_seconds_->setSuffix(tr(" s"));
  advanced_form->addRow(tr("Warn when rendering has not finished for:"), stall_seconds_);
  crash_notice_ = new QCheckBox(tr("Tell me if OnPdf did not exit normally last time"), advanced);
  crash_notice_->setObjectName(QStringLiteral("settings_crash_notice"));
  advanced_form->addRow(QString(), crash_notice_);
  render_process_ = new QCheckBox(tr("Render pages in a separate process (recovers from PDF "
                                     "crashes; applies to the next document)"),
                                  advanced);
  render_process_->setObjectName(QStringLiteral("settings_render_process"));
  advanced_form->addRow(QString(), render_process_);
  render_timeout_ = new QSpinBox(advanced);
  render_timeout_->setObjectName(QStringLiteral("settings_render_timeout"));
  render_timeout_->setRange(Settings::kMinRenderTimeoutSeconds, Settings::kMaxRenderTimeoutSeconds);
  render_timeout_->setSingleStep(5);
  render_timeout_->setSuffix(tr(" s"));
  advanced_form->addRow(tr("Give up rendering a page after:"), render_timeout_);
  tabs->addTab(advanced, tr("Advanced"));

  auto* buttons =
      new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel |
                               QDialogButtonBox::Apply | QDialogButtonBox::RestoreDefaults,
                           this);
  buttons->setObjectName(QStringLiteral("settings_buttons"));
  connect(buttons, &QDialogButtonBox::accepted, this, [this] {
    apply();
    accept();
  });
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, this,
          [this] { apply(); });
  connect(buttons->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, this,
          [this] { load_defaults(); });

  auto* layout = new QVBoxLayout(this);
  layout->addWidget(tabs);
  layout->addWidget(buttons);
  load();
}

void SettingsDialog::fill_color_combo(QComboBox* combo, engine::Color current, bool offer_black) {
  combo->clear();
  if (offer_black) {
    combo->addItem(color_swatch(Settings::kDefaultTextColor), tr("Black"),
                   Settings::color_to_string(Settings::kDefaultTextColor));
  }
  for (int i = 0; i < kPaletteSize; ++i) {
    combo->addItem(color_swatch(kPalette[i]), MainWindow::tr(kPaletteNames[i]),
                   Settings::color_to_string(kPalette[i]));
  }
  const QString current_text = Settings::color_to_string(current);
  if (combo->findData(current_text) <
      0) {  // 팔레트 밖 색(이전 판·직접 편집한 설정)은 그대로 보인다
    combo->addItem(color_swatch(current), tr("Custom (%1)").arg(current_text), current_text);
  }
  select_data(combo, current_text);
}

engine::Color SettingsDialog::combo_color(const QComboBox* combo, engine::Color fallback) {
  return Settings::color_from_string(combo->currentData().toString(), fallback);
}

void SettingsDialog::select_data(QComboBox* combo, const QVariant& data) {
  const int index = combo->findData(data);
  combo->setCurrentIndex(index >= 0 ? index : 0);
}

void SettingsDialog::load() {
  select_data(language_, static_cast<int>(settings_.language()));
  select_data(start_zoom_, static_cast<int>(settings_.start_zoom()));
  select_data(sidebar_, static_cast<int>(settings_.sidebar_start()));
  select_data(border_width_, settings_.border_width());
  select_data(opacity_, settings_.opacity_percent());
  fill_color_combo(annotation_color_, settings_.annotation_color(), false);
  text_size_->setValue(settings_.text_size());
  fill_color_combo(text_color_, settings_.text_color(), true);
  author_->setText(settings_.author());
  cache_mb_->setValue(settings_.cache_mb());
  hazard_notice_->setChecked(settings_.hazard_notice());
  stall_seconds_->setValue(settings_.stall_seconds());
  crash_notice_->setChecked(settings_.crash_notice());
  render_process_->setChecked(settings_.render_process());
  render_timeout_->setValue(settings_.render_timeout_seconds());
}

void SettingsDialog::load_defaults() {
  select_data(language_, static_cast<int>(LanguageChoice::kSystem));
  select_data(start_zoom_, static_cast<int>(StartZoom::kFitWidth));
  select_data(sidebar_, static_cast<int>(SidebarStart::kLast));
  select_data(border_width_, Settings::kDefaultBorderWidth);
  select_data(opacity_, Settings::kDefaultOpacityPercent);
  fill_color_combo(annotation_color_, Settings::kDefaultAnnotationColor, false);
  text_size_->setValue(Settings::kDefaultTextSize);
  fill_color_combo(text_color_, Settings::kDefaultTextColor, true);
  author_->clear();
  cache_mb_->setValue(Settings::kDefaultCacheMb);
  hazard_notice_->setChecked(true);
  stall_seconds_->setValue(Settings::kDefaultStallSeconds);
  crash_notice_->setChecked(true);
  render_process_->setChecked(false);
  render_timeout_->setValue(Settings::kDefaultRenderTimeoutSeconds);
}

void SettingsDialog::apply() {
  const LanguageChoice before = settings_.language();
  settings_.set_language(static_cast<LanguageChoice>(language_->currentData().toInt()));
  settings_.set_start_zoom(static_cast<StartZoom>(start_zoom_->currentData().toInt()));
  settings_.set_sidebar_start(static_cast<SidebarStart>(sidebar_->currentData().toInt()));
  settings_.set_border_width(border_width_->currentData().toDouble());
  settings_.set_opacity_percent(opacity_->currentData().toInt());
  settings_.set_annotation_color(combo_color(annotation_color_, Settings::kDefaultAnnotationColor));
  settings_.set_text_size(text_size_->value());
  settings_.set_text_color(combo_color(text_color_, Settings::kDefaultTextColor));
  settings_.set_author(author_->text());
  settings_.set_cache_mb(cache_mb_->value());
  settings_.set_hazard_notice(hazard_notice_->isChecked());
  settings_.set_stall_seconds(stall_seconds_->value());
  settings_.set_crash_notice(crash_notice_->isChecked());
  settings_.set_render_process(render_process_->isChecked());
  settings_.set_render_timeout_seconds(render_timeout_->value());
  settings_.sync();
  if (settings_.language() != before) emit language_changed();
}

}  // namespace onpdf::app
