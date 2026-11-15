#include "onpdf/core/geometry.h"

#include <gtest/gtest.h>

namespace onpdf {
namespace {

TEST(Rect, IntersectionAndContainment) {
  const Rect a{0, 0, 10, 10};
  const Rect b{5, 5, 10, 10};
  EXPECT_TRUE(a.intersects(b));
  EXPECT_EQ(a.intersected(b), (Rect{5, 5, 5, 5}));
  EXPECT_TRUE(a.contains({0, 0}));
  EXPECT_FALSE(a.contains({10, 10}));  // 오른쪽·아래 경계는 제외
}

TEST(Rect, DisjointAndEmpty) {
  const Rect a{0, 0, 10, 10};
  EXPECT_FALSE(a.intersects({10, 0, 5, 5}));  // 변만 맞닿음
  EXPECT_TRUE(a.intersected({20, 20, 5, 5}).empty());
  EXPECT_FALSE(a.intersects(Rect{}));
  EXPECT_EQ(a.translated(3, -2), (Rect{3, -2, 10, 10}));
}

}  // namespace
}  // namespace onpdf
