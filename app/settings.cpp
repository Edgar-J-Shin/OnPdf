#include "settings.h"

#include <QColor>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QVariant>
#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace onpdf::app {

namespace {

constexpr int kSchemaVersion = 1;

constexpr const char* kKeySchema = "schema";
constexpr const char* kKeyLanguage = "general/language";
constexpr const char* kKeyStartZoom = "general/start_zoom";
constexpr const char* kKeyLastZoomKind = "general/last_zoom_mode";
constexpr const char* kKeyLastZoom = "general/last_zoom";
constexpr const char* kKeySidebarStart = "general/sidebar";
constexpr const char* kKeyBorderWidth = "edit/border_width";
constexpr const char* kKeyOpacity = "edit/opacity";
constexpr const char* kKeyAnnotationColor = "edit/annotation_color";
constexpr const char* kKeyTextSize = "edit/text_size";
constexpr const char* kKeyTextColor = "edit/text_color";
constexpr const char* kKeyAuthor = "edit/author";
constexpr const char* kKeyCacheMb = "advanced/cache_mb";
constexpr const char* kKeyHazardNotice = "advanced/hazard_notice";
constexpr const char* kKeyStallSeconds = "advanced/stall_seconds";
constexpr const char* kKeyCrashNotice = "advanced/crash_notice";
constexpr const char* kKeyRenderProcess = "advanced/render_process";
constexpr const char* kKeyRenderTimeout = "advanced/render_timeout_seconds";
constexpr const char* kKeySessionPath = "session/path";
constexpr const char* kKeySessionStage = "session/stage";
constexpr const char* kKeySessionSince = "session/since";
constexpr const char* kKeySessionAction = "session/last_action";
constexpr const char* kKeySessionVersion = "session/version";
constexpr const char* kKeyGeometry = "window/geometry";
constexpr const char* kKeySplitter = "window/splitter";
constexpr const char* kKeySidebarVisible = "window/sidebar_visible";
constexpr const char* kKeyRecent = "files/recent";

QString& default_path_storage() {
  static QString path;
  return path;
}

// 허용 목록에서 가장 가까운 값 (중간이면 작은 쪽)
template <class T>
T snap(T value, std::initializer_list<T> allowed) {
  T best = *allowed.begin();
  for (T a : allowed) {
    if (std::abs(static_cast<double>(a) - static_cast<double>(value)) <
        std::abs(static_cast<double>(best) - static_cast<double>(value)))
      best = a;
  }
  return best;
}

const char* dir_key(DirKind kind) {
  switch (kind) {
    case DirKind::kOpen:
      return "files/last_dir/open";
    case DirKind::kSave:
      return "files/last_dir/save";
    case DirKind::kInsert:
      return "files/last_dir/insert";
    case DirKind::kImage:
      return "files/last_dir/image";
    case DirKind::kMerge:
      return "files/last_dir/merge";
    case DirKind::kSplit:
      return "files/last_dir/split";
  }
  return "files/last_dir/open";
}

}  // namespace

Settings::Settings(QObject* parent) : QObject(parent) {
  const QString path = default_path();
  store_ = path.isEmpty() ? std::make_unique<QSettings>()
                          : std::make_unique<QSettings>(path, QSettings::IniFormat);
  if (!store_->contains(QLatin1String(kKeySchema))) {
    store_->setValue(QLatin1String(kKeySchema), kSchemaVersion);
  }
}

Settings::~Settings() {
  store_->sync();
}

void Settings::set_default_path(const QString& ini_path) {
  default_path_storage() = ini_path;
}

QString Settings::default_path() {
  return default_path_storage();
}

// ---------------------------------------------------------------- 읽기·쓰기

QString Settings::read_string(const char* key, const QString& fallback) const {
  const QVariant v = store_->value(QLatin1String(key));
  return v.isValid() && v.canConvert<QString>() ? v.toString() : fallback;
}

int Settings::read_int(const char* key, int fallback) const {
  const QVariant v = store_->value(QLatin1String(key));
  bool ok = false;
  const int n = v.toInt(&ok);
  return ok ? n : fallback;
}

