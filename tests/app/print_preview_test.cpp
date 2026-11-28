// 인쇄 미리보기 (#23): 장 순서(범위·매수), 배치가 인쇄와 같은 place_page 인지, 배율 변경이 재렌더
// 없이 배치만 바꾸는지, 선택한 옵션이 요청에 실리는지, 큰 문서에서 보이는 장만 렌더하는지.

#include "onpdf/core/engine/document_handle.h"
#include "onpdf/core/engine/pdfium_worker.h"
#include "onpdf/core/view/print_layout.h"

#include "../test_support.h"
#include "dialog_helpers.h"
#include "print_job.h"
#include "print_preview_dialog.h"
#include "ui_dispatcher.h"
#include <QCheckBox>
#include <QComboBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QPageLayout>
#include <QPageRanges>
#include <QPageSize>
#include <QPrinter>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QTimer>
#include <cstdlib>
#include <gtest/gtest.h>
#include <memory>

namespace onpdf::app {
namespace {

using onpdf::test::spin_until;
using onpdf::test::testdata;

class PrintPreviewTest : public ::testing::Test {
 protected:
  PrintPreviewTest() : worker_(options()) {}
  static engine::PdfiumWorker::Options options() {
    engine::PdfiumWorker::Options o;
    o.use_system_fonts = false;
    return o;
  }
  engine::DocumentHandle open(const char* name) {
    auto result = engine::DocumentHandle::open(worker_, testdata(name)).get();
    EXPECT_TRUE(result);
    return result ? *result : engine::DocumentHandle{};
  }
  std::unique_ptr<QPrinter> printer(const QString& file = QStringLiteral("out.pdf")) {
    auto p = std::make_unique<QPrinter>(QPrinter::HighResolution);
    p->setOutputFormat(QPrinter::PdfFormat);
    p->setOutputFileName(dir_.filePath(file));
    p->setPageSize(QPageSize(QPageSize::Letter));
    return p;
  }
  static engine::PageSize printable_of(const QPrinter& printer) {
    const QRectF paint = printer.pageLayout().paintRect(QPageLayout::Point);
    return {paint.width(), paint.height()};
  }
  // 대기 렌더가 모두 끝날 때까지
  static bool settle(PrintPreviewView* view) {
    return spin_until([&] { return view->idle(); });
  }
  std::unique_ptr<PrintPreviewDialog> make_dialog(const engine::DocumentHandle& handle,
                                                  QPrinter& printer) {
    auto dialog = std::make_unique<PrintPreviewDialog>(
        handle, dispatcher_, printer, make_print_request(printer, handle.info(), 0));
    dialog->resize(600, 700);
    dialog->show();
    return dialog;
  }

