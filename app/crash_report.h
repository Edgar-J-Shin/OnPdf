#pragma once

// 충돌 보고 (#131) 의 Qt 쪽 순수 유틸: 세션 표식(설정에 저장)·크래시 기록 읽기·보고 폴더·GitHub
// 이슈 URL. MainWindow 가 안내 창을 만들 때 쓰고 테스트가 직접 검증한다. 핸들러 자체는
// crash_handler.h (Qt 없음)

#include <QDateTime>
#include <QDir>
#include <QString>
#include <QUrl>
#include <optional>

namespace onpdf::app {

// 문서를 여는/보는 동안 설정(session/*)에 남는 표식. 정상 종료·닫기 때 지워지므로 다음 실행 때
// 남아 있으면 비정상 종료다
struct SessionMarker {
  QString path;         // 절대 경로
  QString stage;        // "opening" | "viewing" | "forced-quit"
  QDateTime since;      // 표식을 남긴 시각 (로컬)
  QString last_action;  // "open", "view", "render p.3", "save", "edit", "print"
  QString version;      // 남긴 앱 버전
  [[nodiscard]] bool valid() const;
};

// 핸들러가 남긴 crash-<epoch>.txt
struct CrashRecord {
  QDateTime time;
  QString signal_text;  // "SIGTRAP(5)" 또는 "0x80000003"
  QString last_action;
  QString version;
  QString pdfium;
};

// 보고 폴더: override 가 있으면 그것, 아니면 AppLocalDataLocation/crash-reports
[[nodiscard]] QDir report_dir();
void set_report_dir_override(const QString& dir);  // 테스트·--self-test
// since 이후(포함)의 가장 최근 기록. 없으면 nullopt
[[nodiscard]] std::optional<CrashRecord> latest_record_since(const QDir& dir,
                                                             const QDateTime& since);
[[nodiscard]] std::optional<CrashRecord> parse_record(const QString& text,
                                                      const QDateTime& fallback);
[[nodiscard]] QString pdfium_version();  // 빌드에 고정한 PDFium 판 (chromium/NNNN)
[[nodiscard]] QString app_version();
// GitHub 새 이슈 URL. 버전·OS·Qt·PDFium·단계·신호·마지막 동작을 본문에 넣고 파일 경로·이름은 넣지
// 않는다
[[nodiscard]] QUrl issue_url(const SessionMarker& marker, const std::optional<CrashRecord>& record);
// 단계 → 사람이 읽는 문구 ("opening a file" …, 번역 문맥 MainWindow)
[[nodiscard]] QString stage_text(const QString& stage);

}  // namespace onpdf::app
