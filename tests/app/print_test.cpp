#include "document_view.h"
#include "main_window.h"
#include "onpdf/core/engine/pdfium_worker.h"
#include "onpdf/core/platform/path.h"
#include "print_job.h"
#include "ui_dispatcher.h"

#include "../test_support.h"

#include <gtest/gtest.h>

#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QPageRanges>
#include <QPageSize>
#include <QPrinter>
#include <QTemporaryDir>
#include <QTimer>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

namespace onpdf::app {
namespace {

using onpdf::test::testdata;

template <class Pred>
bool spin_until(Pred pred, int timeout_ms = 20000) {
  QEventLoop loop;
  QTimer poll;
  bool timed_out = false;
  QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
    if (pred()) loop.quit();
  });
  poll.start(5);
  QTimer::singleShot(timeout_ms, &loop, [&] {
    timed_out = true;
    loop.quit();
  });
  if (pred()) return true;
  loop.exec();
  return !timed_out;
}

struct Box {
  int left = 1 << 30, top = 1 << 30, right = -1, bottom = -1, count = 0;
  [[nodiscard]] double cx() const { return (left + right) / 2.0; }
  [[nodiscard]] double cy() const { return (top + bottom) / 2.0; }
};

// 조건에 맞는 픽셀의 경계 상자
template <class Pred>
Box find_pixels(const Bitmap& bmp, Pred pred) {
  Box box;
  for (int y = 0; y < bmp.height; ++y) {
    const auto* row = bmp.pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(bmp.stride);
    for (int x = 0; x < bmp.width; ++x) {
      const auto* p = row + static_cast<std::size_t>(x) * 4;
      if (pred(p[2], p[1], p[0])) {
        box.left = std::min(box.left, x);
        box.top = std::min(box.top, y);
        box.right = std::max(box.right, x);
        box.bottom = std::max(box.bottom, y);
        ++box.count;
      }
    }
  }
  return box;
}

bool blue(int r, int g, int b) { return b > 180 && r < 80 && g < 80; }
bool green(int r, int g, int b) { return g > 100 && r < 80 && b < 80; }
bool red(int r, int g, int b) { return r > 160 && g < 80 && b < 80; }

class PrintTest : public ::testing::Test {
 protected:
  PrintTest() : worker_(options()), window_(worker_, dispatcher_) {
    window_.resize(900, 900);
    window_.show();
  }
  static engine::PdfiumWorker::Options options() {
    engine::PdfiumWorker::Options o;
    o.use_system_fonts = false;
    return o;
  }
  bool open(const char* name) {
    window_.open_file(QString::fromStdString(onpdf::path_to_utf8(testdata(name))));
    return spin_until([&] {
      return window_.windowTitle().endsWith(QString::fromUtf8(name)) && window_.document_view()->has_document();
    });
  }
  std::unique_ptr<QPrinter> pdf_printer(const QString& file) {
    auto printer = std::make_unique<QPrinter>(QPrinter::HighResolution);
    printer->setOutputFormat(QPrinter::PdfFormat);
    printer->setOutputFileName(dir_.filePath(file));
    printer->setPageSize(QPageSize(QPageSize::Letter));
    return printer;
  }
  // 인쇄를 끝까지 기다린다
  bool print_and_wait(QPrinter& printer) {
    if (!window_.print_with(printer)) return false;
    return spin_until([&] { return window_.print_job() != nullptr && !window_.print_job()->active(); });
  }
  // 출력 PDF 를 같은 워커로 연다
  Result<engine::DocumentHandle> open_output(const QString& file) {
    auto f = engine::DocumentHandle::open(worker_, onpdf::path_from_utf8(dir_.filePath(file).toStdString()));
    return f.get();
  }
  Result<Bitmap> render(const engine::DocumentHandle& doc, int page, float scale = 0.25f) {
    engine::RenderOptions o;
    o.scale = scale;
    return doc.render_page(page, o).get();
  }

