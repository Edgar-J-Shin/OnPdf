// onpdf_app — Qt 6 Widgets 메인 창.
//   onpdf_app [file.pdf] [--screenshot out.png]
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
#include "main_window.h"
#include "ui_dispatcher.h"
#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDir>
#include <QString>
#include <cstdio>

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  QApplication::setApplicationName(QStringLiteral("OnPdf"));
  QApplication::setApplicationVersion(QStringLiteral(ONPDF_VERSION));

  // --help / --version 은 여기서 종료하므로 워커보다 먼저 처리한다
  QCommandLineParser parser;
  parser.addHelpOption();
  parser.addVersionOption();
  parser.addPositionalArgument(QStringLiteral("file"),
                               QCoreApplication::translate("main", "열 PDF 파일"));
  QCommandLineOption screenshot(
      QStringLiteral("screenshot"),
      QCoreApplication::translate("main", "첫 쪽을 그린 뒤 창을 PNG 로 저장하고 종료"),
      QStringLiteral("png"));
  parser.addOption(screenshot);
  QCommandLineOption self_test(
      QStringLiteral("self-test"),
      QCoreApplication::translate(
          "main", "배포물 점검: 파일을 열어 첫 쪽을 그린 뒤 결과를 종료 코드로 알리고 끝낸다"));
  parser.addOption(self_test);
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
