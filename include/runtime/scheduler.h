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

class Scheduler;
extern thread_local Scheduler *local_scheduler_ptr;

class Scheduler {
public:
  struct Awaiter {
    Scheduler *scheduler_{nullptr};

    explicit Awaiter(Scheduler *scheduler) : scheduler_(scheduler) {}

    bool await_ready() noexcept { return false; }

    template <typename Promise>
    void await_suspend(std::coroutine_handle<Promise> handle) noexcept {
      scheduler_->reschedule(handle);
    }

    void await_resume() noexcept {}
  };

  Scheduler() = default;
  ~Scheduler();

  Scheduler(const Scheduler &) = delete;
  Scheduler(Scheduler &&) = delete;
  Scheduler &operator=(const Scheduler &) = delete;
  Scheduler &operator=(Scheduler &&) = delete;

  void init(size_t context_count = std::thread::hardware_concurrency());

  void submit(Task<void> &&task);

  void submit(std::coroutine_handle<> handle);

  Awaiter schedule() { return Awaiter(this); }

  void loop();

  void stop();

private:
  void submit_new(std::coroutine_handle<> handle);

  void reschedule(std::coroutine_handle<> handle);

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

inline Scheduler *try_local_scheduler() noexcept { return local_scheduler_ptr; }

inline void submit_to_scheduler(Scheduler &scheduler, Task<void> &&task) {
  scheduler.submit(std::move(task));
}

inline void submit_to_scheduler(Scheduler &scheduler,
                                std::coroutine_handle<> handle) {
  scheduler.submit(handle);
}

inline void submit_to_scheduler(Task<void> &&task) {
  auto handle = task.get_handle();
  task.detach();

  auto *scheduler = try_local_scheduler();
  if (scheduler == nullptr) {
    handle.destroy();
    return;
  }

  scheduler->submit(handle);
}

inline void submit_to_scheduler(std::coroutine_handle<> handle) {
  if (!handle) {
    return;
  }

  auto *scheduler = try_local_scheduler();
  if (scheduler == nullptr) {
    handle.destroy();
    return;
  }

  scheduler->submit(handle);
}

inline void submit_to_context(Task<void> &&task) {
  auto handle = task.get_handle();
  task.detach();

  auto *ctx = try_local_context();
  if (ctx == nullptr) {
    handle.destroy();
    return;
  }

  ctx->submit_tracked_task(handle);
}

inline void submit_to_context(std::coroutine_handle<> handle) {
  if (!handle) {
    return;
  }

  auto *ctx = try_local_context();
  if (ctx == nullptr) {
    handle.destroy();
    return;
  }

  ctx->submit_tracked_task(handle);
}

} // namespace tiny_coroutine::runtime
