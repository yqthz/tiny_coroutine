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

class Context;
inline thread_local Context *local_context_ptr = nullptr;

class Context {
public:
  using OnTaskCompleted = std::function<void()>;

  Context(size_t id, std::atomic<size_t> *pending_tasks,
          OnTaskCompleted on_task_completed)
      : id_(id), pending_tasks_(pending_tasks),
        on_task_completed_(std::move(on_task_completed)) {}

  Context(const Context &) = delete;
  Context(Context &&) = delete;
  Context &operator=(const Context &) = delete;
  Context &operator=(Context &&) = delete;

  void start() {
    worker_ = std::jthread([this](std::stop_token token) { run(token); });
  }

  void notify_stop() {
    worker_.request_stop();
    engine_.notify_all();
  }

  void join() {
    if (worker_.joinable()) {
      worker_.join();
    }
  }

  void submit_task(std::coroutine_handle<> handle) { engine_.submit_task(handle); }

  void submit_tracked_task(std::coroutine_handle<> handle) {
    if (!handle) {
      return;
    }
    pending_tasks_->fetch_add(1, std::memory_order_release);
    engine_.submit_task(handle);
  }

  size_t id() const noexcept { return id_; }

private:
  void run(std::stop_token token) {
    local_context_ptr = this;
    while (true) {
      auto handle =
          engine_.pop_task_or_wait([&token]() { return token.stop_requested(); });
      if (!handle) {
        if (token.stop_requested() && engine_.empty()) {
          break;
        }
        continue;
      }

      handle.resume();
      if (handle.done()) {
        handle.destroy();
        on_task_completed_();
      }
    }
    local_context_ptr = nullptr;
  }

  size_t id_{0};
  std::atomic<size_t> *pending_tasks_{nullptr};
  Engine engine_;
  std::jthread worker_;
  OnTaskCompleted on_task_completed_;
};

inline Context *try_local_context() noexcept { return local_context_ptr; }

inline Context &local_context() noexcept { return *local_context_ptr; }

} // namespace tiny_coroutine::runtime
