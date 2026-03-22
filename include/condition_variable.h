#pragma once

#include "async_mutex.h"
#include "task.h"

#include <coroutine>
#include <mutex>
#include <queue>
#include <vector>

namespace tiny_coroutine {
class ConditionVariable {
public:
  struct Awaiter {
    ConditionVariable *cv_{nullptr};
    AsyncMutex::LockGuard *guard_{nullptr};

    Awaiter() = delete;
    Awaiter(ConditionVariable *cv, AsyncMutex::LockGuard &guard)
        : cv_(cv), guard_(&guard) {}

    bool await_ready() noexcept { return false; }

    template <typename T>
    bool await_suspend(std::coroutine_handle<T> handle) noexcept {
      {
        std::lock_guard<std::mutex> lock(cv_->mtx_);
        cv_->wait_queue_.push(handle);
      }
      guard_->unlock();
      return true;
    }

    void await_resume() noexcept {}
  };

  template <typename Callable>
  [[nodiscard]] Task<void> wait(AsyncMutex::LockGuard &guard,
                                Callable op) noexcept {
    while (op() == false) {
      co_await Awaiter{this, guard};
      co_await guard.relock();
    }
  }

  [[nodiscard]] Task<void> wait(AsyncMutex::LockGuard &guard) noexcept {
    co_await Awaiter{this, guard};
    co_await guard.relock();
  }

  void notify_one() noexcept {
    std::unique_lock<std::mutex> lock(mtx_);
    if (wait_queue_.empty() == false) {
      auto handle = wait_queue_.front();
      wait_queue_.pop();
      lock.unlock();
      handle.resume();
    }
  }

  void notify_all() noexcept {
    std::unique_lock<std::mutex> lock(mtx_);
    std::vector<std::coroutine_handle<>> handles;
    while (wait_queue_.empty() == false) {
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
