#pragma once

#include "task.h"

#include <atomic>
#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <cstdint>
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

  struct StatsSnapshot {
    std::uint64_t enqueued{0};
    std::uint64_t dequeued{0};
    std::uint64_t resumed{0};
    std::uint64_t notify_calls{0};
    std::uint64_t worker_waits{0};
    std::uint64_t worker_wakeups{0};
  };

  explicit Scheduler(
      size_t thread_count = std::thread::hardware_concurrency(),
      size_t batch_size = 32)
      : batch_size_(batch_size == 0 ? 1 : batch_size) {
    const size_t worker_count = thread_count == 0 ? 1 : thread_count;
    for (size_t i = 0; i < worker_count; i++) {
      threads_.emplace_back([this]() {
        std::vector<std::coroutine_handle<>> local_batch;
        local_batch.reserve(batch_size_);

        while (true) {
          {
            std::unique_lock<std::mutex> lock(mtx);
            stats_worker_waits_.fetch_add(1, std::memory_order_relaxed);
            ++idle_workers_;
            cv.wait(lock,
                    [this]() { return stop_flag_ || !work_queue_.empty(); });
            --idle_workers_;
            stats_worker_wakeups_.fetch_add(1, std::memory_order_relaxed);

            if (stop_flag_ && work_queue_.empty()) {
              break;
            }

            while (!work_queue_.empty() && local_batch.size() < batch_size_) {
              local_batch.push_back(work_queue_.front());
              work_queue_.pop();
            }
          }

          stats_dequeued_.fetch_add(static_cast<std::uint64_t>(local_batch.size()),
                                    std::memory_order_relaxed);
          for (auto handle : local_batch) {
            if (handle) {
              handle.resume();
              stats_resumed_.fetch_add(1, std::memory_order_relaxed);
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
    enqueue_handle(task.get_handle());
    task.detach();
  }

  template <typename T>
  void add_handle(std::coroutine_handle<T> handle) noexcept {
    enqueue_handle(handle);
  }

  Awaiter schedule() { return Awaiter(this); }

  StatsSnapshot stats_snapshot() const noexcept {
    return StatsSnapshot{
        .enqueued = stats_enqueued_.load(std::memory_order_relaxed),
        .dequeued = stats_dequeued_.load(std::memory_order_relaxed),
        .resumed = stats_resumed_.load(std::memory_order_relaxed),
        .notify_calls = stats_notify_calls_.load(std::memory_order_relaxed),
        .worker_waits = stats_worker_waits_.load(std::memory_order_relaxed),
        .worker_wakeups = stats_worker_wakeups_.load(std::memory_order_relaxed),
    };
  }

  void reset_stats() noexcept {
    stats_enqueued_.store(0, std::memory_order_relaxed);
    stats_dequeued_.store(0, std::memory_order_relaxed);
    stats_resumed_.store(0, std::memory_order_relaxed);
    stats_notify_calls_.store(0, std::memory_order_relaxed);
    stats_worker_waits_.store(0, std::memory_order_relaxed);
    stats_worker_wakeups_.store(0, std::memory_order_relaxed);
  }

private:
  void enqueue_handle(std::coroutine_handle<> handle) noexcept {
    bool should_notify = false;
    {
      std::lock_guard<std::mutex> lock(mtx);
      work_queue_.push(handle);
      should_notify = idle_workers_ > 0;
    }

    stats_enqueued_.fetch_add(1, std::memory_order_relaxed);
    if (should_notify) {
      stats_notify_calls_.fetch_add(1, std::memory_order_relaxed);
      cv.notify_one();
    }
  }

  std::mutex mtx;
  std::condition_variable cv;
  std::vector<std::thread> threads_;
  std::queue<std::coroutine_handle<>> work_queue_;
  std::atomic<bool> stop_flag_{false};
  size_t batch_size_{32};
  size_t idle_workers_{0};

  std::atomic<std::uint64_t> stats_enqueued_{0};
  std::atomic<std::uint64_t> stats_dequeued_{0};
  std::atomic<std::uint64_t> stats_resumed_{0};
  std::atomic<std::uint64_t> stats_notify_calls_{0};
  std::atomic<std::uint64_t> stats_worker_waits_{0};
  std::atomic<std::uint64_t> stats_worker_wakeups_{0};
};
} // namespace tiny_coroutine
