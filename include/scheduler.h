#pragma once

#include "task.h"

#include <atomic>
#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

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

  explicit Scheduler(
      size_t thread_count = std::thread::hardware_concurrency(),
      size_t batch_size = 32)
      : batch_size_(batch_size == 0 ? 1 : batch_size) {
    for (size_t i = 0; i < thread_count; i++) {
      threads_.emplace_back([this]() {
        std::vector<std::coroutine_handle<>> local_batch;
        local_batch.reserve(batch_size_);

        while (true) {
          {
            std::unique_lock<std::mutex> lock(mtx);
            cv.wait(lock,
                    [this]() { return stop_flag_ || !work_queue_.empty(); });

            if (stop_flag_ && work_queue_.empty()) {
              break;
            }

            while (!work_queue_.empty() && local_batch.size() < batch_size_) {
              local_batch.push_back(work_queue_.front());
              work_queue_.pop();
            }
          }

          for (auto handle : local_batch) {
            if (handle) {
              handle.resume();
            }
          }
          local_batch.clear();
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
      if (threads_[i].joinable()) {
        threads_[i].join();
      }
    }
  }

  // 提交一个 Task 到调度器
  template <typename T> void spawn(Task<T> &&task) noexcept {
    {
      std::lock_guard<std::mutex> lock(mtx);
      work_queue_.push(task.get_handle());
      task.detach();
    }
    cv.notify_one();
  }

  template <typename T>
  void add_handle(std::coroutine_handle<T> handle) noexcept {
    {
      std::lock_guard<std::mutex> lock(mtx);
      work_queue_.push(handle);
    }
    cv.notify_one();
  }

  Awaiter schedule() { return Awaiter(this); }

private:
  std::mutex mtx;
  std::condition_variable cv;
  std::vector<std::thread> threads_;
  std::queue<std::coroutine_handle<>> work_queue_;
  std::atomic<bool> stop_flag_{false};
  size_t batch_size_{32};
};
} // namespace tiny_coroutine