  QTemporaryDir dir_;
  UiDispatcher dispatcher_;
  engine::PdfiumWorker worker_;
  MainWindow window_;
};

TEST_F(PrintTest, PrintsAllPagesFittedAndRotated) {
  ASSERT_TRUE(open("simple.pdf"));
  EXPECT_TRUE(window_.print_action()->isEnabled());
  auto printer = pdf_printer("simple.pdf");
  ASSERT_TRUE(print_and_wait(*printer));
  const PrintJob* job = window_.print_job();
  EXPECT_EQ(job->printed_sheets(), 2);
  EXPECT_TRUE(job->failed_pages().empty());
  EXPECT_LE(job->max_in_flight(), PrintJob::kMaxInFlight);
  std::printf("[print] max draw ms (300 dpi Letter): %lld\n", static_cast<long long>(job->max_draw_ms()));

  auto out = open_output("simple.pdf");
  ASSERT_TRUE(out) << out.error().message;
  ASSERT_EQ(out->info().page_count, 2);
  for (const auto& size : out->info().page_sizes) {  // 두 쪽 모두 세로 Letter 용지
    EXPECT_NEAR(size.width, 612.0, 1.0);
    EXPECT_NEAR(size.height, 792.0, 1.0);
  }
  // 1쪽: 원본 파란 사각형이 위쪽 왼편에
  auto p0 = render(*out, 0);
  ASSERT_TRUE(p0) << p0.error().message;
  const Box b = find_pixels(*p0, blue);
  ASSERT_GT(b.count, 50);
  EXPECT_LT(b.cx(), p0->width / 2.0);
  EXPECT_LT(b.cy(), p0->height / 2.0);
  // 2쪽(가로): 반시계 90° 회전 → 원본 왼쪽 가운데의 녹색 사각형이 아래쪽 오른편으로
  auto p1 = render(*out, 1);
  ASSERT_TRUE(p1) << p1.error().message;
  const Box g = find_pixels(*p1, green);
  ASSERT_GT(g.count, 50);
  EXPECT_GT(g.cx(), p1->width / 2.0);
  EXPECT_GT(g.cy(), p1->height / 2.0);
}

TEST_F(PrintTest, PageRangeCurrentPageAndCopies) {
  ASSERT_TRUE(open("simple.pdf"));
  {
    auto printer = pdf_printer("range.pdf");
    QPageRanges ranges;
    ranges.addRange(2, 2);
    printer->setPageRanges(ranges);
    printer->setPrintRange(QPrinter::PageRange);
    ASSERT_TRUE(print_and_wait(*printer));
    auto out = open_output("range.pdf");
    ASSERT_TRUE(out) << out.error().message;
    EXPECT_EQ(out->info().page_count, 1);
    auto p = render(*out, 0);
    ASSERT_TRUE(p);
    EXPECT_GT(find_pixels(*p, green).count, 50);  // 2쪽 내용
  }
  {
    window_.document_view()->go_to_page(0);
    auto printer = pdf_printer("current.pdf");
    printer->setPrintRange(QPrinter::CurrentPage);
    ASSERT_TRUE(print_and_wait(*printer));
    auto out = open_output("current.pdf");
    ASSERT_TRUE(out);
    EXPECT_EQ(out->info().page_count, 1);
  }
  {
    auto printer = pdf_printer("copies.pdf");
    printer->setCopyCount(2);
    printer->setCollateCopies(true);
    const bool driver_copies = printer->supportsMultipleCopies();
    ASSERT_TRUE(print_and_wait(*printer));
    auto out = open_output("copies.pdf");
    ASSERT_TRUE(out);
    // 인쇄 장치가 매수를 처리하지 않으면 앱이 반복한다 (한 부씩: 1,2,1,2)
    EXPECT_EQ(out->info().page_count, driver_copies ? 2 : 4);
    if (!driver_copies) {
      auto third = render(*out, 2);
      ASSERT_TRUE(third);
      EXPECT_GT(find_pixels(*third, blue).count, 50);  // 세 번째 장 = 1쪽
    }
  }
}

