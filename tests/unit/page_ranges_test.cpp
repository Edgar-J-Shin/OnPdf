#include "onpdf/core/page_ranges.h"

#include <gtest/gtest.h>

namespace onpdf {
namespace {

TEST(PageRanges, ParsesListsAndOpenEnds) {
  auto r = parse_page_ranges("1-3, 5, 8-", 10);
  ASSERT_TRUE(r) << r.error().message;
  ASSERT_EQ(r->size(), 3u);
  EXPECT_EQ((*r)[0], (PageRange{1, 3}));
  EXPECT_EQ((*r)[1], (PageRange{5, 5}));
  EXPECT_EQ((*r)[2], (PageRange{8, 10}));
  EXPECT_TRUE(parse_page_ranges("10", 10));
  EXPECT_TRUE(parse_page_ranges(" 2 - 4 ", 10));
}

TEST(PageRanges, RejectsBadInput) {
  for (const char* bad : {"", "0", "3-2", "11", "1-11", "a", "1,,3", "-3", "2-x"}) {
    auto r = parse_page_ranges(bad, 10);
    EXPECT_FALSE(r) << bad;
    if (!r) EXPECT_EQ(r.error().code, ErrorCode::kInvalidArgument) << bad;
  }
  // "N-" 는 끝까지
  auto open = parse_page_ranges("8-", 10);
  ASSERT_TRUE(open);
  EXPECT_EQ((*open)[0].last, 10);
  EXPECT_FALSE(parse_page_ranges("1", 0));
}

TEST(PageRanges, EveryAndEach) {
  auto every = ranges_every(3, 10);
  ASSERT_EQ(every.size(), 4u);
  EXPECT_EQ(every[0], (PageRange{1, 3}));
  EXPECT_EQ(every[3], (PageRange{10, 10}));
  EXPECT_EQ(ranges_each(3).size(), 3u);
  EXPECT_TRUE(ranges_every(0, 10).empty());
  EXPECT_TRUE(ranges_every(3, 0).empty());
  EXPECT_EQ(ranges_every(100, 10).size(), 1u);
}

TEST(PageRanges, OutputNames) {
  EXPECT_EQ(output_name("doc-%d.pdf", 2, 12, {4, 6}, 36), "doc-02.pdf");
  EXPECT_EQ(output_name("doc-%r.pdf", 2, 12, {4, 6}, 36), "doc-p004-006.pdf");
  EXPECT_EQ(output_name("doc.pdf", 7, 7, {7, 7}, 7), "doc-7.pdf");  // 패턴 없음 → 확장자 앞에 -번호
  EXPECT_EQ(output_name("noext", 1, 1, {1, 1}, 1), "noext-1");
  EXPECT_EQ(output_name("%d-%r.pdf", 1, 100, {1, 1}, 1000), "001-p0001-0001.pdf");
  EXPECT_EQ(range_text({3, 3}), "3");
  EXPECT_EQ(range_text({3, 7}), "3-7");
}

}  // namespace
}  // namespace onpdf
