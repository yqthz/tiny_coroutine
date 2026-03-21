#pragma once

#include <atomic>
#include <coroutine>
#include <mutex>
#include <pthread.h>
#include <queue>

namespace tiny_coroutine {
class AsyncMutex {
public:
  struct LockGuard {
    AsyncMutex *async_mutex_{nullptr};

    LockGuard() = delete;
    explicit LockGuard(AsyncMutex *async_mutex) : async_mutex_(async_mutex) {}

    LockGuard(const LockGuard &) = delete;
    LockGuard &operator=(const LockGuard &) = delete;

    LockGuard(LockGuard &&lockGuard) noexcept {
      async_mutex_ = lockGuard.async_mutex_;
      lockGuard.async_mutex_ = nullptr;
    }

    LockGuard &operator=(LockGuard &&lockguard) noexcept {
      if (this == &lockguard) {
        return *this;
      }
      if (async_mutex_) {
        async_mutex_->unlock();
      }
      async_mutex_ = lockguard.async_mutex_;
      lockguard.async_mutex_ = nullptr;
      return *this;
    }

    ~LockGuard() {
      if (async_mutex_) {
        async_mutex_->unlock();
      }
    }
  };

  struct Awaiter {
    AsyncMutex *async_mutex_{nullptr};

    explicit Awaiter(AsyncMutex *async_mutex) : async_mutex_(async_mutex) {}

    bool await_ready() noexcept { return false; }

    template <typename T>
    bool await_suspend(std::coroutine_handle<T> handle) noexcept {
      std::lock_guard<std::mutex> lock(async_mutex_->mtx);

      if (!async_mutex_->lock_) {
        async_mutex_->lock_ = true;
        return false;
      }
      async_mutex_->wait_queue_.push(handle);
      return true;
    }

    LockGuard await_resume() noexcept { return LockGuard{async_mutex_}; }
  };

  Awaiter lock() noexcept { return Awaiter{this}; }

  void unlock() noexcept {
    std::unique_lock<std::mutex> lock(mtx);
    if (!wait_queue_.empty()) {
      auto handle = wait_queue_.front();
      wait_queue_.pop();
      lock.unlock();
      handle.resume();
    } else {
      lock_ = false;
    }
  }

private:
  bool lock_{false};
  std::mutex mtx;
  std::queue<std::coroutine_handle<>> wait_queue_;
};
} // namespace tiny_coroutine