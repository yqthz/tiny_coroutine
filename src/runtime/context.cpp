#include "runtime/context.h"

namespace tiny_coroutine::runtime {

thread_local Context *local_context_ptr = nullptr;

Context::Context(size_t id, Scheduler *owner_scheduler,
                 std::atomic<size_t> *pending_tasks,
                 OnTaskCompleted on_task_completed)
    : id_(id), owner_scheduler_(owner_scheduler), pending_tasks_(pending_tasks),
      on_task_completed_(std::move(on_task_completed)) {}

void Context::start() {
  worker_ = std::jthread([this](std::stop_token token) { run(token); });
}

void Context::notify_stop() {
  worker_.request_stop();
  engine_.notify_all();
}

void Context::join() {
  if (worker_.joinable()) {
    worker_.join();
  }
}

void Context::submit_task(std::coroutine_handle<> handle) {
  engine_.submit_task(handle);
}

void Context::submit_tracked_task(std::coroutine_handle<> handle) {
  if (!handle) {
    return;
  }
  pending_tasks_->fetch_add(1, std::memory_order_release);
  engine_.submit_task(handle);
}

size_t Context::id() const noexcept { return id_; }

void Context::run(std::stop_token token) {
  local_scheduler_ptr = owner_scheduler_;
  local_context_ptr = this;

  while (true) {
    if (process_work_once()) {
      continue;
    }

    if (poll_io_once()) {
      continue;
    }

    if (token.stop_requested() && engine_.empty()) {
      break;
    }

    wait_or_idle(token);
  }

  local_context_ptr = nullptr;
  local_scheduler_ptr = nullptr;
}

bool Context::process_work_once() {
  auto handle = engine_.try_pop_task();
  if (!handle) {
    return false;
  }

  handle.resume();
  if (handle.done()) {
    handle.destroy();
    on_task_completed_();
  }

  return true;
}

bool Context::poll_io_once() {
  // Placeholder for io_uring submit/completion processing.
  return false;
}

void Context::wait_or_idle(std::stop_token token) {
  engine_.wait_for_work_or_stop(token);
}

} // namespace tiny_coroutine::runtime
