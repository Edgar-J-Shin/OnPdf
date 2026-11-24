// onpdf_app — Qt 6 Widgets 메인 창.
//   onpdf_app [file.pdf] [--screenshot out.png] [--lang ko|en]
//   onpdf_app --self-test file.pdf   배포물 점검(#90): qpdf·번들 글꼴·PDFium 렌더·플랫폼 플러그인을
//   거쳐 종료 코드로 보고
//                                    0 성공, 1 렌더 실패·시간 초과, 2 번들 글꼴 없음, 3 qpdf 가
//                                    파일을 못 읽음
// 객체 수명 순서(선언 순서의 역순으로 소멸):
//   QApplication → UiDispatcher → PdfiumWorker → MainWindow
// 창이 먼저 죽고(핸들 close), 워커가 join 되고, 그 뒤 디스패처가 미처리 콜백을 버린다.

#include "onpdf/core/engine/pdfium_worker.h"
#include "onpdf/core/engine/qpdf_tools.h"
#include "onpdf/core/platform/path.h"

#include "file_open_filter.h"
#include "i18n.h"
#include "main_window.h"
#include "ui_dispatcher.h"
#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDir>
#include <QLocale>
#include <QString>
#include <cstdio>
#include <optional>

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  QApplication::setApplicationName(QStringLiteral("OnPdf"));
  QApplication::setApplicationVersion(QStringLiteral(ONPDF_VERSION));

  // UI 언어 (#8): --lang → ONPDF_LANG → 시스템 선호 언어(ko 외에는 영어). 파서의 도움말 문구도
  // 번역되도록 파서보다 먼저 argv 를 훑는다
  std::optional<onpdf::app::Language> forced;
  const QStringList args = QCoreApplication::arguments();
  for (qsizetype i = 0; i < args.size(); ++i) {
    QString value;
    if (args[i] == QLatin1String("--lang") && i + 1 < args.size())
      value = args[i + 1];
    else if (args[i].startsWith(QLatin1String("--lang=")))
      value = args[i].mid(7);
    else
      continue;
    forced = onpdf::app::parse_language(value);
    if (!forced) std::fprintf(stderr, "unsupported --lang %s (use ko or en)\n", qPrintable(value));
    break;
  }
  if (!forced && qEnvironmentVariableIsSet("ONPDF_LANG"))
    forced = onpdf::app::parse_language(qEnvironmentVariable("ONPDF_LANG"));
  (void)onpdf::app::set_language(
      app, forced.value_or(onpdf::app::language_from_locale(QLocale::system().uiLanguages())));

  // --help / --version 은 여기서 종료하므로 워커보다 먼저 처리한다
  QCommandLineParser parser;
  parser.addHelpOption();
  parser.addVersionOption();
  parser.addPositionalArgument(QStringLiteral("file"),
                               QCoreApplication::translate("main", "PDF file to open"));
  QCommandLineOption screenshot(
      QStringLiteral("screenshot"),
      QCoreApplication::translate("main", "Render the first page, save the window as PNG and exit"),
      QStringLiteral("png"));
  parser.addOption(screenshot);
  QCommandLineOption self_test(
      QStringLiteral("self-test"),
      QCoreApplication::translate("main",
                                  "Release check: open the file, render the first page, report the "
                                  "result as the exit code and quit"));
  parser.addOption(self_test);
  QCommandLineOption lang(QStringLiteral("lang"),
                          QCoreApplication::translate("main", "User interface language (ko or en)"),
                          QStringLiteral("language"));
  parser.addOption(lang);  // 값은 위에서 이미 썼다 — 도움말과 "알 수 없는 옵션" 방지용
  parser.process(app);

  onpdf::app::UiDispatcher dispatcher;

  onpdf::engine::PdfiumWorker::Options options;
  options.use_system_fonts = true;
  onpdf::engine::PdfiumWorker worker(options);

  const QStringList files = parser.positionalArguments();
  if ((parser.isSet(screenshot) || parser.isSet(self_test)) && files.isEmpty()) {
    qWarning("--screenshot / --self-test requires a PDF file argument");
    return 1;
  }

  // qpdf 는 PDFium 과 무관해 메인 스레드에서 바로 확인한다 (vcpkg 공유 라이브러리가 배포물에
  // 들어갔는지)
  int self_test_pages = 0;
  if (parser.isSet(self_test)) {
    const auto pages =
        onpdf::engine::QpdfTools::page_count(onpdf::path_from_utf8(files.first().toStdString()));
    if (!pages) {
      std::fprintf(stderr, "self-test: qpdf cannot read %s: %s\n", qPrintable(files.first()),
                   pages.error().message.c_str());
      return 3;
    }
    self_test_pages = *pages;
  }

  onpdf::app::MainWindow window(worker, dispatcher);
  if (parser.isSet(self_test)) {
    if (!window.has_text_font()) {
      std::fprintf(stderr, "self-test: bundled font not found next to the executable\n");
      return 2;
    }
    window.screenshot_after_render(QDir::temp().filePath(QStringLiteral("onpdf-self-test.png")));
  } else if (parser.isSet(screenshot)) {
    window.screenshot_after_render(parser.value(screenshot));
  }
  onpdf::app::FileOpenFilter file_open(window);
  app.installEventFilter(&file_open);
  window.show();
  if (!files.isEmpty()) window.open_file(files.first());

  const int code = app.exec();
  if (parser.isSet(self_test) && code == 0) {
    std::printf("self-test ok: OnPdf %s, pages %d\n", ONPDF_VERSION, self_test_pages);
  }
  return code;
}
