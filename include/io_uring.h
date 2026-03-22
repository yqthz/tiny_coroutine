#pragma once

#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <liburing.h>
#include <liburing/io_uring.h>
#include <stdexcept>

namespace tiny_coroutine {

struct IoInfo {
  uint64_t user_data;
  int32_t result;
};

class IoUring {
public:
  IoUring(size_t queue_depth = 256) {
    int ret = io_uring_queue_init(queue_depth, &uring_, 0);
    if (ret < 0) {
      throw std::runtime_error("io_uring_queue_init failed");
    }
  }
  ~IoUring() { io_uring_queue_exit(&uring_); }

  void submit_read(int fd, void *buf, size_t len, size_t offset,
                   uint64_t user_data) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&uring_);
    if (sqe == nullptr) {
      throw std::runtime_error("io_uring_get_sqe failed");
    }
    io_uring_prep_read(sqe, fd, buf, len, offset);
    sqe->user_data = user_data;
  }

  void submit_write(int fd, const void *buf, size_t len, size_t offset,
                    uint64_t user_data) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&uring_);
    if (sqe == nullptr) {
      throw std::runtime_error("io_uring_get_sqe failed");
    }
    io_uring_prep_write(sqe, fd, buf, len, offset);
    sqe->user_data = user_data;
  }

  int submit() { return io_uring_submit(&uring_); }

  IoInfo wait_one() {
    struct io_uring_cqe *cqe;
    int ret = io_uring_wait_cqe(&uring_, &cqe);
    if (ret < 0) {
      throw std::runtime_error("io_uring_wait_cqe failed");
    }

    IoInfo info{.user_data = cqe->user_data, .result = cqe->res};

    io_uring_cqe_seen(&uring_, cqe);
    return info;
  }

  auto poll() {
    struct io_uring_cqe *cqe;
    io_uring_peek_cqe(&uring_, &cqe);
    return cqe != nullptr;
  }

private:
  struct io_uring uring_;
};
} // namespace tiny_coroutine