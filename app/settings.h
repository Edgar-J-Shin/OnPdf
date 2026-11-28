#pragma once

// 사용자 설정 저장소 (#129). QSettings 를 감싸 키 스키마(v1)·기본값·범위를 한 곳에 둔다.
//  - getter 는 기본값과 clamp/snap 을 내장하므로 호출자는 값을 검증하지 않는다
//  - setter 는 바로 저장하고 값이 실제로 바뀌었을 때만 changed() 를 낸다
//  - 저장 위치는 Qt 기본(Windows 레지스트리, macOS plist — main 이 조직 이름을 지정). 테스트·이식은
//    set_default_path() 로 INI 파일 경로를 주면 프로세스 안의 모든 Settings 가 그 파일을 쓴다
//  - 창 상태·최근 파일·마지막 폴더는 "설정" 이 아니라 상태라 reset_preferences() 가 지우지 않는다

#include "onpdf/core/engine/annotation.h"

#include "crash_report.h"
#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>
#include <memory>

class QSettings;

namespace onpdf::app {

enum class LanguageChoice { kSystem, kKorean, kEnglish };
enum class StartZoom { kFitWidth, kFitPage, kActualSize, kLast };
enum class ZoomKind { kFitWidth, kFitPage, kCustom };  // DocumentView::ZoomMode 와 1:1 (Qt 무관)
enum class SidebarStart { kVisible, kHidden, kLast };
enum class DirKind { kOpen, kSave, kInsert, kImage, kMerge, kSplit };

class Settings : public QObject {
  Q_OBJECT

 public:
  explicit Settings(QObject* parent = nullptr);
  ~Settings() override;

  // 프로세스 전역 기본 경로. 비어 있으면(기본) 네이티브 저장소. 테스트 진입점이 한 번 부른다
  static void set_default_path(const QString& ini_path);
  [[nodiscard]] static QString default_path();

  // ---- 일반 ----
  [[nodiscard]] LanguageChoice language() const;
  void set_language(LanguageChoice choice);
  [[nodiscard]] StartZoom start_zoom() const;
  void set_start_zoom(StartZoom zoom);
  [[nodiscard]] ZoomKind last_zoom_kind() const;
  [[nodiscard]] double last_zoom() const;  // 0.25~8
  void set_last_zoom(ZoomKind kind, double zoom);
  [[nodiscard]] SidebarStart sidebar_start() const;
  void set_sidebar_start(SidebarStart start);
  [[nodiscard]] bool sidebar_visible() const;  // 마지막 상태
  void set_sidebar_visible(bool visible);

  // ---- 편집 기본값 ----
  [[nodiscard]] double border_width() const;  // {1,2,4,8}
  void set_border_width(double width);
  [[nodiscard]] int opacity_percent() const;  // {25,50,75,100}
  void set_opacity_percent(int percent);
  [[nodiscard]] std::uint8_t opacity_alpha() const;  // percent → 0~255 (툴바와 같은 반올림)
  [[nodiscard]] engine::Color annotation_color() const;
  void set_annotation_color(engine::Color color);
  [[nodiscard]] int text_size() const;  // {8,10,12,14,18,24,36,48,72}
  void set_text_size(int size);
  [[nodiscard]] engine::Color text_color() const;
  void set_text_color(engine::Color color);
  [[nodiscard]] QString author() const;  // 앞뒤 공백 제거, 256자
  void set_author(const QString& author);

  // ---- 고급 ----
  [[nodiscard]] int cache_mb() const;  // 64~2048
  void set_cache_mb(int mb);
  [[nodiscard]] bool hazard_notice() const;
  void set_hazard_notice(bool on);
  [[nodiscard]] int stall_seconds() const;  // 10~600
  void set_stall_seconds(int seconds);
  [[nodiscard]] bool crash_notice() const;  // 비정상 종료 안내 (#131)
  void set_crash_notice(bool on);

  // ---- 세션 표식 (#131): 상태라 changed 를 내지 않고 reset_preferences 도 건드리지 않는다 ----
  [[nodiscard]] SessionMarker session_marker() const;  // 없으면 valid() == false
  void set_session_marker(const SessionMarker& marker);
  void set_session_stage(const QString& stage);
  void set_session_last_action(const QString& action);
  void clear_session_marker();

  // ---- 창 상태 ----
  [[nodiscard]] QByteArray window_geometry() const;
  void set_window_geometry(const QByteArray& geometry);
  [[nodiscard]] QByteArray splitter_state() const;
  void set_splitter_state(const QByteArray& state);

  // ---- 파일 ----
  [[nodiscard]] QString last_dir(DirKind kind) const;  // 존재하는 폴더만, 없으면 빈 문자열
  void set_last_dir(DirKind kind, const QString& dir);
  [[nodiscard]] QStringList recent_files();  // 없는 파일은 걸러 내고 저장본도 갱신. 최근이 앞
  void add_recent(const QString& path);      // 절대 경로로 정규화, 중복 제거, 최대 10
  void clear_recent();
  static constexpr int kMaxRecent = 10;

  void reset_preferences();  // 일반·편집·고급 → 기본값 ("기본값으로")
  void reset_all();          // 전부 (테스트)
  void sync();
  [[nodiscard]] bool writable() const;  // 저장소를 열고 쓸 수 있다

  // 기본값 (대화상자 "기본값으로" 가 위젯에 넣는다)
  static constexpr double kDefaultBorderWidth = 2.0;
  static constexpr int kDefaultOpacityPercent = 100;
  static constexpr int kDefaultTextSize = 24;
  static constexpr int kDefaultCacheMb = 256;
  static constexpr int kDefaultStallSeconds = 60;
  static constexpr int kMinCacheMb = 64;
  static constexpr int kMaxCacheMb = 2048;
  static constexpr int kMinStallSeconds = 10;
  static constexpr int kMaxStallSeconds = 600;
  static constexpr engine::Color kDefaultAnnotationColor{255, 255, 0, 255};
  static constexpr engine::Color kDefaultTextColor{0, 0, 0, 255};

  // "#RRGGBB" ↔ Color (알파 255). 대화상자·테스트도 쓴다
  [[nodiscard]] static QString color_to_string(engine::Color color);
  [[nodiscard]] static engine::Color color_from_string(const QString& text, engine::Color fallback);

 signals:
  void changed();

 private:
  [[nodiscard]] QString read_string(const char* key, const QString& fallback) const;
  [[nodiscard]] int read_int(const char* key, int fallback) const;
  [[nodiscard]] double read_double(const char* key, double fallback) const;
  [[nodiscard]] bool read_bool(const char* key, bool fallback) const;
  template <class T>
  void write(const char* key, const T& value);  // 같으면 쓰지 않는다. 바뀌면 changed()

  std::unique_ptr<QSettings> store_;
};

}  // namespace onpdf::app
