#pragma once

#include "async_mutex.h"
#include "task.h"
#include "wait_group.h"

#include <coroutine>
#include <mutex>
#include <queue>

namespace tiny_coroutine {
class ConditionVariable {
public:
  struct Awaiter {
    ConditionVariable *cv_{nullptr};

    explicit Awaiter(ConditionVariable *cv) : cv_(cv) {}

    bool await_ready() noexcept { return false; }

    template <typename T>
    void await_suspend(std::coroutine_handle<T> handle) noexcept {
      std::lock_guard<std::mutex> lock(cv_->mtx_);
      cv_->wait_queue_.push(handle);
    }

    void await_resume() noexcept {}
  };

  template <typename Callable>
  Task<void> wait(AsyncMutex &mtx, Callable op) noexcept {
    while (!op()) {
      mtx.unlock();
      co_await Awaiter{this};
      co_await mtx.lock();
    }
  }

  Awaiter wait(AsyncMutex &mtx) noexcept {
    mtx.unlock();
    return Awaiter{this};
  }

  void notify_one() noexcept {
    std::unique_lock<std::mutex> lock(mtx_);
    if (!wait_queue_.empty()) {
      auto handle = wait_queue_.front();
      wait_queue_.pop();
      lock.unlock();
      handle.resume();
    }
  }

  void notify_all() noexcept {
    std::unique_lock<std::mutex> lock(mtx_);
    std::vector<std::coroutine_handle<>> handles;
    while (!wait_queue_.empty()) {
      handles.push_back(wait_queue_.front());
      wait_queue_.pop();
    }

    lock.unlock();
    for (auto handle : handles) {
      handle.resume();
    }
  }

private:
  std::mutex mtx_;
  std::queue<std::coroutine_handle<>> wait_queue_;
};
} // namespace tiny_coroutine