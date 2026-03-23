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
  ~Scheduler() { stop(); }

  Scheduler(const Scheduler &) = delete;
  Scheduler(Scheduler &&) = delete;
  Scheduler &operator=(const Scheduler &) = delete;
  Scheduler &operator=(Scheduler &&) = delete;

  void init(size_t context_count = std::thread::hardware_concurrency()) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (initialized_) {
      return;
    }

    const size_t worker_count = context_count == 0 ? 1 : context_count;
    contexts_.reserve(worker_count);
    for (size_t i = 0; i < worker_count; ++i) {
      contexts_.push_back(std::make_unique<Context>(
          i, &pending_tasks_, [this]() noexcept { on_task_completed(); }));
    }
    for (auto &context : contexts_) {
      context->start();
    }

    dispatcher_.init(worker_count);
    pending_tasks_.store(0, std::memory_order_relaxed);
    stop_requested_.store(false, std::memory_order_relaxed);
    accepting_ = true;
    initialized_ = true;
  }

  void submit(Task<void> &&task) {
    auto handle = task.get_handle();
    task.detach();
    submit_new(handle);
  }

  void submit(std::coroutine_handle<> handle) { submit_new(handle); }

  Awaiter schedule() { return Awaiter(this); }

  void loop() {
    std::unique_lock<std::mutex> lock(state_mutex_);
    if (!initialized_) {
      throw std::logic_error("runtime::Scheduler::init must be called before loop");
    }

    completion_cv_.wait(lock, [this]() {
      return stop_requested_.load(std::memory_order_acquire) ||
             pending_tasks_.load(std::memory_order_acquire) == 0;
    });

    accepting_ = false;
    lock.unlock();

    shutdown_contexts();
  }

  void stop() {
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (!initialized_) {
        return;
      }
      accepting_ = false;
      stop_requested_.store(true, std::memory_order_release);
    }

    completion_cv_.notify_all();
    shutdown_contexts();
  }

private:
  void submit_new(std::coroutine_handle<> handle) {
    if (!handle) {
      return;
    }

    Context *target = nullptr;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (!initialized_ || !accepting_) {
        handle.destroy();
        return;
      }
      pending_tasks_.fetch_add(1, std::memory_order_release);
      target = contexts_[dispatcher_.dispatch()].get();
    }

    target->submit_task(handle);
  }

  void reschedule(std::coroutine_handle<> handle) {
    if (!handle) {
      return;
    }

    if (auto *ctx = try_local_context(); ctx != nullptr) {
      ctx->submit_task(handle);
      return;
    }

    Context *target = nullptr;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (!initialized_ || !accepting_) {
        handle.destroy();
        return;
      }
      target = contexts_[dispatcher_.dispatch()].get();
    }

    target->submit_task(handle);
  }

  void on_task_completed() {
    if (pending_tasks_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      completion_cv_.notify_all();
    }
  }

  void shutdown_contexts() {
    std::vector<std::unique_ptr<Context>> contexts;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (!initialized_) {
        return;
      }
      contexts.swap(contexts_);
      initialized_ = false;
    }

    for (auto &context : contexts) {
      context->notify_stop();
    }
    for (auto &context : contexts) {
      context->join();
    }
  }

  std::mutex state_mutex_;
  std::condition_variable completion_cv_;
  std::vector<std::unique_ptr<Context>> contexts_;
  Dispatcher dispatcher_;

  std::atomic<size_t> pending_tasks_{0};
  std::atomic<bool> stop_requested_{false};

  bool accepting_{false};
  bool initialized_{false};
};

inline void submit_to_scheduler(Scheduler &scheduler, Task<void> &&task) {
  scheduler.submit(std::move(task));
}

inline void submit_to_scheduler(Scheduler &scheduler,
                                std::coroutine_handle<> handle) {
  scheduler.submit(handle);
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
