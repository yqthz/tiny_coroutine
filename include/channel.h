#pragma once

#include <atomic>
#include <coroutine>
#include <mutex>
#include <queue>
#include <stdexcept>

namespace tiny_coroutine {

template <typename T> class Channel {
public:
  struct SenderAwaiter;
  struct ReceiverAwaiter;

  struct SenderAwaiter {
    Channel<T> *channel_{nullptr};
    T value_;

    SenderAwaiter(Channel<T> *channel, T value)
        : channel_(channel), value_(std::move(value)) {}

    bool await_ready() noexcept { return false; }

    template <typename U>
    std::coroutine_handle<>
    await_suspend(std::coroutine_handle<U> handle) noexcept {
      std::lock_guard<std::mutex> lock(channel_->mtx_);

      // 有等待的 receiver，直接转移数据，唤醒它
      if (!channel_->receiver_queue_.empty()) {
        auto [receiver_handle, receiver_awaiter] =
            channel_->receiver_queue_.front();
        channel_->receiver_queue_.pop();
        receiver_awaiter->value_ = std::move(value_);
        return receiver_handle;
      }

      // buffer 未满，写入 buffer，不挂起
      if (!channel_->isFull()) {
        channel_->buffer_[channel_->tail_] = std::move(value_);
        channel_->tail_ = (channel_->tail_ + 1) % channel_->capacity_;
        return handle;
      }

      // buffer 满，挂起 sender
      channel_->sender_queue_.push({handle, this});
      return std::noop_coroutine();
    }

    void await_resume() noexcept {}
  };

  struct ReceiverAwaiter {
    Channel<T> *channel_{nullptr};
    T value_{};

    explicit ReceiverAwaiter(Channel<T> *channel) : channel_(channel) {}

    bool await_ready() noexcept { return false; }

    template <typename U>
    std::coroutine_handle<>
    await_suspend(std::coroutine_handle<U> handle) noexcept {
      std::lock_guard<std::mutex> lock(channel_->mtx_);

      // buffer 有数据，直接读取
      if (!channel_->isEmpty()) {
        value_ = std::move(channel_->buffer_[channel_->header_]);
        channel_->header_ = (channel_->header_ + 1) % channel_->capacity_;

        // 唤醒一个等待的 sender，让它写入 buffer
        if (!channel_->sender_queue_.empty()) {
          auto [sender_handle, sender_awaiter] =
              channel_->sender_queue_.front();
          channel_->sender_queue_.pop();
          channel_->buffer_[channel_->tail_] =
              std::move(sender_awaiter->value_);
          channel_->tail_ = (channel_->tail_ + 1) % channel_->capacity_;
          return sender_handle;
        }

        return handle;
      }

      // buffer 空，有等待的 sender，直接取数据
      if (!channel_->sender_queue_.empty()) {
        auto [sender_handle, sender_awaiter] = channel_->sender_queue_.front();
        channel_->sender_queue_.pop();
        value_ = std::move(sender_awaiter->value_);
        return sender_handle;
      }

      // 无数据，挂起 receiver
      channel_->receiver_queue_.push({handle, this});
      return std::noop_coroutine();
    }

    T await_resume() noexcept { return std::move(value_); }
  };

  Channel() = default;
  explicit Channel(size_t capacity) : capacity_(capacity) {
    buffer_.resize(capacity);
  }

  ~Channel() = default;

  bool isEmpty() noexcept { return header_ == tail_; }
  bool isFull() noexcept { return (tail_ + 1) % capacity_ == header_; }

  SenderAwaiter send(T value) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (close_) {
      throw std::runtime_error("channel is closed");
    }
    return SenderAwaiter{this, std::move(value)};
  }

  ReceiverAwaiter receive() noexcept { return ReceiverAwaiter{this}; }

  void close() {
    std::unique_lock<std::mutex> lock(mtx_);
    if (close_) {
      throw std::runtime_error("channel is already closed");
    }
    close_ = true;

    // 唤醒所有等待的 receiver
    std::vector<std::coroutine_handle<>> handles;
    while (!receiver_queue_.empty()) {
      handles.push_back(receiver_queue_.front().first);
      receiver_queue_.pop();
    }
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
  bool close_{false};
  std::mutex mtx_;
  std::queue<std::pair<std::coroutine_handle<>, SenderAwaiter *>> sender_queue_;
  std::queue<std::pair<std::coroutine_handle<>, ReceiverAwaiter *>>
      receiver_queue_;
};

} // namespace tiny_coroutine
