#pragma once

#include <condition_variable>
#include <coroutine>
#include <mutex>
#include <queue>

namespace tiny_coroutine::runtime {

class Engine {
public:
  void submit_task(std::coroutine_handle<> handle) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      task_queue_.push(handle);
    }
    cv_.notify_one();
  }

  template <typename StopPredicate>
  std::coroutine_handle<> pop_task_or_wait(StopPredicate stop_requested) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this, &stop_requested]() {
      return stop_requested() || !task_queue_.empty();
    });

    if (task_queue_.empty()) {
      return {};
    }

    auto handle = task_queue_.front();
    task_queue_.pop();
    return handle;
  }

  bool empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return task_queue_.empty();
  }

  void notify_all() { cv_.notify_all(); }

private:
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::queue<std::coroutine_handle<>> task_queue_;
};

} // namespace tiny_coroutine::runtime
