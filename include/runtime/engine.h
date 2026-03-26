#pragma once

#include "io_uring.h"

#include <atomic>
#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <mutex>
#include <queue>
#include <stop_token>
#include <vector>

namespace tiny_coroutine::runtime {

class IoOpAwaiterBase;
class IoReadAwaiter;
class IoWriteAwaiter;

class Engine {
public:
  void submit_task(std::coroutine_handle<> handle);
  void submit_io_waiting(std::coroutine_handle<> handle);
  void submit_io_read(IoReadAwaiter *awaiter);
  void submit_io_write(IoWriteAwaiter *awaiter);

  std::coroutine_handle<> try_pop_task();
  size_t pop_batch(std::vector<std::coroutine_handle<>> &out, size_t max_n);
  size_t poll_io(size_t max_n);

  void on_task_resume_begin() noexcept;
  void on_task_resume_end() noexcept;

  void on_io_inflight_begin(size_t count = 1) noexcept;
  void on_io_inflight_end(size_t count = 1) noexcept;

  bool can_stop(std::stop_token token) const;
  void wait_for_work_or_stop(std::stop_token token);
  bool empty() const;
  void notify_all();

private:
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::queue<std::coroutine_handle<>> task_queue_; // 待执行的任务队列
  std::queue<std::coroutine_handle<>> io_waiting_queue_; // 待执行的 IO 操作队列
  std::queue<IoReadAwaiter *> io_read_submit_queue_; // 待提交的读 IO 操作队列
  std::queue<IoWriteAwaiter *> io_write_submit_queue_; // 待提交的写 IO 操作队列

  IoUring uring_;
  size_t pending_submit_count_{0}; // 待提交的 IO 操作数

  std::atomic<size_t> inflight_tasks_{0}; // 正在执行的任务数
  std::atomic<size_t> inflight_io_{0};    // 正在执行的 IO 操作数
};

} // namespace tiny_coroutine::runtime
