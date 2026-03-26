#include "runtime/engine.h"

#include "runtime/io_awaiter.h"

#include <stdexcept>

namespace tiny_coroutine::runtime {

void Engine::submit_task(std::coroutine_handle<> handle) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    task_queue_.push(handle);
  }
  cv_.notify_one();
}

void Engine::submit_io_waiting(std::coroutine_handle<> handle) {
  on_io_inflight_begin();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    io_waiting_queue_.push(handle);
  }
  cv_.notify_one();
}

void Engine::submit_io_read(IoReadAwaiter *awaiter) {
  if (awaiter == nullptr) {
    return;
  }

  on_io_inflight_begin();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    io_read_submit_queue_.push(awaiter);
  }
  cv_.notify_one();
}

void Engine::submit_io_write(IoWriteAwaiter *awaiter) {
  if (awaiter == nullptr) {
    return;
  }

  on_io_inflight_begin();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    io_write_submit_queue_.push(awaiter);
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

size_t Engine::pop_batch(std::vector<std::coroutine_handle<>> &out,
                         size_t max_n) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (task_queue_.empty() || max_n == 0) {
    return 0;
  }

  out.clear();
  while (out.size() < max_n && !task_queue_.empty()) {
    out.push_back(task_queue_.front());
    task_queue_.pop();
  }

  return out.size();
}

// 提交 IO 操作
size_t Engine::poll_io(size_t max_n) {
  if (max_n == 0) {
    return 0;
  }

  size_t progressed = 0;

  {
    std::lock_guard<std::mutex> lock(mutex_);

    while (progressed < max_n && !io_waiting_queue_.empty()) {
      task_queue_.push(io_waiting_queue_.front());
      io_waiting_queue_.pop();
      // 减少正在执行的 IO 操作计数
      on_io_inflight_end();
      ++progressed;
    }

    size_t prepared = 0;
    while (prepared < max_n && !io_read_submit_queue_.empty()) {
      auto *awaiter = io_read_submit_queue_.front();
      io_read_submit_queue_.pop();

      // 提交读 IO 操作
      uring_.submit_read(awaiter->fd(), awaiter->buf(), awaiter->len(),
                         awaiter->offset(),
                         reinterpret_cast<uint64_t>(awaiter));

      // 增加待提交的 IO 操作计数
      ++pending_submit_count_;
      ++prepared;
    }

    while (prepared < max_n && !io_write_submit_queue_.empty()) {
      auto *awaiter = io_write_submit_queue_.front();
      io_write_submit_queue_.pop();

      uring_.submit_write(awaiter->fd(), awaiter->buf(), awaiter->len(),
                          awaiter->offset(),
                          reinterpret_cast<uint64_t>(awaiter));

      ++pending_submit_count_;
      ++prepared;
    }

    while (pending_submit_count_ > 0) {
      const int submitted = uring_.submit();
      if (submitted <= 0) {
        throw std::runtime_error("io_uring_submit failed in runtime::Engine");
      }

      const size_t submitted_size = static_cast<size_t>(submitted);
      if (submitted_size > pending_submit_count_) {
        throw std::runtime_error("io_uring_submit returned invalid count");
      }
      pending_submit_count_ -= submitted_size;
      progressed += submitted_size;
    }
  }

  size_t completed = 0;
  IoInfo info{};
  while (completed < max_n && uring_.try_wait_one(info)) {
    auto *awaiter = reinterpret_cast<IoOpAwaiterBase *>(info.user_data);
    awaiter->set_result(info.result);

    {
      std::lock_guard<std::mutex> lock(mutex_);
      task_queue_.push(awaiter->handle());
    }

    on_io_inflight_end();
    ++completed;
  }

  if (completed > 0) {
    progressed += completed;
  }

  return progressed;
}

void Engine::on_task_resume_begin() noexcept {
  inflight_tasks_.fetch_add(1, std::memory_order_acq_rel);
}

void Engine::on_task_resume_end() noexcept {
  inflight_tasks_.fetch_sub(1, std::memory_order_acq_rel);
}

void Engine::on_io_inflight_begin(size_t count) noexcept {
  if (count == 0) {
    return;
  }
  inflight_io_.fetch_add(count, std::memory_order_acq_rel);
}

void Engine::on_io_inflight_end(size_t count) noexcept {
  if (count == 0) {
    return;
  }
  inflight_io_.fetch_sub(count, std::memory_order_acq_rel);
}

bool Engine::can_stop(std::stop_token token) const {
  if (token.stop_requested() == false) {
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (task_queue_.empty() == false || io_waiting_queue_.empty() == false ||
        io_read_submit_queue_.empty() == false ||
        io_write_submit_queue_.empty() == false || pending_submit_count_ != 0) {
      return false;
    }
  }

  if (inflight_tasks_.load(std::memory_order_acquire) != 0) {
    return false;
  }

  if (inflight_io_.load(std::memory_order_acquire) != 0) {
    return false;
  }

  return true;
}

void Engine::wait_for_work_or_stop(std::stop_token token) {
  std::unique_lock<std::mutex> lock(mutex_);
  cv_.wait(lock, [this, &token]() {
    return token.stop_requested() || task_queue_.empty() == false ||
           io_waiting_queue_.empty() == false ||
           io_read_submit_queue_.empty() == false ||
           io_write_submit_queue_.empty() == false ||
           pending_submit_count_ != 0 ||
           inflight_io_.load(std::memory_order_acquire) != 0;
  });
}

bool Engine::empty() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return task_queue_.empty() && io_waiting_queue_.empty() &&
         io_read_submit_queue_.empty() && io_write_submit_queue_.empty() &&
         pending_submit_count_ == 0;
}

void Engine::notify_all() { cv_.notify_all(); }

} // namespace tiny_coroutine::runtime
