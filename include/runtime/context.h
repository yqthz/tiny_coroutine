#pragma once

#include "engine.h"

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <functional>
#include <stop_token>
#include <thread>
#include <utility>

namespace tiny_coroutine::runtime {

class Scheduler;
class Context;

extern thread_local Scheduler *local_scheduler_ptr;
extern thread_local Context *local_context_ptr;

class Context {
public:
  using OnTaskCompleted = std::function<void()>;

  Context(size_t id, Scheduler *owner_scheduler,
          std::atomic<size_t> *pending_tasks,
          OnTaskCompleted on_task_completed);

  Context(const Context &) = delete;
  Context(Context &&) = delete;
  Context &operator=(const Context &) = delete;
  Context &operator=(Context &&) = delete;

  void start();

  void notify_stop();

  void join();

  void submit_task(std::coroutine_handle<> handle);

  void submit_tracked_task(std::coroutine_handle<> handle);

  size_t id() const noexcept;

private:
  void run(std::stop_token token);
  bool process_work_once();
  bool poll_io_once();
  void wait_or_idle(std::stop_token token);

  size_t id_{0};
  Scheduler *owner_scheduler_{nullptr};
  std::atomic<size_t> *pending_tasks_{nullptr};
  Engine engine_;
  std::jthread worker_;
  OnTaskCompleted on_task_completed_;
};

inline Context *try_local_context() noexcept { return local_context_ptr; }

inline Context &local_context() noexcept { return *local_context_ptr; }

} // namespace tiny_coroutine::runtime
