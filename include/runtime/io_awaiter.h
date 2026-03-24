#pragma once

#include "runtime/context.h"

#include <coroutine>
#include <cstddef>
#include <cstdint>

namespace tiny_coroutine::runtime {

class IoSuspendOnceAwaiter {
public:
  bool await_ready() const noexcept { return false; }

  bool await_suspend(std::coroutine_handle<> handle) const noexcept {
    auto *context = try_local_context();
    if (context == nullptr) {
      return false;
    }

    context->submit_io_waiting(handle);
    return true;
  }

  void await_resume() const noexcept {}
};

class IoOpAwaiterBase {
public:
  bool await_ready() const noexcept { return false; }
  int32_t await_resume() const noexcept { return result_; }

  int fd() const noexcept { return fd_; }
  size_t len() const noexcept { return len_; }
  size_t offset() const noexcept { return offset_; }

  std::coroutine_handle<> handle() const noexcept { return handle_; }
  void set_result(int32_t result) noexcept { result_ = result; }

protected:
  IoOpAwaiterBase(int fd, size_t len, size_t offset)
      : fd_(fd), len_(len), offset_(offset) {}

  void set_handle(std::coroutine_handle<> handle) noexcept { handle_ = handle; }

private:
  int fd_;
  size_t len_;
  size_t offset_;

  int32_t result_{0};
  std::coroutine_handle<> handle_{};
};

class IoReadAwaiter : public IoOpAwaiterBase {
public:
  IoReadAwaiter(int fd, void *buf, size_t len, size_t offset)
      : IoOpAwaiterBase(fd, len, offset), buf_(buf) {}

  bool await_suspend(std::coroutine_handle<> handle) noexcept {
    set_handle(handle);

    auto *context = try_local_context();
    if (context == nullptr) {
      set_result(-1);
      return false;
    }

    context->submit_io_read(this);
    return true;
  }

  void *buf() const noexcept { return buf_; }

private:
  void *buf_;
};

class IoWriteAwaiter : public IoOpAwaiterBase {
public:
  IoWriteAwaiter(int fd, const void *buf, size_t len, size_t offset)
      : IoOpAwaiterBase(fd, len, offset), buf_(buf) {}

  bool await_suspend(std::coroutine_handle<> handle) noexcept {
    set_handle(handle);

    auto *context = try_local_context();
    if (context == nullptr) {
      set_result(-1);
      return false;
    }

    context->submit_io_write(this);
    return true;
  }

  const void *buf() const noexcept { return buf_; }

private:
  const void *buf_;
};

inline IoSuspendOnceAwaiter io_suspend_once() noexcept { return {}; }

namespace io {
inline IoReadAwaiter read(int fd, void *buf, size_t len,
                          size_t offset = 0) noexcept {
  return IoReadAwaiter(fd, buf, len, offset);
}

inline IoWriteAwaiter write(int fd, const void *buf, size_t len,
                            size_t offset = 0) noexcept {
  return IoWriteAwaiter(fd, buf, len, offset);
}
} // namespace io

} // namespace tiny_coroutine::runtime
