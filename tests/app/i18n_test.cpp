// 다국어 (#8): 언어 선택 규칙, 영어 원문·한국어 번역·영어 복수형·Qt 표준 문구. 테스트 진입점은
// 한국어를 설치하므로 영어를 보는 테스트는 끝나면 한국어로 되돌린다.

#include "i18n.h"

#include "onpdf/core/engine/pdfium_worker.h"

#include "main_window.h"
#include "ui_dispatcher.h"
#include <QAction>
#include <QApplication>
#include <QFile>
#include <QMenuBar>
#include <gtest/gtest.h>

namespace onpdf::app {
namespace {

TEST(I18nTest, LanguageFromLocale) {
  EXPECT_EQ(language_from_locale({QStringLiteral("ko-KR")}), Language::kKorean);
  EXPECT_EQ(language_from_locale({QStringLiteral("en-US"), QStringLiteral("ko")}),
            Language::kEnglish);
  EXPECT_EQ(language_from_locale({QStringLiteral("ja-JP")}), Language::kEnglish);
  EXPECT_EQ(language_from_locale({QStringLiteral("ja-JP"), QStringLiteral("ko-KR")}),
            Language::kKorean)
      << "지원하는 첫 선호 언어";
  EXPECT_EQ(language_from_locale({}), Language::kEnglish);
  EXPECT_EQ(parse_language(u"KO"), Language::kKorean);
  EXPECT_EQ(parse_language(u"ko_KR"), Language::kKorean);
  EXPECT_EQ(parse_language(u" en-GB "), Language::kEnglish);
  EXPECT_FALSE(parse_language(u"de"));
  EXPECT_FALSE(parse_language(u""));
}

class I18nWindowTest : public ::testing::Test {
 protected:
  ~I18nWindowTest() override { (void)set_language(*qApp, Language::kKorean); }
  static engine::PdfiumWorker::Options options() {
    engine::PdfiumWorker::Options o;
    o.use_system_fonts = false;
    return o;
  }
  static QString translate(const char* source, int n = -1) {
    return QCoreApplication::translate("onpdf::app::MainWindow", source, nullptr, n);
  }
};

TEST_F(I18nWindowTest, EnglishIsTheSourceLanguage) {
  ASSERT_TRUE(set_language(*qApp, Language::kEnglish));
  EXPECT_EQ(current_language(), Language::kEnglish);
  UiDispatcher dispatcher;
  engine::PdfiumWorker worker(options());
  MainWindow window(worker, dispatcher);
  const QList<QAction*> menus = window.menuBar()->actions();
  ASSERT_FALSE(menus.isEmpty());
  EXPECT_EQ(menus.front()->text(), QStringLiteral("&File"));
  EXPECT_EQ(window.undo_action()->text(), QStringLiteral("&Undo"));
  // 영어 복수형 (onpdf_en.ts)
  EXPECT_EQ(translate("Saved %n file(s).", 1), QStringLiteral("Saved 1 file."));
  EXPECT_EQ(translate("Saved %n file(s).", 3), QStringLiteral("Saved 3 files."));
  EXPECT_EQ(translate("Copied %n character(s)", 1), QStringLiteral("Copied 1 character"));
}

TEST_F(I18nWindowTest, KoreanTranslation) {
  ASSERT_TRUE(set_language(*qApp, Language::kKorean));
  UiDispatcher dispatcher;
  engine::PdfiumWorker worker(options());
  MainWindow window(worker, dispatcher);
  EXPECT_EQ(window.menuBar()->actions().front()->text(), QStringLiteral("파일(&F)"));
  EXPECT_EQ(window.undo_action()->text(), QStringLiteral("실행 취소(&U)"));
  EXPECT_EQ(translate("Saved %n file(s).", 3), QStringLiteral("3개 파일을 저장했습니다."));
  EXPECT_EQ(translate("Undo"), QStringLiteral("Undo")) << "원문에 없는 문자열은 그대로";
}

TEST_F(I18nWindowTest, QtStandardStringsInKorean) {
  if (!QFile::exists(QStringLiteral(":/i18n/qtbase_ko.qm")))
    GTEST_SKIP() << "qtbase_ko.qm was not found in the Qt installation at build time";
  ASSERT_TRUE(set_language(*qApp, Language::kKorean));
  EXPECT_EQ(QCoreApplication::translate("QPlatformTheme", "Cancel"), QStringLiteral("취소"));
  ASSERT_TRUE(set_language(*qApp, Language::kEnglish));
  EXPECT_EQ(QCoreApplication::translate("QPlatformTheme", "Cancel"), QStringLiteral("Cancel"));
}

}  // namespace
}  // namespace onpdf::app
