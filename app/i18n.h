#pragma once

// 앱 UI 언어 (#8). 원문은 영어이고 한국어는 번역(:/i18n/onpdf_ko.qm + Qt 표준 문구
// qtbase_ko.qm)이다. 영어는 복수형만 번역(:/i18n/onpdf_en.qm)으로 처리한다. 위젯을 만들기 전에 메인
// 스레드에서 정한다 (실행 중 재번역은 하지 않는다).

#include <QCoreApplication>
#include <QStringList>
#include <QStringView>
#include <optional>

namespace onpdf::app {

enum class Language { kEnglish, kKorean };

// 선호 언어 목록(QLocale::uiLanguages 순서)에서 처음 나오는 ko/en. 둘 다 없으면 영어
[[nodiscard]] Language language_from_locale(const QStringList& ui_languages);
// "ko", "ko-KR", "EN_us" … → 언어. 지원하지 않으면 nullopt
[[nodiscard]] std::optional<Language> parse_language(QStringView tag);
// 이전에 설치한 번역기를 걷어 내고 lang 의 번역기를 설치한다. 번역 파일을 읽지 못하면 false
// (영어 원문으로 계속 동작한다)
bool set_language(QCoreApplication& app, Language lang);
[[nodiscard]] Language current_language() noexcept;

}  // namespace onpdf::app
