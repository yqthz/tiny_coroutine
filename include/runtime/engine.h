#pragma once

#include <condition_variable>
#include <coroutine>
#include <mutex>
#include <queue>
#include <stop_token>

namespace tiny_coroutine::runtime {

class Engine {
public:
  void submit_task(std::coroutine_handle<> handle);
  std::coroutine_handle<> try_pop_task();
  void wait_for_work_or_stop(std::stop_token token);
  bool empty() const;
  void notify_all();

private:
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::queue<std::coroutine_handle<>> task_queue_;
};

} // namespace tiny_coroutine::runtime
