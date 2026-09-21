/** @fileoverview Auto-scroll setup stays responsive and cannot revive a cancelled panel. */
#include "scroll-startup-smoke.hpp"
#include "scroll-capture.hpp"

#include <QCoreApplication>
#include <QFutureWatcher>
#include <QElapsedTimer>
#include <QEvent>
#include <algorithm>
#include <atomic>
#include <memory>
#include <utility>
#include <QScopeGuard>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>

namespace {
template <typename Predicate> bool waitUntil(Predicate ready) {
  QElapsedTimer timer;
  timer.start();
  while (!ready() && timer.elapsed() < 2000) {
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QThread::msleep(1);
  }
  return ready();
}

struct SetupState {
  std::atomic<bool> entered{false};
  std::atomic<bool> unblock{false};
  std::atomic<bool> exited{false};
  std::atomic<bool> offGuiThread{false};
  std::shared_ptr<std::atomic<bool>> stop;
};
} // namespace

bool runScrollStartupSmoke(QString &error) {
  // A cancelled setup must return before probing the real compositor.
  auto cancelled = std::make_shared<std::atomic<bool>>(true);
  if (spawnScrollInjector(cancelled, std::make_shared<stitch::CaptureHandshake>(),
                          0, 0, stitch::Axis::Vertical, QString(), error)) {
    error = QStringLiteral("Cancelled injector unexpectedly started");
    return false;
  }
  // Hold one setup job while a replacement completes, including on
  // single-core test runners whose default pool has only one thread.
  auto *pool = QThreadPool::globalInstance();
  const int previousThreads = pool->maxThreadCount();
  pool->setMaxThreadCount(std::max(2, previousThreads));
  const auto restorePool = qScopeGuard([pool, previousThreads] {
    pool->setMaxThreadCount(previousThreads);
  });
  MonitorInfo monitor;
  monitor.geometry = QRect(0, 0, 800, 600);
  monitor.scale = 1;
  const auto state = std::make_shared<SetupState>();
  const auto cleanup = qScopeGuard([state] { state->unblock = true; });
  auto panel = std::make_unique<ScrollCapturePanel>(monitor, nullptr, nullptr);
  panel->resize(800, 600);
  panel->region_ = QRect(100, 100, 400, 300);
  panel->phase_ = ScrollCapturePanel::Phase::Capturing;
  panel->mode_ = ScrollCapturePanel::Mode::Auto;
  // A stopped loop can have UI notices queued behind Back. They must not
  // make a replacement capture appear stalled or overwrite its status.
  auto notices = QtConcurrent::run([&panel] {
    panel->postStatus(QStringLiteral("old capture"));
    panel->postStalled();
  });
  notices.waitForFinished();
  panel->autoStalled_ = true;
  panel->returnToModeChoice();
  if (panel->autoStalled_) {
    error = QStringLiteral("Back retained the old capture's Continue state");
    return false;
  }
  panel->phase_ = ScrollCapturePanel::Phase::Capturing;
  panel->setStatus(QStringLiteral("replacement capture"));
  QCoreApplication::processEvents();
  if (panel->autoStalled_ || panel->status_ != QStringLiteral("replacement capture")) {
    error = QStringLiteral("Queued notices revived a stopped capture");
    return false;
  }
  notices = QtConcurrent::run([&panel] {
    panel->postStatus(QStringLiteral("current capture"));
    panel->postStalled();
  });
  notices.waitForFinished();
  QCoreApplication::processEvents();
  if (!panel->autoStalled_ || panel->status_ != QStringLiteral("current capture")) {
    error = QStringLiteral("Current capture notices were lost");
    return false;
  }
  panel->autoStalled_ = false;
  // Back and another manual selection can fit inside the chrome-settle
  // interval. The old start must not launch against the replacement Worker.
  // There is deliberately no Worker here: reviving that start is invalid.
  panel->mode_ = ScrollCapturePanel::Mode::Manual;
  panel->startManualCapture();
  panel->returnToModeChoice();
  panel->phase_ = ScrollCapturePanel::Phase::Capturing;
  bool settled = false;
  QTimer::singleShot(120, panel.get(), [&settled] { settled = true; });
  if (!waitUntil([&] { return settled; }) || !panel->workerFuture_.isCanceled()) {
    error = QStringLiteral("A cancelled manual start reached the replacement capture");
    return false;
  }
  panel->mode_ = ScrollCapturePanel::Mode::Auto;
  panel->injectorStarter_ = [state](auto stop, auto, int, int, auto,
                                  const QString &, QString &) {
    state->stop = std::move(stop);
    state->offGuiThread = !QThread::isMainThread();
    state->entered.store(true, std::memory_order_release);
    while (!state->unblock.load(std::memory_order_acquire))
      QThread::msleep(1);
    state->exited = true;
    return true;
  };
  panel->startInjector(false);
  if (!waitUntil([&] { return state->entered.load(std::memory_order_acquire); })) {
    error = QStringLiteral("Auto-scroll setup did not start");
    return false;
  }
  bool heartbeat = false;
  QTimer::singleShot(0, [&heartbeat] { heartbeat = true; });
  if (!waitUntil([&] { return heartbeat; }) || !state->offGuiThread) {
    error = QStringLiteral("Auto-scroll setup blocked GUI events");
    return false;
  }

  // Back cancels the token without waiting for setup, then a new attempt can
  // complete while the old one is still probing the compositor.
  panel->returnToModeChoice();
  if (!state->stop->load(std::memory_order_acquire)) {
    error = QStringLiteral("Back did not cancel pending injector setup");
    return false;
  }
  panel->phase_ = ScrollCapturePanel::Phase::Capturing;
  panel->injectorStarter_ = [](auto, auto, int, int, auto,
                               const QString &, QString &spawnError) {
    spawnError = QStringLiteral("deliberate setup failure");
    return false;
  };
  panel->startInjector(true);
  if (!waitUntil([&] { return panel->autoStalled_; }) ||
      !panel->status_.contains(QStringLiteral("deliberate setup failure"))) {
    error = QStringLiteral("Continue did not report asynchronous setup failure");
    return false;
  }
  const QString currentStatus = panel->status_;
  state->unblock = true;
  if (!waitUntil([&] { return state->exited.load(); })) {
    error = QStringLiteral("Cancelled setup did not exit");
    return false;
  }
  // Drain both watcher callbacks; the stale success must not start a capture
  // loop (there is deliberately no OutputCapture worker in this fixture).
  if (!waitUntil([&] {
        return panel->findChildren<QFutureWatcherBase *>().isEmpty();
      }) || panel->status_ != currentStatus) {
    error = QStringLiteral("Stale setup completion changed the new capture");
    return false;
  }

  // Destruction while a setup job is running is safe: only copied inputs and
  // shared cancellation state belong to that job.
  const auto dying = std::make_shared<SetupState>();
  const auto unblockDying = qScopeGuard([dying] { dying->unblock = true; });
  panel->injectorStarter_ = [dying](auto stop, auto, int, int, auto,
                                  const QString &, QString &) {
    dying->stop = std::move(stop);
    dying->entered.store(true, std::memory_order_release);
    while (!dying->unblock.load(std::memory_order_acquire))
      QThread::msleep(1);
    dying->exited = true;
    return true;
  };
  panel->startInjector(true);
  if (!waitUntil([&] { return dying->entered.load(std::memory_order_acquire); })) {
    error = QStringLiteral("Destruction fixture did not start setup");
    return false;
  }
  panel.reset();
  if (!dying->stop->load(std::memory_order_acquire)) {
    error = QStringLiteral("Panel destruction did not cancel setup");
    return false;
  }
  dying->unblock = true;
  if (!waitUntil([&] { return dying->exited.load(); })) {
    error = QStringLiteral("Detached setup did not finish after destruction");
    return false;
  }
  QCoreApplication::processEvents();
  return true;
}
