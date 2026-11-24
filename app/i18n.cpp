#include "i18n.h"

#include <QPointer>
#include <QTranslator>
#include <QtGlobal>

namespace onpdf::app {
namespace {

// 설치한 번역기 (앱이 부모 — 앱과 함께 사라진다). 메인 스레드에서만 다룬다
QPointer<QTranslator> g_app_translator;
QPointer<QTranslator> g_qt_translator;
Language g_language = Language::kEnglish;

void remove(QCoreApplication& app, QPointer<QTranslator>& translator) {
  if (translator == nullptr) return;
  QCoreApplication::removeTranslator(translator);
  translator->deleteLater();
  translator = nullptr;
  (void)app;
}

bool install(QCoreApplication& app, QPointer<QTranslator>& slot, const QString& resource) {
  auto* translator = new QTranslator(&app);  // 부모가 소유한다
  if (!translator->load(resource)) {
    qWarning("i18n: cannot load %s", qUtf8Printable(resource));
    delete translator;
    return false;
  }
  QCoreApplication::installTranslator(translator);
  slot = translator;
  return true;
}

}  // namespace

std::optional<Language> parse_language(QStringView tag) {
  const QString lower =
      tag.toString().trimmed().toLower().replace(QLatin1Char('_'), QLatin1Char('-'));
  const QString primary = lower.section(QLatin1Char('-'), 0, 0);
  if (primary == QLatin1String("ko")) return Language::kKorean;
  if (primary == QLatin1String("en")) return Language::kEnglish;
  return std::nullopt;
}

Language language_from_locale(const QStringList& ui_languages) {
  for (const QString& tag : ui_languages) {
    if (const auto lang = parse_language(tag)) return *lang;
  }
  return Language::kEnglish;
}

bool set_language(QCoreApplication& app, Language lang) {
  remove(app, g_app_translator);
  remove(app, g_qt_translator);
  g_language = lang;
  if (lang == Language::kKorean) {
    const bool ok = install(app, g_app_translator, QStringLiteral(":/i18n/onpdf_ko.qm"));
    // Qt 표준 문구(QMessageBox 단추 등). 빌드 때 Qt 설치에서 찾지 못했으면 없다 — 영어로 남는다
    (void)install(app, g_qt_translator, QStringLiteral(":/i18n/qtbase_ko.qm"));
    return ok;
  }
  // 영어 원문은 번역이 필요 없고 복수형("%n file(s)" → "1 file" / "2 files")만 번역한다
  return install(app, g_app_translator, QStringLiteral(":/i18n/onpdf_en.qm"));
}

Language current_language() noexcept {
  return g_language;
}

}  // namespace onpdf::app
