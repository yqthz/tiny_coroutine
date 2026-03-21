#pragma once

#include "async_io.h"
#include "io_uring.h"
#include "task.h"

#include <coroutine>
#include <cstdint>
#include <queue>

namespace tiny_coroutine {

struct AsyncRead;
struct AsyncWrite;

class IoContext {
public:
  IoContext() {}

  template <typename T> void spawn(Task<T> &&task) {
    ready_queue_.push(task.get_handle());
    task.detach();
  }

  void run() {
    while (!ready_queue_.empty() || io_count_ > 0) {
      while (!ready_queue_.empty()) {
        auto handle = ready_queue_.front();
        ready_queue_.pop();
        handle.resume();
      }

      if (io_count_ > 0) {
        auto info = uring_.wait_one();
        auto awaiter = reinterpret_cast<AsyncIoBase *>(info.user_data);
        awaiter->set_result(info.result);
        io_count_--;

        if (awaiter->handle_) {
          ready_queue_.push(awaiter->handle_);
        }
      }
    }
  }

  void submit_read(int fd, void *buf, size_t len, size_t offset,
                   AsyncRead *async_read) {
    uring_.submit_read(fd, buf, len, offset,
                       reinterpret_cast<uint64_t>(async_read));
    uring_.submit();
    io_count_++;
  }

  void submit_write(int fd, const void *buf, size_t len, size_t offset,
                    AsyncWrite *async_write) {
    uring_.submit_write(fd, buf, len, offset,
                        reinterpret_cast<uint64_t>(async_write));
    uring_.submit();
    io_count_++;
  }

  AsyncRead async_read(int fd, void *data, size_t len, size_t offset) {
    return AsyncRead{*this, fd, data, len, offset};
  }

  AsyncWrite async_write(int fd, const void *data, size_t len, size_t offset) {
    return AsyncWrite{*this, fd, data, len, offset};
  }

private:
  IoUring uring_;
  std::queue<std::coroutine_handle<>> ready_queue_;
  size_t io_count_{0};
};
} // namespace tiny_coroutine

#include "async_io.inl"