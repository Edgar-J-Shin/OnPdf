#include "onpdf/edit/page_ops.h"

#include "../fake_document.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <numeric>
#include <random>
#include <vector>

namespace onpdf::edit {
namespace {

using onpdf::test::FakeDocument;

TEST(PageOps, NormalizeSortsAndDedups) {
  EXPECT_EQ(normalize_pages({3, 1, 3, 0, 1}), (std::vector<int>{0, 1, 3}));
  EXPECT_TRUE(normalize_pages({}).empty());
}

// PDFium 헤더(fpdf_edit.h)의 예시: [A,B,C,D] 에서 Move([3,2], dest 1) → [A,D,C,B]
TEST(PageOps, SimulateMoveMatchesPdfiumExamples) {
  EXPECT_EQ(simulate_move(4, {3, 2}, 1), (std::vector<int>{0, 3, 2, 1}));
  EXPECT_EQ(simulate_move(4, {0}, 3), (std::vector<int>{1, 2, 3, 0}));
  EXPECT_EQ(simulate_move(4, {1, 3}, 0), (std::vector<int>{1, 3, 0, 2}));
  EXPECT_EQ(simulate_move(4, {1, 2}, 1), (std::vector<int>{0, 1, 2, 3}));  // 제자리
}

TEST(PageOps, ValidateMoveRejectsBadArguments) {
  EXPECT_TRUE(validate_move(4, {3, 2}, 1));
  EXPECT_FALSE(validate_move(4, {}, 0));
  EXPECT_FALSE(validate_move(4, {4}, 0));        // 범위
  EXPECT_FALSE(validate_move(4, {2, 2}, 0));     // 중복
  EXPECT_FALSE(validate_move(4, {0, 1, 2}, 2));  // dest > count − n
  EXPECT_TRUE(validate_move(4, {0, 1, 2}, 1));
  EXPECT_FALSE(validate_move(4, {0}, -1));
}

TEST(PageOps, InvertOrderIsInverse) {
  const std::vector<int> order{2, 0, 3, 1};
  const auto inv = invert_order(order);
  for (std::size_t i = 0; i < order.size(); ++i) EXPECT_EQ(inv[static_cast<std::size_t>(order[i])], static_cast<int>(i));
}

// apply_order 가 가짜 문서를 정확히 target 순서로 만들고, 되돌리기(역순열)로 원래로 온다 — 무작위 순열 100회
TEST(PageOps, ApplyOrderRandomPermutationsRoundTrip) {
  std::mt19937 rng(1234);
  for (int trial = 0; trial < 100; ++trial) {
    const int n = 1 + static_cast<int>(rng() % 9);
    std::vector<int> target(static_cast<std::size_t>(n));
    std::iota(target.begin(), target.end(), 0);
    std::shuffle(target.begin(), target.end(), rng);
    FakeDocument doc(std::vector<engine::PageSize>(static_cast<std::size_t>(n), engine::PageSize{100, 200}));
    ASSERT_TRUE(apply_order(doc, target));
    EXPECT_EQ(doc.labels(), target) << "trial " << trial;
    EXPECT_LE(doc.move_calls, n);
    ASSERT_TRUE(apply_order(doc, invert_order(target)));
    std::vector<int> identity(static_cast<std::size_t>(n));
    std::iota(identity.begin(), identity.end(), 0);
    EXPECT_EQ(doc.labels(), identity) << "trial " << trial;
  }
}

TEST(PageOps, ApplyOrderRejectsNonPermutation) {
  FakeDocument doc({{1, 1}, {2, 2}, {3, 3}});
  EXPECT_FALSE(apply_order(doc, {0, 0, 1}));
  EXPECT_FALSE(apply_order(doc, {0, 1}));
}

TEST(PageOps, StructuralChangeListsAllSizes) {
  FakeDocument doc({{100, 200}, {300, 400}});
  auto change = structural_change(doc);
  ASSERT_TRUE(change);
  EXPECT_TRUE(change->structural);
  ASSERT_EQ(change->all_sizes.size(), 2u);
  EXPECT_DOUBLE_EQ(change->all_sizes[1].width, 300);
  EXPECT_TRUE(change->pages.empty());
  EXPECT_EQ(change->focus_page, -1);
  EXPECT_EQ(structural_change(doc, 7)->focus_page, 1);  // 새 범위로 클램프
  EXPECT_EQ(structural_change(doc, 0)->focus_page, 0);
}

}  // namespace
}  // namespace onpdf::edit
