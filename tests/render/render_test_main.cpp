// 렌더 테스트 진입점: --render-child 면 자식 본체(#137) — 자식 경로 렌더 회귀가 이 바이너리를
// 자식으로 띄운다. 그 밖에는 gtest (전역 PdfiumEnvironment 는 test_support.h 가 등록한다)

#include "onpdf/core/engine/render_child.h"

#include <cstring>
#include <gtest/gtest.h>

int main(int argc, char** argv) {
  if (argc >= 2 && std::strcmp(argv[1], "--render-child") == 0) {
    return onpdf::engine::run_render_child(argc, argv);
  }
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
