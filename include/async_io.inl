#pragma once

namespace tiny_coroutine {

template <typename T>
bool AsyncRead::await_suspend(std::coroutine_handle<T> handle) {
  handle_ = handle;
  ctx_.submit_read(fd_, buf_, len_, offset_, this);
  return true;
}

template <typename T>
bool AsyncWrite::await_suspend(std::coroutine_handle<T> handle) {
  handle_ = handle;
  ctx_.submit_write(fd_, buf_, len_, offset_, this);
  return true;
}

} // namespace tiny_coroutine