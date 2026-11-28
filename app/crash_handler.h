#pragma once

// 프로세스 크래시 핸들러 (#131). Qt 를 쓰지 않는다 — 핸들러는 크래시가 난 스레드(워커 포함)에서
// 비동기 신호 안전한 호출만으로 기록 파일(crash-<epoch>.txt: 버전·PDFium 판·신호/예외·마지막
// 동작)을 쓰고 기본 처리로 넘긴다(macOS 시스템 리포트·Windows WER 그대로). Windows 는 이어서
// 미니덤프 (crash-<epoch>.dmp, dbghelp MiniDumpWriteDump)도 남긴다. 플랫폼 분기는 crash_handler.cpp
// 안에만.
//
//  - install() 은 평상시(main, 워커 생성 전)에 한 번: 보고 폴더를 만들고 경로·버전을 고정 버퍼에
//    복사한다. 테스트 프로세스는 설치하지 않는다
//  - set_last_action() 은 메인 스레드가 짧은 진단 문자열("render p.3", "save")을 고정 버퍼에 둔다.
//    핸들러는 읽기만 하며 찢긴 읽기는 허용한다(진단용)
//  - format_record()·prune_reports() 는 순수 함수라 테스트한다

#include <cstddef>
#include <filesystem>
#include <string>

namespace onpdf::app::crash {

struct InstallInfo {
  std::filesystem::path report_dir;
  std::string app_version;
  std::string pdfium_version;
};

// 한 번만 설치된다. 보고 폴더를 만들 수 없거나 이미 설치됐으면 false (앱은 정상 동작)
bool install(const InstallInfo& info);
[[nodiscard]] bool installed() noexcept;

// 64바이트 상한, 메인 스레드만. nullptr 또는 빈 문자열은 지운다
void set_last_action(const char* utf8) noexcept;

struct RecordFields {
  const char* version = "";
  const char* pdfium = "";
  bool is_exception = false;  // true: Windows 예외 코드(16진), false: POSIX 신호 번호
  long signal_or_code = 0;
  const char* last_action = "";
  long long epoch = 0;  // UTC 초
};

// "version=…\npdfium=…\nsignal=SIGTRAP(5)\n" 또는 "exception=0x80000003\n" +
// "last_action=…\ntime=…\n". 비동기 신호 안전(힙·locale 없음). cap 을 넘기지 않고 항상 널 종료.
// 돌려주는 값은 쓴 길이
std::size_t format_record(char* out, std::size_t cap, const RecordFields& fields) noexcept;

// 신호 번호 → 이름("SIGSEGV" …). 모르면 "SIG?"
const char* signal_name(int signal) noexcept;

// 보고 폴더의 crash-<epoch>.txt/.dmp 가운데 가장 최근 keep 묶음(같은 epoch 의 txt·dmp 는 한 묶음)만
// 남기고 지운다. 평상시(설치 직후) 호출. 지운 파일 수
int prune_reports(const std::filesystem::path& dir, int keep);

}  // namespace onpdf::app::crash
