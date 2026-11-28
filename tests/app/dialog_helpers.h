#pragma once

// 앱 테스트 공용: 모달 대화상자를 objectName 으로 찾아 자동으로 답한다 (#112).
// QApplication::activeModalWidget() 은 "활성" 모달만 돌려줘, 앞 모달이 닫힌 직후 새 모달의 창
// 활성화가 늦으면 (Windows offscreen 에서 간헐적으로) null 이 되어 폴링이 영영 맞지 않았다 — 보이는
// 최상위 창을 직접 훑는다. 대기가 시간 초과하면 describe_open_windows() 로 무엇이 떠 있었는지
// 남긴다.

#include <QAbstractButton>
#include <QApplication>
#include <QDialog>
#include <QInputDialog>
#include <QMessageBox>
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

// 보이는 최상위 대화상자 가운데 아무거나 (이름 무관). 없으면 nullptr
inline QWidget* find_any_dialog() {
  for (QWidget* w : QApplication::topLevelWidgets()) {
    if (w->isVisible() && (w->isModal() || qobject_cast<QDialog*>(w) != nullptr)) return w;
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

// 대화상자가 뜨면 fn 을 한 번 부른다 (10ms 폴링). fn 을 부르기 전에 타이머를 멈춘다 — deleteLater
// 만으로는 Windows 에서 삭제되기 전에 한 번 더 발화해 같은 이름의 "다음" 대화상자에 이전 단계의
// 조작을 적용했다 (#129 설정 테스트에서 발견)
template <class Fn>
void when_dialog(const char* object_name, Fn fn) {
  auto* poll = new QTimer();
  poll->setInterval(10);
  QObject::connect(poll, &QTimer::timeout, poll, [poll, object_name, fn] {
    QWidget* w = find_dialog(object_name);
    if (w == nullptr) return;
    poll->stop();
    poll->deleteLater();
    fn(w);
  });
  poll->start();
}

// 이름이 object_name 인 메시지 상자가 뜨면 role 버튼을 누른다. answered 에 눌렀는지, text 에 본문
// (#145: activeModalWidget() 폴링 복사본들을 이것으로 — 활성화가 늦으면 exec() 가 영영 안 끝난다)
inline void answer_message_box(const char* object_name, QMessageBox::ButtonRole role,
                               bool* answered = nullptr, QString* text = nullptr) {
  when_dialog(object_name, [role, answered, text](QWidget* w) {
    auto* box = qobject_cast<QMessageBox*>(w);
    if (box == nullptr) return;
    if (text != nullptr) *text = box->text();
    for (QAbstractButton* button : box->buttons()) {
      if (box->buttonRole(button) != role) continue;
      if (answered != nullptr) *answered = true;
      button->click();
      break;
    }
  });
}

// 이름이 object_name 인 입력 대화상자가 뜨면 text 를 넣고 확인한다
inline void answer_input_dialog(const char* object_name, const QString& text,
                                bool* answered = nullptr) {
  when_dialog(object_name, [text, answered](QWidget* w) {
    auto* dialog = qobject_cast<QInputDialog*>(w);
    if (dialog == nullptr) return;
    if (answered != nullptr) *answered = true;
    dialog->setTextValue(text);
    dialog->accept();
  });
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
