#include "runtime/scheduler.h"

namespace tiny_coroutine::runtime {

thread_local Scheduler *local_scheduler_ptr = nullptr;

Scheduler::~Scheduler() { stop(); }

void Scheduler::init(size_t context_count) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  if (initialized_) {
    return;
  }

  const size_t worker_count = context_count == 0 ? 1 : context_count;
  contexts_.reserve(worker_count);
  for (size_t i = 0; i < worker_count; ++i) {
    contexts_.push_back(std::make_unique<Context>(
        i, this, &pending_tasks_, [this]() noexcept { on_task_completed(); }));
  }
  for (auto &context : contexts_) {
    context->start();
  }

  dispatcher_.init(worker_count);
  pending_tasks_.store(0, std::memory_order_relaxed);
  stop_requested_.store(false, std::memory_order_relaxed);
  accepting_ = true;
  initialized_ = true;
}

void Scheduler::submit(Task<void> &&task) {
  auto handle = task.get_handle();
  task.detach();
  submit_new(handle);
}

void Scheduler::submit(std::coroutine_handle<> handle) { submit_new(handle); }

void Scheduler::loop() {
  std::unique_lock<std::mutex> lock(state_mutex_);
  if (!initialized_) {
    throw std::logic_error("runtime::Scheduler::init must be called before loop");
  }

  completion_cv_.wait(lock, [this]() {
    return stop_requested_.load(std::memory_order_acquire) ||
           pending_tasks_.load(std::memory_order_acquire) == 0;
  });

  accepting_ = false;
  lock.unlock();

  shutdown_contexts();
}

void Scheduler::stop() {
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!initialized_) {
      return;
    }
    accepting_ = false;
    stop_requested_.store(true, std::memory_order_release);
  }

  completion_cv_.notify_all();
  shutdown_contexts();
}

void Scheduler::submit_new(std::coroutine_handle<> handle) {
  if (!handle) {
    return;
  }

  Context *target = nullptr;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!initialized_ || !accepting_) {
      handle.destroy();
      return;
    }
    pending_tasks_.fetch_add(1, std::memory_order_release);
    target = contexts_[dispatcher_.dispatch()].get();
  }

  target->submit_task(handle);
}

void Scheduler::reschedule(std::coroutine_handle<> handle) {
  if (!handle) {
    return;
  }

  if (auto *ctx = try_local_context(); ctx != nullptr) {
    ctx->submit_task(handle);
    return;
  }

  Context *target = nullptr;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!initialized_ || !accepting_) {
      handle.destroy();
      return;
    }
    target = contexts_[dispatcher_.dispatch()].get();
  }

  target->submit_task(handle);
}

void Scheduler::on_task_completed() {
  if (pending_tasks_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    completion_cv_.notify_all();
  }
}

void Scheduler::shutdown_contexts() {
  std::vector<std::unique_ptr<Context>> contexts;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!initialized_) {
      return;
    }
    contexts.swap(contexts_);
    initialized_ = false;
  }

  for (auto &context : contexts) {
    context->notify_stop();
  }
  for (auto &context : contexts) {
    context->join();
  }
}

} // namespace tiny_coroutine::runtime