  QTemporaryDir dir_;
  UiDispatcher dispatcher_;
  engine::PdfiumWorker worker_;
};

TEST_F(PrintPreviewTest, SheetsFollowRangeAndCopies) {
  auto handle = open("mixed-sizes.pdf");
  ASSERT_TRUE(handle.valid());
  ASSERT_GE(handle.info().page_count, 3);
  auto printer = this->printer();
  QPageRanges ranges;
  ranges.addRange(2, 3);
  printer->setPageRanges(ranges);
  printer->setPrintRange(QPrinter::PageRange);
  printer->setCopyCount(2);  // PDF 출력은 매수를 지원하지 않아 요청이 장을 반복한다
  printer->setCollateCopies(true);
  auto dialog = make_dialog(handle, *printer);
  PrintPreviewView* view = dialog->view();
  ASSERT_EQ(view->sheet_count(), 4);
  EXPECT_EQ(view->sheet_page(0), 1);
  EXPECT_EQ(view->sheet_page(1), 2);
  EXPECT_EQ(view->sheet_page(2), 1);
  EXPECT_EQ(view->sheet_page(3), 2);
  EXPECT_EQ(view->sheet_page(4), -1);
  ASSERT_TRUE(settle(view));
  EXPECT_GE(view->cached_count(), 1);
  EXPECT_LE(view->requested_total(), 2) << "쪽 두 개뿐이라 요청도 둘을 넘지 않는다";
  if (const char* out = std::getenv("ONPDF_PREVIEW_FIT_PNG"))
    dialog->grab().save(QString::fromUtf8(out));
  // 한 부씩이 아니면 쪽마다 반복
  printer->setCollateCopies(false);
  dialog->set_request(make_print_request(*printer, handle.info(), 0));
  ASSERT_EQ(view->sheet_count(), 4);
  EXPECT_EQ(view->sheet_page(1), 1);
  EXPECT_EQ(view->sheet_page(2), 2);
}

TEST_F(PrintPreviewTest, PlacementMatchesPrintLayoutAndScalingDoesNotRerender) {
  auto handle = open("mixed-sizes.pdf");
  ASSERT_TRUE(handle.valid());
  auto printer = this->printer();
  auto dialog = make_dialog(handle, *printer);
  PrintPreviewView* view = dialog->view();
  ASSERT_TRUE(settle(view));
  const int before = view->requested_total();
  const engine::PageSize printable = printable_of(*printer);
  const auto& sizes = handle.info().page_sizes;
  bool saw_rotation = false;
  for (int sheet = 0; sheet < view->sheet_count(); ++sheet) {
    const auto placement = view->placement_of(sheet);
    ASSERT_TRUE(placement) << sheet;
    const auto expected = view::place_page(sizes[static_cast<std::size_t>(view->sheet_page(sheet))],
                                           printable, view::PrintScaling::kFit);
    ASSERT_TRUE(expected);
    EXPECT_EQ(placement->rotation, expected->rotation);
    EXPECT_DOUBLE_EQ(placement->scale, expected->scale);
    EXPECT_DOUBLE_EQ(placement->target.w, expected->target.w);
    saw_rotation = saw_rotation || placement->rotation != 0;
  }
  EXPECT_TRUE(saw_rotation) << "mixed-sizes 의 가로 쪽은 자동 회전된다";
  // 실제 크기로 바꾸면 배치만 바뀐다 (렌더 요청 불변)
  dialog->scaling_combo()->setCurrentIndex(
      dialog->scaling_combo()->findData(static_cast<int>(view::PrintScaling::kActualSize)));
  ASSERT_TRUE(settle(view));
  EXPECT_EQ(view->requested_total(), before);
  const auto actual = view->placement_of(0);
  const auto expected_actual =
      view::place_page(sizes[static_cast<std::size_t>(view->sheet_page(0))], printable,
                       view::PrintScaling::kActualSize);
  ASSERT_TRUE(actual && expected_actual);
  EXPECT_DOUBLE_EQ(actual->scale, 1.0);
  EXPECT_DOUBLE_EQ(actual->target.x, expected_actual->target.x);
  dialog->grayscale_check()->setChecked(true);
  ASSERT_TRUE(settle(view));
  EXPECT_EQ(view->requested_total(), before) << "흑백도 그리기 때만 바뀐다";
  // 눈으로 확인용: ONPDF_PREVIEW_PNG=<경로> 면 실제 크기·흑백 상태의 창을 저장한다
  if (const char* out = std::getenv("ONPDF_PREVIEW_PNG"))
    dialog->grab().save(QString::fromUtf8(out));
}

TEST_F(PrintPreviewTest, AcceptReturnsChosenOptions) {
  auto handle = open("simple.pdf");
  ASSERT_TRUE(handle.valid());
  auto printer = this->printer();
  auto dialog = make_dialog(handle, *printer);
  EXPECT_EQ(dialog->request().scaling, view::PrintScaling::kFit);
  EXPECT_FALSE(dialog->request().grayscale);
  dialog->scaling_combo()->setCurrentIndex(
      dialog->scaling_combo()->findData(static_cast<int>(view::PrintScaling::kShrinkOversized)));
  dialog->grayscale_check()->setChecked(true);
  dialog->accept();
  EXPECT_EQ(dialog->result(), QDialog::Accepted);
  const PrintJob::Request request = dialog->request();
  EXPECT_EQ(request.scaling, view::PrintScaling::kShrinkOversized);
  EXPECT_TRUE(request.grayscale);
  EXPECT_EQ(request.steps.size(), 2u) << "범위·매수는 그대로 실린다";
}

TEST_F(PrintPreviewTest, LargeDocumentRendersOnlyVisibleSheets) {
  auto handle = open("large-1000.pdf");
  ASSERT_TRUE(handle.valid());
  ASSERT_EQ(handle.info().page_count, 1000);
  auto printer = this->printer();
  QElapsedTimer timer;
  timer.start();
  auto dialog = make_dialog(handle, *printer);
  EXPECT_LT(timer.elapsed(), 300) << "미리보기 창은 전 쪽을 그리지 않고 바로 뜬다";
  PrintPreviewView* view = dialog->view();
  EXPECT_EQ(view->sheet_count(), 1000);
  ASSERT_TRUE(settle(view));
  const int first_batch = view->requested_total();
  EXPECT_LE(first_batch, PrintPreviewView::kMaxConcurrent);
  EXPECT_GE(first_batch, 1);
  // 끝으로 스크롤: 마지막 장들만 추가로 요청한다
  view->verticalScrollBar()->setValue(view->verticalScrollBar()->maximum());
  ASSERT_TRUE(settle(view));
  EXPECT_LE(view->requested_total() - first_batch, PrintPreviewView::kMaxConcurrent + 2);
  EXPECT_LE(view->pending_requests(), PrintPreviewView::kMaxConcurrent);
}

// 창 크기를 조금 바꾸면 옛 이미지를 늘려 쓰고(세대 유지), 크게 바꾸면 디바운스 뒤 한 번만 다시
// 렌더한다 (#101)
TEST_F(PrintPreviewTest, SmallResizeKeepsRenders) {
  auto handle = open("simple.pdf");
  ASSERT_TRUE(handle.valid());
  auto printer = this->printer();
  auto dialog = make_dialog(handle, *printer);
  PrintPreviewView* view = dialog->view();
  ASSERT_TRUE(settle(view));
  const int generation = view->generation();
  const int requested = view->requested_total();
  dialog->resize(dialog->width() + 20, dialog->height());  // ≈3%
  ASSERT_TRUE(settle(view));
  EXPECT_EQ(view->generation(), generation) << "작은 변화는 캐시를 비우지 않는다";
  EXPECT_EQ(view->requested_total(), requested);
  dialog->resize(dialog->width() * 2, dialog->height());  // 100%
  ASSERT_TRUE(settle(view));
  EXPECT_EQ(view->generation(), generation + 1) << "큰 변화는 디바운스 뒤 한 번";
  EXPECT_GT(view->requested_total(), requested);
}

TEST_F(PrintPreviewTest, SetRequestRebuildsSheetsAndDropsOldRenders) {
  auto handle = open("mixed-sizes.pdf");
  ASSERT_TRUE(handle.valid());
  auto printer = this->printer();
  auto dialog = make_dialog(handle, *printer);
  PrintPreviewView* view = dialog->view();
  ASSERT_TRUE(settle(view));
  const int generation = view->generation();
  PrintJob::Request request = dialog->request();
  request.steps = {{0, 1}};
  dialog->set_request(std::move(request));
  EXPECT_EQ(view->sheet_count(), 1);
  EXPECT_GT(view->generation(), generation) << "용지·장 목록이 바뀌면 늦은 렌더를 버린다";
  ASSERT_TRUE(settle(view));
  EXPECT_GE(view->cached_count(), 1);
}

}  // namespace
}  // namespace onpdf::app