double Settings::read_double(const char* key, double fallback) const {
  const QVariant v = store_->value(QLatin1String(key));
  bool ok = false;
  const double d = v.toDouble(&ok);
  return ok && std::isfinite(d) ? d : fallback;
}

bool Settings::read_bool(const char* key, bool fallback) const {
  const QVariant v = store_->value(QLatin1String(key));
  if (!v.isValid()) return fallback;
  const QString s = v.toString().trimmed().toLower();
  if (s == QLatin1String("true") || s == QLatin1String("1")) return true;
  if (s == QLatin1String("false") || s == QLatin1String("0")) return false;
  return fallback;
}

template <class T>
void Settings::write(const char* key, const T& value) {
  const QVariant next = QVariant::fromValue(value);
  if (store_->contains(QLatin1String(key)) && store_->value(QLatin1String(key)) == next) return;
  store_->setValue(QLatin1String(key), next);
  emit changed();
}

// ---------------------------------------------------------------- 일반

LanguageChoice Settings::language() const {
  const QString s = read_string(kKeyLanguage, QStringLiteral("system")).toLower();
  if (s == QLatin1String("ko")) return LanguageChoice::kKorean;
  if (s == QLatin1String("en")) return LanguageChoice::kEnglish;
  return LanguageChoice::kSystem;
}

void Settings::set_language(LanguageChoice choice) {
  const char* s = choice == LanguageChoice::kKorean    ? "ko"
                  : choice == LanguageChoice::kEnglish ? "en"
                                                       : "system";
  write(kKeyLanguage, QString::fromLatin1(s));
}

StartZoom Settings::start_zoom() const {
  const QString s = read_string(kKeyStartZoom, QStringLiteral("fit_width")).toLower();
  if (s == QLatin1String("fit_page")) return StartZoom::kFitPage;
  if (s == QLatin1String("actual")) return StartZoom::kActualSize;
  if (s == QLatin1String("last")) return StartZoom::kLast;
  return StartZoom::kFitWidth;
}

void Settings::set_start_zoom(StartZoom zoom) {
  const char* s = zoom == StartZoom::kFitPage      ? "fit_page"
                  : zoom == StartZoom::kActualSize ? "actual"
                  : zoom == StartZoom::kLast       ? "last"
                                                   : "fit_width";
  write(kKeyStartZoom, QString::fromLatin1(s));
}

ZoomKind Settings::last_zoom_kind() const {
  const QString s = read_string(kKeyLastZoomKind, QStringLiteral("fit_width")).toLower();
  if (s == QLatin1String("fit_page")) return ZoomKind::kFitPage;
  if (s == QLatin1String("custom")) return ZoomKind::kCustom;
  return ZoomKind::kFitWidth;
}

double Settings::last_zoom() const {
  return std::clamp(read_double(kKeyLastZoom, 1.0), 0.25, 8.0);
}

void Settings::set_last_zoom(ZoomKind kind, double zoom) {
  const char* s = kind == ZoomKind::kFitPage  ? "fit_page"
                  : kind == ZoomKind::kCustom ? "custom"
                                              : "fit_width";
  write(kKeyLastZoomKind, QString::fromLatin1(s));
  if (std::isfinite(zoom)) write(kKeyLastZoom, std::clamp(zoom, 0.25, 8.0));
}

SidebarStart Settings::sidebar_start() const {
  const QString s = read_string(kKeySidebarStart, QStringLiteral("last")).toLower();
  if (s == QLatin1String("visible")) return SidebarStart::kVisible;
  if (s == QLatin1String("hidden")) return SidebarStart::kHidden;
  return SidebarStart::kLast;
}

void Settings::set_sidebar_start(SidebarStart start) {
  const char* s = start == SidebarStart::kVisible  ? "visible"
                  : start == SidebarStart::kHidden ? "hidden"
                                                   : "last";
  write(kKeySidebarStart, QString::fromLatin1(s));
}

bool Settings::sidebar_visible() const {
  return read_bool(kKeySidebarVisible, true);
}

void Settings::set_sidebar_visible(bool visible) {
  write(kKeySidebarVisible, visible);
}

// ---------------------------------------------------------------- 편집 기본값

double Settings::border_width() const {
  return snap(read_double(kKeyBorderWidth, kDefaultBorderWidth), {1.0, 2.0, 4.0, 8.0});
}

