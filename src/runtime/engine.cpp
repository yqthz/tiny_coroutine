#include "runtime/engine.h"

namespace tiny_coroutine::runtime {

void Engine::submit_task(std::coroutine_handle<> handle) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    task_queue_.push(handle);
  }
  cv_.notify_one();
}

std::coroutine_handle<> Engine::try_pop_task() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (task_queue_.empty()) {
    return {};
  }

  auto handle = task_queue_.front();
  task_queue_.pop();
  return handle;
}

void Engine::wait_for_work_or_stop(std::stop_token token) {
  std::unique_lock<std::mutex> lock(mutex_);
  cv_.wait(lock, [this, &token]() {
    return token.stop_requested() || !task_queue_.empty();
  });
}

bool Engine::empty() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return task_queue_.empty();
}

void Engine::notify_all() { cv_.notify_all(); }

} // namespace tiny_coroutine::runtime
