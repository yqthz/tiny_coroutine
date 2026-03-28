#pragma once

#include "engine.h"

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <functional>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

namespace tiny_coroutine::runtime {

class Context;
class IoReadAwaiter;
class IoWriteAwaiter;

extern thread_local Context *local_context_ptr;

class Context {
public:
  using OnTaskCompleted = std::function<void()>;

  Context(size_t id, std::atomic<size_t> *pending_tasks,
          OnTaskCompleted on_task_completed);

  Context(const Context &) = delete;
  Context(Context &&) = delete;
  Context &operator=(const Context &) = delete;
  Context &operator=(Context &&) = delete;

  void start();

  void notify_stop();

  void join();

  void submit_task(std::coroutine_handle<> handle);
  void submit_io_waiting(std::coroutine_handle<> handle);
  void submit_io_read(IoReadAwaiter *awaiter);
  void submit_io_write(IoWriteAwaiter *awaiter);

  void submit_tracked_task(std::coroutine_handle<> handle);

  size_t id() const noexcept;

private:
  static constexpr size_t kProcessBatchSize = 64;
  static constexpr size_t kIoPollBatchSize = 64;

  void run(std::stop_token token);
  bool process_work_once();
  bool poll_io_once();
  void wait_or_idle(std::stop_token token);

  size_t id_{0};
  std::atomic<size_t> *pending_tasks_{nullptr}; // 待执行的任务数
  Engine engine_;                     // 负责管理任务和 IO 操作
  std::jthread worker_;               // 工作线程
  OnTaskCompleted on_task_completed_; // task 完成时的回调
  std::vector<std::coroutine_handle<>> work_batch_; // 待执行的任务列表
};

inline Context *try_local_context() noexcept { return local_context_ptr; }

inline Context &local_context() noexcept { return *local_context_ptr; }

} // namespace tiny_coroutine::runtime