TEST_F(PrintTest, PrintFlagsAnnotations) {
  ASSERT_TRUE(open("print-flags.pdf"));
  auto printer = pdf_printer("flags.pdf");
  QPageRanges ranges;
  ranges.addRange(1, 1);
  printer->setPageRanges(ranges);
  printer->setPrintRange(QPrinter::PageRange);
  ASSERT_TRUE(print_and_wait(*printer));
  // 문서가 /PrintScaling None → 실제 크기
  EXPECT_EQ(window_.print_job()->request().scaling, view::PrintScaling::kActualSize);
  auto out = open_output("flags.pdf");
  ASSERT_TRUE(out);
  auto p = render(*out, 0);
  ASSERT_TRUE(p);
  EXPECT_GT(find_pixels(*p, red).count, 50);    // 인쇄 전용 주석은 나온다
  EXPECT_EQ(find_pixels(*p, green).count, 0);   // 화면 전용 주석은 빠진다
}

TEST_F(PrintTest, GrayscaleAndLowResolutionRequests) {
  ASSERT_TRUE(open("print-low-res.pdf"));
  auto printer = pdf_printer("gray.pdf");
  printer->setColorMode(QPrinter::GrayScale);
  ASSERT_TRUE(print_and_wait(*printer));
  EXPECT_TRUE(window_.print_job()->request().grayscale);
  EXPECT_FALSE(window_.print_job()->request().high_quality);  // 저해상도 권한 → 150 DPI
  auto out = open_output("gray.pdf");
  ASSERT_TRUE(out);
  auto p = render(*out, 0);
  ASSERT_TRUE(p);
  EXPECT_EQ(find_pixels(*p, blue).count, 0);  // 파란색이 회색으로
}

TEST_F(PrintTest, PermissionDeniedDisablesPrinting) {
  ASSERT_TRUE(open("encrypted-owner-only.pdf"));
  EXPECT_FALSE(window_.print_action()->isEnabled());
  EXPECT_FALSE(window_.print_action()->statusTip().isEmpty());
  auto printer = pdf_printer("denied.pdf");
  EXPECT_FALSE(window_.print_with(*printer));
  EXPECT_EQ(window_.print_job(), nullptr);
  ASSERT_TRUE(open("simple.pdf"));
  EXPECT_TRUE(window_.print_action()->isEnabled());
}

TEST_F(PrintTest, CancelLargeDocumentKeepsUiResponsive) {
  ASSERT_TRUE(open("large-1000.pdf"));
  auto printer = pdf_printer("large.pdf");
  // 이벤트 루프 간격 측정
  QElapsedTimer clock;
  clock.start();
  qint64 last = 0, worst_gap = 0;
  QTimer tick;
  QObject::connect(&tick, &QTimer::timeout, [&] {
    worst_gap = std::max(worst_gap, clock.elapsed() - last);
    last = clock.elapsed();
  });
  tick.start(10);
  ASSERT_TRUE(window_.print_with(*printer));
  EXPECT_FALSE(window_.print_action()->isEnabled());  // 인쇄 중에는 다시 인쇄할 수 없다
  PrintJob* job = window_.print_job();
  EXPECT_EQ(job->total_sheets(), 1000);
  ASSERT_TRUE(spin_until([&] { return job->printed_sheets() >= 6; }));
  bool cancelled = false;
  QObject::connect(job, &PrintJob::finished, [&](PrintJob::Outcome o) { cancelled = o == PrintJob::Outcome::kCancelled; });
  job->cancel();
  tick.stop();
  EXPECT_TRUE(cancelled);
  EXPECT_FALSE(job->active());
  EXPECT_LT(job->printed_sheets(), 1000);
  EXPECT_LE(job->max_in_flight(), PrintJob::kMaxInFlight);
  // 최적화 빌드 실측: 장당 메인 스레드 ≤ 30ms, 인쇄 시작·진행 창 표시 때 한 번 ≤ 200ms.
  // Debug(-O0) 는 32MB 쪽 비트맵 해제가 원소 단위 루프로 돌아 장당 수십 ms 가 더 든다
#ifdef NDEBUG
  constexpr qint64 kMaxGapMs = 250;
#else
  constexpr qint64 kMaxGapMs = 600;
#endif
  EXPECT_LT(worst_gap, kMaxGapMs) << "event loop stalled while printing";
  std::printf("[print] sheets before cancel=%d worst event-loop gap=%lldms max draw=%lldms max handler=%lldms\n",
              job->printed_sheets(), static_cast<long long>(worst_gap), static_cast<long long>(job->max_draw_ms()),
              static_cast<long long>(job->max_handler_ms()));
  EXPECT_TRUE(window_.print_action()->isEnabled());
  // 워커가 계속 쓸 수 있다
  ASSERT_TRUE(open("simple.pdf"));
}

