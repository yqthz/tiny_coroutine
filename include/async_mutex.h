#pragma once

#include <coroutine>
#include <mutex>
#include <queue>

namespace tiny_coroutine {
class AsyncMutex {
public:
  struct LockGuard {
    AsyncMutex *async_mutex_{nullptr};
    bool owns_lock_{false};

    struct ReLockAwaiter {
      LockGuard *guard_{nullptr};

      explicit ReLockAwaiter(LockGuard *guard) : guard_(guard) {}

      bool await_ready() noexcept { return false; }

      template <typename T>
      bool await_suspend(std::coroutine_handle<T> handle) noexcept {
        std::lock_guard<std::mutex> lock(guard_->async_mutex_->mtx_);

        if (guard_->async_mutex_->lock_ == false) {
          guard_->async_mutex_->lock_ = true;
          return false;
        }
        guard_->async_mutex_->wait_queue_.push(handle);
        return true;
      }

      void await_resume() noexcept { guard_->owns_lock_ = true; }
    };

    LockGuard() = delete;
    explicit LockGuard(AsyncMutex *async_mutex)
        : async_mutex_(async_mutex), owns_lock_(true) {}

    LockGuard(const LockGuard &) = delete;
    LockGuard &operator=(const LockGuard &) = delete;

    LockGuard(LockGuard &&other) noexcept
        : async_mutex_(other.async_mutex_), owns_lock_(other.owns_lock_) {
      other.async_mutex_ = nullptr;
      other.owns_lock_ = false;
    }

    LockGuard &operator=(LockGuard &&other) noexcept {
      if (this == &other) {
        return *this;
      }
      if (owns_lock_ && async_mutex_ != nullptr) {
        async_mutex_->unlock();
      }
      async_mutex_ = other.async_mutex_;
      owns_lock_ = other.owns_lock_;
      other.async_mutex_ = nullptr;
      other.owns_lock_ = false;
      return *this;
    }

    ~LockGuard() {
      if (owns_lock_ && async_mutex_ != nullptr) {
        async_mutex_->unlock();
      }
    }

    void unlock() noexcept {
      if (owns_lock_ && async_mutex_ != nullptr) {
        async_mutex_->unlock();
        owns_lock_ = false;
      }
    }

    ReLockAwaiter relock() noexcept { return ReLockAwaiter{this}; }
  };

  struct Awaiter {
    AsyncMutex *async_mutex_{nullptr};

    explicit Awaiter(AsyncMutex *async_mutex) : async_mutex_(async_mutex) {}

    bool await_ready() noexcept { return false; }

    template <typename T>
    bool await_suspend(std::coroutine_handle<T> handle) noexcept {
      std::lock_guard<std::mutex> lock(async_mutex_->mtx_);

      if (async_mutex_->lock_ == false) {
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
    std::unique_lock<std::mutex> lock(mtx_);
    if (wait_queue_.empty() == false) {
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
  std::mutex mtx_;
  std::queue<std::coroutine_handle<>> wait_queue_;
};
} // namespace tiny_coroutine
