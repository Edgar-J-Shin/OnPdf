#pragma once

// 워커 스레드의 콜백을 메인(UI) 스레드로 넘기는 유일한 착지점.
// 어느 스레드에서든 post() 를 부를 수 있고, fn 은 이 객체가 속한 스레드(메인)에서 실행된다.
// PdfiumWorker 보다 오래 살아야 한다 (main() 에서 워커보다 먼저 선언).

#include <QObject>
#include <QPointer>

#include <functional>
#include <utility>

namespace onpdf::app {

class UiDispatcher : public QObject {
  Q_OBJECT
 public:
  explicit UiDispatcher(QObject* parent = nullptr);

  // fn 을 메인 스레드 이벤트 루프에 넣는다. 디스패처가 소멸하면 미처리 fn 은 버려진다.
  void post(std::function<void()> fn);

  // receiver 가 살아 있을 때만 fn(receiver) 를 메인 스레드에서 실행한다.
  template <class R, class F>
  void post_to(R* receiver, F fn) {
    QPointer<R> guard(receiver);
    post([guard, fn = std::move(fn)]() mutable {
      if (guard) fn(guard.data());
    });
  }
};

}  // namespace onpdf::app