TEST_F(PrintTest, OpeningAnotherDocumentCancelsPrinting) {
  ASSERT_TRUE(open("large-1000.pdf"));
  auto printer = pdf_printer("replaced.pdf");
  ASSERT_TRUE(window_.print_with(*printer));
  ASSERT_TRUE(spin_until([&] { return window_.print_job()->printed_sheets() >= 2; }));
  ASSERT_TRUE(open("bookmarks.pdf"));
  EXPECT_EQ(window_.print_job(), nullptr);
  QApplication::processEvents();
}

TEST(PrintRequest, MapsPrinterState) {
  engine::DocumentInfo info;
  info.page_count = 5;
  info.page_sizes.assign(5, engine::PageSize{612, 792});
  QPrinter printer(QPrinter::HighResolution);
  printer.setOutputFormat(QPrinter::PdfFormat);
  auto all = make_print_request(printer, info, 2);
  ASSERT_EQ(all.steps.size(), 5u);
  EXPECT_EQ(all.scaling, view::PrintScaling::kFit);
  EXPECT_TRUE(all.high_quality);

  printer.setPrintRange(QPrinter::CurrentPage);
  auto current = make_print_request(printer, info, 3);
  ASSERT_EQ(current.steps.size(), 1u);
  EXPECT_EQ(current.steps[0].page, 3);

  QPageRanges ranges;
  ranges.addRange(2, 3);
  ranges.addRange(5, 9);  // 쪽 수 밖은 잘린다
  printer.setPageRanges(ranges);
  printer.setPrintRange(QPrinter::PageRange);
  auto ranged = make_print_request(printer, info, 0);
  ASSERT_EQ(ranged.steps.size(), 3u);
  EXPECT_EQ(ranged.steps[0].page, 1);
  EXPECT_EQ(ranged.steps[2].page, 4);

  info.print.no_scaling = true;
  info.print.high_quality = false;
  auto prefs = make_print_request(printer, info, 0);
  EXPECT_EQ(prefs.scaling, view::PrintScaling::kActualSize);
  EXPECT_FALSE(prefs.high_quality);
}

TEST(PrintRequest, AppliesDocumentPreferences) {
  QPrinter printer(QPrinter::HighResolution);
  printer.setOutputFormat(QPrinter::PdfFormat);
  view::PrintPreferences prefs;
  prefs.copies = 3;
  prefs.page_ranges = {{1, 2}};
  prefs.duplex = view::Duplex::kFlipLongEdge;
  apply_print_preferences(printer, prefs, 5);
  EXPECT_EQ(printer.printRange(), QPrinter::PageRange);
  EXPECT_EQ(printer.pageRanges().toRangeList().size(), 1);
  EXPECT_EQ(printer.pageRanges().firstPage(), 2);
  EXPECT_EQ(printer.pageRanges().lastPage(), 3);
  // 다음 문서(기본값)에서는 범위가 지워진다
  apply_print_preferences(printer, view::PrintPreferences{}, 5);
  EXPECT_EQ(printer.printRange(), QPrinter::AllPages);
  EXPECT_TRUE(printer.pageRanges().isEmpty());
}

}  // namespace
}  // namespace onpdf::app