void Settings::set_border_width(double width) {
  if (!std::isfinite(width)) return;
  write(kKeyBorderWidth, snap(width, {1.0, 2.0, 4.0, 8.0}));
}

int Settings::opacity_percent() const {
  return snap(read_int(kKeyOpacity, kDefaultOpacityPercent), {25, 50, 75, 100});
}

void Settings::set_opacity_percent(int percent) {
  write(kKeyOpacity, snap(percent, {25, 50, 75, 100}));
}

std::uint8_t Settings::opacity_alpha() const {
  return static_cast<std::uint8_t>((opacity_percent() * 255 + 50) / 100);
}

engine::Color Settings::annotation_color() const {
  return color_from_string(read_string(kKeyAnnotationColor, QString()), kDefaultAnnotationColor);
}

void Settings::set_annotation_color(engine::Color color) {
  write(kKeyAnnotationColor, color_to_string(color));
}

int Settings::text_size() const {
  return snap(read_int(kKeyTextSize, kDefaultTextSize), {8, 10, 12, 14, 18, 24, 36, 48, 72});
}

void Settings::set_text_size(int size) {
  write(kKeyTextSize, snap(size, {8, 10, 12, 14, 18, 24, 36, 48, 72}));
}

engine::Color Settings::text_color() const {
  return color_from_string(read_string(kKeyTextColor, QString()), kDefaultTextColor);
}

void Settings::set_text_color(engine::Color color) {
  write(kKeyTextColor, color_to_string(color));
}

QString Settings::author() const {
  return read_string(kKeyAuthor, QString()).trimmed().left(256);
}

void Settings::set_author(const QString& author) {
  write(kKeyAuthor, author.trimmed().left(256));
}

// ---------------------------------------------------------------- 고급

int Settings::cache_mb() const {
  return std::clamp(read_int(kKeyCacheMb, kDefaultCacheMb), kMinCacheMb, kMaxCacheMb);
}

void Settings::set_cache_mb(int mb) {
  write(kKeyCacheMb, std::clamp(mb, kMinCacheMb, kMaxCacheMb));
}

bool Settings::hazard_notice() const {
  return read_bool(kKeyHazardNotice, true);
}

void Settings::set_hazard_notice(bool on) {
  write(kKeyHazardNotice, on);
}

int Settings::stall_seconds() const {
  return std::clamp(read_int(kKeyStallSeconds, kDefaultStallSeconds), kMinStallSeconds,
                    kMaxStallSeconds);
}

void Settings::set_stall_seconds(int seconds) {
  write(kKeyStallSeconds, std::clamp(seconds, kMinStallSeconds, kMaxStallSeconds));
}

bool Settings::render_process() const {
  return read_bool(kKeyRenderProcess, true);
}

void Settings::set_render_process(bool on) {
  write(kKeyRenderProcess, on);
}

int Settings::render_timeout_seconds() const {
  return std::clamp(read_int(kKeyRenderTimeout, kDefaultRenderTimeoutSeconds),
                    kMinRenderTimeoutSeconds, kMaxRenderTimeoutSeconds);
}

void Settings::set_render_timeout_seconds(int seconds) {
  write(kKeyRenderTimeout, std::clamp(seconds, kMinRenderTimeoutSeconds, kMaxRenderTimeoutSeconds));
}

bool Settings::crash_notice() const {
  return read_bool(kKeyCrashNotice, true);
}

void Settings::set_crash_notice(bool on) {
  write(kKeyCrashNotice, on);
}

// ---------------------------------------------------------------- 세션 표식 (#131)

SessionMarker Settings::session_marker() const {
  SessionMarker m;
  m.path = read_string(kKeySessionPath, QString());
  m.stage = read_string(kKeySessionStage, QString());
  m.since = QDateTime::fromString(read_string(kKeySessionSince, QString()), Qt::ISODate);
  m.last_action = read_string(kKeySessionAction, QString());
  m.version = read_string(kKeySessionVersion, QString());
  return m;
}

