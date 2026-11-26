#pragma once

// 앱 테스트 공용: 모달 대화상자를 objectName 으로 찾아 자동으로 답한다 (#112).
// QApplication::activeModalWidget() 은 "활성" 모달만 돌려줘, 앞 모달이 닫힌 직후 새 모달의 창
// 활성화가 늦으면 (Windows offscreen 에서 간헐적으로) null 이 되어 폴링이 영영 맞지 않았다 — 보이는
// 최상위 창을 직접 훑는다. 대기가 시간 초과하면 describe_open_windows() 로 무엇이 떠 있었는지
// 남긴다.

#include <QApplication>
#include <QDialog>
#include <QString>
#include <QTimer>
#include <QWidget>

namespace onpdf::test {

// 보이는 최상위 대화상자(QDialog 또는 모달 위젯) 가운데 objectName 이 맞는 것. 없으면 nullptr
inline QWidget* find_dialog(const char* object_name) {
  for (QWidget* w : QApplication::topLevelWidgets()) {
    if (!w->isVisible() || w->objectName() != QLatin1String(object_name)) continue;
    if (w->isModal() || qobject_cast<QDialog*>(w) != nullptr) return w;
  }
  return nullptr;
}

// 보이는 모달 대화상자 수 (어느 이름이든)
inline int open_dialog_count() {
  int n = 0;
  for (QWidget* w : QApplication::topLevelWidgets()) {
    if (w->isVisible() && (w->isModal() || qobject_cast<QDialog*>(w) != nullptr)) ++n;
  }
  return n;
}

// 대화상자가 뜨면 fn 을 한 번 부른다 (10ms 폴링, 처리 뒤 타이머 해제)
template <class Fn>
void when_dialog(const char* object_name, Fn fn) {
  auto* poll = new QTimer();
  poll->setInterval(10);
  QObject::connect(poll, &QTimer::timeout, poll, [poll, object_name, fn] {
    QWidget* w = find_dialog(object_name);
    if (w == nullptr) return;
    fn(w);
    poll->deleteLater();
  });
  poll->start();
}

// 시간 초과 진단용: 보이는 최상위 창 목록
inline QString describe_open_windows() {
  QString out;
  for (QWidget* w : QApplication::topLevelWidgets()) {
    if (!w->isVisible()) continue;
    out += QStringLiteral("%1(%2)%3 ")
               .arg(QString::fromLatin1(w->metaObject()->className()), w->objectName(),
                    w->isModal() ? QStringLiteral("[modal]") : QString());
  }
  QWidget* active = QApplication::activeModalWidget();
  out += QStringLiteral("| activeModalWidget=%1")
             .arg(active != nullptr ? active->objectName() : QStringLiteral("null"));
  return out;
}

}  // namespace onpdf::test
