#pragma once

#include "task.h"

#include <cassert>
#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>

namespace tiny_coroutine {
class Scheduler {
public:
  struct Awaiter {
    Scheduler *scheduler_{nullptr};

    explicit Awaiter(Scheduler *scheduler) : scheduler_(scheduler) {}

    bool await_ready() noexcept { return false; }

    template <typename T>
    void await_suspend(std::coroutine_handle<T> task) noexcept {
      scheduler_->add_handle(task);
    }

    void await_resume() noexcept {}
  };

  Scheduler(size_t thread_count = std::thread::hardware_concurrency()) {
    for (size_t i = 0; i < thread_count; i++) {
      threads_.emplace_back([this]() {
        while (true) {
          std::coroutine_handle<> handle;
          {
            std::unique_lock<std::mutex> lock(mtx);
            cv.wait(lock,
                    [this]() { return !work_queue_.empty() || stop_flag_; });
            if (stop_flag_) {
              break;
            }
            handle = work_queue_.front();
            work_queue_.pop();
          }

          if (handle) {
            handle.resume();
          }
        }
      });
    }
  }

  ~Scheduler() {
    {
      std::lock_guard<std::mutex> lock(mtx);
      stop_flag_ = true;
    }
    cv.notify_all();
    for (size_t i = 0; i < threads_.size(); i++) {
      threads_[i].join();
    }
  }

  // 提交一个 Task 到调度器
  template <typename T> void spawn(Task<T> &&task) noexcept {
    std::lock_guard<std::mutex> lock(mtx);
    work_queue_.push(task.get_handle());
    task.detach();
    cv.notify_one();
  }

  template <typename T>
  void add_handle(std::coroutine_handle<T> handle) noexcept {
    std::lock_guard<std::mutex> lock(mtx);
    work_queue_.push(handle);
    cv.notify_one();
  }

  Awaiter schedule() { return Awaiter(this); }

private:
  std::mutex mtx;
  std::condition_variable cv;
  std::vector<std::thread> threads_;
  std::queue<std::coroutine_handle<>> work_queue_;
  std::atomic<bool> stop_flag_{false};
};
} // namespace tiny_coroutine