void Settings::set_session_marker(const SessionMarker& marker) {
  store_->setValue(QLatin1String(kKeySessionPath), marker.path);
  store_->setValue(QLatin1String(kKeySessionStage), marker.stage);
  store_->setValue(
      QLatin1String(kKeySessionSince),
      (marker.since.isValid() ? marker.since : QDateTime::currentDateTime()).toString(Qt::ISODate));
  store_->setValue(QLatin1String(kKeySessionAction), marker.last_action);
  store_->setValue(QLatin1String(kKeySessionVersion), marker.version);
  store_->sync();  // 크래시 직전까지 남아야 한다
}

void Settings::set_session_stage(const QString& stage) {
  store_->setValue(QLatin1String(kKeySessionStage), stage);
  store_->sync();
}

void Settings::set_session_last_action(const QString& action) {
  if (store_->value(QLatin1String(kKeySessionAction)).toString() == action) return;
  store_->setValue(QLatin1String(kKeySessionAction), action);
}

void Settings::clear_session_marker() {
  store_->beginGroup(QStringLiteral("session"));
  store_->remove(QString());
  store_->endGroup();
  store_->sync();
}

// ---------------------------------------------------------------- 창 상태

QByteArray Settings::window_geometry() const {
  return store_->value(QLatin1String(kKeyGeometry)).toByteArray();
}

void Settings::set_window_geometry(const QByteArray& geometry) {
  write(kKeyGeometry, geometry);
}

QByteArray Settings::splitter_state() const {
  return store_->value(QLatin1String(kKeySplitter)).toByteArray();
}

void Settings::set_splitter_state(const QByteArray& state) {
  write(kKeySplitter, state);
}

// ---------------------------------------------------------------- 파일

QString Settings::last_dir(DirKind kind) const {
  const QString dir = read_string(dir_key(kind), QString());
  return !dir.isEmpty() && QFileInfo(dir).isDir() ? dir : QString();
}

void Settings::set_last_dir(DirKind kind, const QString& dir) {
  if (dir.isEmpty()) return;
  write(dir_key(kind), QDir(dir).absolutePath());
}

QStringList Settings::recent_files() {
  const QStringList stored = store_->value(QLatin1String(kKeyRecent)).toStringList();
  QStringList kept;
  for (const QString& path : stored) {
    if (!path.isEmpty() && QFileInfo::exists(path) && !kept.contains(path)) kept << path;
    if (kept.size() >= kMaxRecent) break;
  }
  if (kept != stored) write(kKeyRecent, kept);
  return kept;
}

void Settings::add_recent(const QString& path) {
  if (path.isEmpty()) return;
  const QString absolute = QFileInfo(path).absoluteFilePath();
  QStringList list = store_->value(QLatin1String(kKeyRecent)).toStringList();
  list.removeAll(absolute);
  list.prepend(absolute);
  while (list.size() > kMaxRecent) list.removeLast();
  write(kKeyRecent, list);
}

void Settings::clear_recent() {
  write(kKeyRecent, QStringList{});
}

// ---------------------------------------------------------------- 초기화·동기화

void Settings::reset_preferences() {
  bool any = false;
  for (const char* group : {"general", "edit", "advanced"}) {
    store_->beginGroup(QLatin1String(group));
    if (!store_->allKeys().isEmpty()) {
      store_->remove(QString());
      any = true;
    }
    store_->endGroup();
  }
  if (any) emit changed();
}

void Settings::reset_all() {
  store_->clear();
  store_->setValue(QLatin1String(kKeySchema), kSchemaVersion);
  store_->sync();
  emit changed();
}

void Settings::sync() {
  store_->sync();
}

bool Settings::writable() const {
  return store_->isWritable() && store_->status() == QSettings::NoError;
}

// ---------------------------------------------------------------- 색

QString Settings::color_to_string(engine::Color color) {
  return QColor(color.r, color.g, color.b).name(QColor::HexRgb).toUpper();
}

engine::Color Settings::color_from_string(const QString& text, engine::Color fallback) {
  const QString t = text.trimmed();
  if (t.size() != 7 || !t.startsWith(QLatin1Char('#'))) return fallback;
  const QColor c = QColor::fromString(t);
  if (!c.isValid()) return fallback;
  return engine::Color{static_cast<std::uint8_t>(c.red()), static_cast<std::uint8_t>(c.green()),
                       static_cast<std::uint8_t>(c.blue()), 255};
}

}  // namespace onpdf::app
