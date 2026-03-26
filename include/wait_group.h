#pragma once

#include "runtime/resume_handle.h"

#include <atomic>
#include <coroutine>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <vector>

namespace tiny_coroutine {
class WaitGroup {
public:
  struct Awaiter {
    WaitGroup *wait_group_;
    Awaiter() = delete;
    explicit Awaiter(WaitGroup *wait_group) : wait_group_(wait_group) {}

    bool await_ready() noexcept { return wait_group_->count_ == 0; }

    template <typename T>
    bool await_suspend(std::coroutine_handle<T> handle) noexcept {
      std::lock_guard<std::mutex> lock(wait_group_->mtx);
      if (wait_group_->count_ == 0) {
        return false;
      }
      wait_group_->wait_queue_.push(handle);
      return true;
    }

    void await_resume() noexcept {}
  };

  void add(int n) noexcept {
    std::lock_guard<std::mutex> lock(mtx);
    count_ += n;
  }

  void done() {
    std::unique_lock<std::mutex> lock(mtx);
    if (count_ == 0) {
      throw std::runtime_error("count is zero");
    }
    count_ -= 1;
    if (count_ == 0) {
      std::vector<std::coroutine_handle<>> handles;
      while (!wait_queue_.empty()) {
        handles.push_back(wait_queue_.front());
        wait_queue_.pop();
      }

      lock.unlock();
      for (auto handle : handles) {
        runtime::reschedule_or_resume(handle);
      }
    }
  }

  Awaiter wait() { return Awaiter{this}; }

private:
  std::atomic<int> count_{0};
  std::queue<std::coroutine_handle<>> wait_queue_;
  std::mutex mtx;
};
} // namespace tiny_coroutine
