#pragma once

#include "context.h"
#include "dispatcher.h"
#include "task.h"

#include <atomic>
#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace tiny_coroutine::runtime {

class Scheduler {
public:
  Scheduler() = default;
  ~Scheduler();

  Scheduler(const Scheduler &) = delete;
  Scheduler(Scheduler &&) = delete;
  Scheduler &operator=(const Scheduler &) = delete;
  Scheduler &operator=(Scheduler &&) = delete;

  void init(size_t context_count = std::thread::hardware_concurrency());

  void submit(Task<void> &&task);

  void submit(std::coroutine_handle<> handle);

  void loop();

  void stop();

private:
  void submit_new(std::coroutine_handle<> handle);

  void on_task_completed();

  void shutdown_contexts();

  std::mutex state_mutex_;
  std::condition_variable completion_cv_;
  std::vector<std::unique_ptr<Context>> contexts_;
  Dispatcher dispatcher_;

  std::atomic<size_t> pending_tasks_{0};
  std::atomic<bool> stop_requested_{false};

  bool accepting_{false};
  bool initialized_{false};
};

struct YieldAwaiter {
  bool missing_context_{false};

  bool await_ready() const noexcept { return false; }

  template <typename Promise>
  bool await_suspend(std::coroutine_handle<Promise> handle) noexcept {
    auto *ctx = try_local_context();
    if (ctx == nullptr) {
      missing_context_ = true;
      return false;
    }

    ctx->submit_task(handle);
    return true;
  }

  void await_resume() const {
    if (missing_context_) {
      throw std::logic_error("runtime::yield() requires runtime context");
    }
  }
};

inline YieldAwaiter yield() noexcept { return YieldAwaiter{}; }

inline void submit_to_scheduler(Task<void> &&task) {
  auto handle = task.get_handle();
  task.detach();

  auto *ctx = try_local_context();
  if (ctx != nullptr) {
    ctx->submit_tracked_task(handle);
    return;
  }

  handle.destroy();
  throw std::logic_error("runtime::submit_to_scheduler() requires runtime context");
}

inline void submit_to_scheduler(std::coroutine_handle<> handle) {
  if (!handle) {
    return;
  }

  auto *ctx = try_local_context();
  if (ctx != nullptr) {
    ctx->submit_tracked_task(handle);
    return;
  }

  handle.destroy();
  throw std::logic_error("runtime::submit_to_scheduler() requires runtime context");
}

} // namespace tiny_coroutine::runtime
