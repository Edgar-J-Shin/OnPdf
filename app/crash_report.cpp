#include "crash_report.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QSysInfo>
#include <QUrlQuery>
#include <QtGlobal>

#ifndef ONPDF_PDFIUM_VERSION
#define ONPDF_PDFIUM_VERSION "unknown"
#endif
#ifndef ONPDF_VERSION
#define ONPDF_VERSION "unknown"
#endif

namespace onpdf::app {

namespace {

QString& override_dir() {
  static QString dir;
  return dir;
}

}  // namespace

bool SessionMarker::valid() const {
  return !stage.isEmpty() && since.isValid() &&
         (stage == QLatin1String("opening") || stage == QLatin1String("viewing") ||
          stage == QLatin1String("forced-quit"));
}

QDir report_dir() {
  if (!override_dir().isEmpty()) return QDir(override_dir());
  const QString base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
  return QDir(base + QStringLiteral("/crash-reports"));
}

void set_report_dir_override(const QString& dir) {
  override_dir() = dir;
}

std::optional<CrashRecord> parse_record(const QString& text, const QDateTime& fallback) {
  CrashRecord record;
  record.time = fallback;
  bool any = false;
  for (const QString& line : text.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
    const qsizetype eq = line.indexOf(QLatin1Char('='));
    if (eq <= 0) continue;
    const QString key = line.left(eq).trimmed();
    const QString value = line.mid(eq + 1).trimmed();
    if (key == QLatin1String("version")) {
      record.version = value;
    } else if (key == QLatin1String("pdfium")) {
      record.pdfium = value;
    } else if (key == QLatin1String("signal") || key == QLatin1String("exception")) {
      record.signal_text = value;
      any = true;
    } else if (key == QLatin1String("last_action")) {
      record.last_action = value;
    } else if (key == QLatin1String("time")) {
      bool ok = false;
      const qint64 epoch = value.toLongLong(&ok);
      if (ok) record.time = QDateTime::fromSecsSinceEpoch(epoch);
    }
  }
  if (!any) return std::nullopt;
  return record;
}

std::optional<CrashRecord> latest_record_since(const QDir& dir, const QDateTime& since) {
  std::optional<CrashRecord> best;
  for (const QFileInfo& info :
       dir.entryInfoList({QStringLiteral("crash-*.txt")}, QDir::Files, QDir::Name)) {
    QFile file(info.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly)) continue;
    const QString text = QString::fromUtf8(file.readAll().left(4096));
    auto record = parse_record(text, info.lastModified());
    if (!record) continue;
    if (since.isValid() && record->time < since.addSecs(-1)) continue;  // 1초 여유
    if (!best || record->time > best->time) best = std::move(record);
  }
  return best;
}

QString pdfium_version() {
  return QStringLiteral(ONPDF_PDFIUM_VERSION);
}

QString app_version() {
  return QStringLiteral(ONPDF_VERSION);
}

QUrl issue_url(const SessionMarker& marker, const std::optional<CrashRecord>& record) {
  const QString signal = record ? record->signal_text : QStringLiteral("unknown");
  const QString stage = marker.stage.isEmpty() ? QStringLiteral("unknown") : marker.stage;
  const QString title = QStringLiteral("Crash while %1 (OnPdf %2, %3)")
                            .arg(stage, app_version(), QSysInfo::prettyProductName());
  QString body;
  body += QStringLiteral(
      "<!-- Filled in by OnPdf. Do not attach the PDF unless you can share it. "
      "Please attach the files from the crash report folder (Help > Open Report "
      "Folder). -->\n\n");
  body += QStringLiteral("| | |\n|---|---|\n");
  body += QStringLiteral("| OnPdf | %1 |\n").arg(app_version());
  body += QStringLiteral("| OS | %1 (%2) |\n")
              .arg(QSysInfo::prettyProductName(), QSysInfo::currentCpuArchitecture());
  body += QStringLiteral("| Qt | %1 |\n").arg(QString::fromLatin1(qVersion()));
  body += QStringLiteral("| PDFium | %1 |\n").arg(pdfium_version());
  body += QStringLiteral("| Stage | %1 |\n").arg(stage);
  body += QStringLiteral("| Signal / exception | %1 |\n").arg(signal);
  body +=
      QStringLiteral("| Last action | %1 |\n")
          .arg(record && !record->last_action.isEmpty() ? record->last_action : marker.last_action);
  body += QStringLiteral("\n**What were you doing?**\n\n\n**Can you share the PDF?** (yes / no)\n");
  QUrl url(QStringLiteral("https://github.com/Edgar-J-Shin/OnPdf/issues/new"));
  QUrlQuery query;
  query.addQueryItem(QStringLiteral("title"), title);
  query.addQueryItem(QStringLiteral("body"), body);
  url.setQuery(query);
  return url;
}

QString stage_text(const QString& stage) {
  if (stage == QLatin1String("opening"))
    return QCoreApplication::translate("onpdf::app::MainWindow", "opening");
  if (stage == QLatin1String("viewing"))
    return QCoreApplication::translate("onpdf::app::MainWindow", "viewing");
  if (stage == QLatin1String("forced-quit"))
    return QCoreApplication::translate("onpdf::app::MainWindow",
                                       "being force-quit while rendering");
  return stage;
}

}  // namespace onpdf::app
