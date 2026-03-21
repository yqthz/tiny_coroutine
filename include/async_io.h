#pragma once

#include "io_uring.h"

#include <coroutine>
#include <cstdint>

namespace tiny_coroutine {

class IoContext;

struct AsyncIoBase {
  IoContext &ctx_;
  int fd_;
  size_t len_;
  size_t offset_;
  int32_t result_{0};
  std::coroutine_handle<> handle_;

  AsyncIoBase(IoContext &ctx, int fd, size_t len, size_t offset);

  bool await_ready();

  size_t await_resume();

  void set_result(int32_t result);
};

struct AsyncRead : public AsyncIoBase {
  void *buf_;

  AsyncRead(IoContext &ctx, int fd, void *buf, size_t len, size_t offset);

  template <typename T> bool await_suspend(std::coroutine_handle<T> handle);
};

struct AsyncWrite : public AsyncIoBase {
  const void *buf_;

  AsyncWrite(IoContext &ctx, int fd, const void *buf, size_t len,
             size_t offset);

  template <typename T> bool await_suspend(std::coroutine_handle<T> handle);
};

} // namespace tiny_coroutine