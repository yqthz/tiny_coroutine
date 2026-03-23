#pragma once

#include "async_io.h"
#include "io_uring.h"
#include "task.h"

#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <queue>
#include <stdexcept>

namespace tiny_coroutine {

struct AsyncRead;
struct AsyncWrite;

class IoContext {
public:
  explicit IoContext(size_t queue_depth = 256, size_t submit_batch_size = 32,
                     size_t completion_batch_size = 32)
      : uring_(queue_depth),
        submit_batch_size_(submit_batch_size == 0 ? 1 : submit_batch_size),
        completion_batch_size_(completion_batch_size == 0
                                   ? 1
                                   : completion_batch_size) {}

  template <typename T> void spawn(Task<T> &&task) {
    ready_queue_.push(task.get_handle());
    task.detach();
  }

  void run() {
    while (!ready_queue_.empty() || in_flight_count_ > 0 ||
           pending_submit_count_ > 0) {
      while (!ready_queue_.empty()) {
        auto handle = ready_queue_.front();
        ready_queue_.pop();
        handle.resume();
      }

      flush_submissions();

      if (in_flight_count_ == 0) {
        continue;
      }

      // Wait for at least one CQE, then drain more CQEs in batch.
      handle_completion(uring_.wait_one());

      IoInfo info{};
      size_t drained = 1;
      while (drained < completion_batch_size_ && uring_.try_wait_one(info)) {
        handle_completion(info);
        ++drained;
      }
    }
  }

  void submit_read(int fd, void *buf, size_t len, size_t offset,
                   AsyncRead *async_read) {
    uring_.submit_read(fd, buf, len, offset,
                       reinterpret_cast<uint64_t>(async_read));
    on_prepared_submission();
  }

  void submit_write(int fd, const void *buf, size_t len, size_t offset,
                    AsyncWrite *async_write) {
    uring_.submit_write(fd, buf, len, offset,
                        reinterpret_cast<uint64_t>(async_write));
    on_prepared_submission();
  }

  AsyncRead async_read(int fd, void *data, size_t len, size_t offset) {
    return AsyncRead{*this, fd, data, len, offset};
  }

  AsyncWrite async_write(int fd, const void *data, size_t len, size_t offset) {
    return AsyncWrite{*this, fd, data, len, offset};
  }

private:
  void on_prepared_submission() {
    ++pending_submit_count_;
    if (pending_submit_count_ >= submit_batch_size_) {
      flush_submissions();
    }
  }

  void flush_submissions() {
    while (pending_submit_count_ > 0) {
      const int submitted = uring_.submit();
      if (submitted < 0) {
        throw std::runtime_error("io_uring_submit failed");
      }
      if (submitted == 0) {
        throw std::runtime_error("io_uring_submit submitted 0 entries");
      }

      const size_t submitted_size = static_cast<size_t>(submitted);
      if (submitted_size > pending_submit_count_) {
        throw std::runtime_error("io_uring_submit returned invalid count");
      }

      pending_submit_count_ -= submitted_size;
      in_flight_count_ += submitted_size;
    }
  }

  void handle_completion(const IoInfo &info) {
    auto *awaiter = reinterpret_cast<AsyncIoBase *>(info.user_data);
    awaiter->set_result(info.result);

    if (in_flight_count_ == 0) {
      throw std::runtime_error("io completion underflow");
    }
    --in_flight_count_;

    if (awaiter->handle_) {
      ready_queue_.push(awaiter->handle_);
    }
  }

  IoUring uring_;
  std::queue<std::coroutine_handle<>> ready_queue_;
  size_t pending_submit_count_{0};
  size_t in_flight_count_{0};
  size_t submit_batch_size_{32};
  size_t completion_batch_size_{32};
};
} // namespace tiny_coroutine

#include "async_io.inl"
