// onpdf_app — Qt 6 Widgets 메인 창.
//   onpdf_app [file.pdf] [--screenshot out.png]
// 객체 수명 순서(선언 순서의 역순으로 소멸):
//   QApplication → UiDispatcher → PdfiumWorker → MainWindow
// 창이 먼저 죽고(핸들 close), 워커가 join 되고, 그 뒤 디스패처가 미처리 콜백을 버린다.

#include "main_window.h"
#include "onpdf/core/engine/pdfium_worker.h"
#include "ui_dispatcher.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QString>

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  QApplication::setApplicationName(QStringLiteral("OnPdf"));
  QApplication::setApplicationVersion(QStringLiteral("0.0.1"));

  onpdf::app::UiDispatcher dispatcher;

  onpdf::engine::PdfiumWorker::Options options;
  options.use_system_fonts = true;
  onpdf::engine::PdfiumWorker worker(options);

  QCommandLineParser parser;
  parser.addHelpOption();
  parser.addVersionOption();
  parser.addPositionalArgument(QStringLiteral("file"), QStringLiteral("열 PDF 파일"));
  QCommandLineOption screenshot(QStringLiteral("screenshot"),
                                QStringLiteral("첫 쪽을 그린 뒤 창을 PNG 로 저장하고 종료"),
                                QStringLiteral("png"));
  parser.addOption(screenshot);
  parser.process(app);

  onpdf::app::MainWindow window(worker, dispatcher);
  if (parser.isSet(screenshot)) window.screenshot_after_render(parser.value(screenshot));
  window.show();
  const QStringList files = parser.positionalArguments();
  if (!files.isEmpty()) window.open_file(files.first());

  return app.exec();
}
