#pragma once

#include <atomic>
#include <coroutine>
#include <iostream>
#include <mutex>
#include <queue>
#include <stdexcept>

namespace tiny_coroutine {

template <typename T> class Channel {
public:
  struct Awaiter {
    bool is_wait_{false};
    Channel<T> *channel_{nullptr};
    bool is_sender_{true};
    T value_;

    Awaiter(bool is_wait, Channel<T> *channel, bool is_sender, T value = T{})
        : is_wait_(is_wait), channel_(channel), is_sender_(is_sender),
          value_(value) {}

    bool await_ready() noexcept { return !is_wait_; }

    template <typename U>
    std::coroutine_handle<>
    await_suspend(std::coroutine_handle<U> handle) noexcept {
      std::lock_guard<std::mutex> lock(channel_->mtx);
      if (is_sender_) {
        channel_->sender_wait_queue_.push(handle);
        if (!channel_->receiver_wait_queue_.empty()) {
          auto receiver_handle = channel_->receiver_wait_queue_.front();
          channel_->receiver_wait_queue_.pop();
          return receiver_handle;
        }
      } else {
        channel_->receiver_wait_queue_.push(handle);
        if (!channel_->sender_wait_queue_.empty()) {
          auto sender_handle = channel_->sender_wait_queue_.front();
          channel_->sender_wait_queue_.pop();
          return sender_handle;
        }
      }
      return std::noop_coroutine();
    }

    T await_resume() noexcept {
      if (is_wait_) {
        std::lock_guard<std::mutex> lock(channel_->mtx);
        if (is_sender_) {
          if (!channel_->isFull()) {
            channel_->buffer_[channel_->tail_] = value_;
            channel_->tail_ = (channel_->tail_ + 1) % channel_->capacity_;
          }
        } else {
          if (!channel_->isEmpty()) {
            value_ = channel_->buffer_[channel_->header_];
            channel_->header_ = (channel_->header_ + 1) % channel_->capacity_;
          }
        }
      }
      return value_;
    }
  };

  Channel() = default;
  explicit Channel(int capacity) : header_(0), tail_(0), capacity_(capacity) {
    buffer_.resize(capacity);
  }

  ~Channel() = default;

  bool isEmpty() noexcept { return header_ == tail_; }

  bool isFull() noexcept { return (tail_ + 1) % capacity_ == header_; }

  Awaiter send(T value) {
    if (close_) {
      throw std::runtime_error("channel is close");
    }
    std::unique_lock<std::mutex> lock(mtx);
    if (!isFull()) {
      buffer_[tail_] = value;
      tail_ = (tail_ + 1) % capacity_;
      if (!receiver_wait_queue_.empty()) {
        auto receiver_handle = receiver_wait_queue_.front();
        receiver_wait_queue_.pop();
        lock.unlock();
        receiver_handle.resume();
      }
      return Awaiter{false, this, true};
    }
    std::cout << "buffer is full, sender suspend" << std::endl;
    return Awaiter{true, this, true, value};
  }

  Awaiter receive() noexcept {
    std::unique_lock<std::mutex> lock(mtx);
    if (isEmpty()) {
      std::cout << "buffer is empty, receiver suspend" << std::endl;
      return Awaiter{true, this, false};
    }
    auto value = buffer_[header_];
    header_ = (header_ + 1) % capacity_;
    if (!sender_wait_queue_.empty()) {
      auto sender_handle = sender_wait_queue_.front();
      sender_wait_queue_.pop();
      lock.unlock();
      sender_handle.resume();
    }
    return Awaiter{false, this, false, value};
  }

  void close() {
    std::unique_lock<std::mutex> lock(mtx);
    if (close_) {
      throw std::runtime_error("channel is close");
    }

    std::vector<std::coroutine_handle<>> handles;
    while (!receiver_wait_queue_.empty()) {
      auto handle = receiver_wait_queue_.front();
      receiver_wait_queue_.pop();
      handles.push_back(handle);
    }
    close_ = true;
    lock.unlock();

    for (auto handle : handles) {
      handle.resume();
    }
  }

private:
  std::vector<T> buffer_;
  size_t header_{0};
  size_t tail_{0};
  size_t capacity_{0};
  std::queue<std::coroutine_handle<>> sender_wait_queue_;
  std::queue<std::coroutine_handle<>> receiver_wait_queue_;
  std::atomic<bool> close_{false};
  std::mutex mtx;
};
} // namespace tiny_coroutine