#pragma once

// 주석·텍스트 색 팔레트 (툴바 색 버튼과 설정 대화상자가 공유, #129). 번역 문맥은 MainWindow — 두 곳
// 모두 MainWindow::tr(kPaletteNames[i]) 로 번역한다

#include "onpdf/core/engine/annotation.h"

#include <QtGlobal>

namespace onpdf::app {

inline constexpr int kPaletteSize = 6;

inline constexpr engine::Color kPalette[kPaletteSize] = {
    {255, 255, 0, 255},   {0, 200, 0, 255}, {0, 150, 255, 255},
    {255, 105, 180, 255}, {255, 0, 0, 255}, {255, 140, 0, 255},
};

inline const char* const kPaletteNames[kPaletteSize] = {
    QT_TRANSLATE_NOOP("onpdf::app::MainWindow", "Yellow"),
    QT_TRANSLATE_NOOP("onpdf::app::MainWindow", "Green"),
    QT_TRANSLATE_NOOP("onpdf::app::MainWindow", "Blue"),
    QT_TRANSLATE_NOOP("onpdf::app::MainWindow", "Pink"),
    QT_TRANSLATE_NOOP("onpdf::app::MainWindow", "Red"),
    QT_TRANSLATE_NOOP("onpdf::app::MainWindow", "Orange"),
};

}  // namespace onpdf::app
