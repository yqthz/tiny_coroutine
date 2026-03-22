#pragma once

#include <atomic>
#include <coroutine>
#include <mutex>
#include <optional>
#include <queue>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace tiny_coroutine {

template <typename T> class Channel {
public:
  struct SenderAwaiter;
  struct ReceiverAwaiter;

  // Close semantics contract:
  // 1) If close() is called while senders are blocked, all blocked senders are
  //    resumed and their co_await send(...) fails with "channel is closed".
  // 2) receive() after close behaves as: drain buffered values first; once the
  //    buffer is empty, co_await receive() fails with "channel is closed".
  // 3) New receiver after close must not block forever; it is resumed
  //    immediately and fails with "channel is closed" when no buffered value
  //    remains.
  struct SenderAwaiter {
    Channel<T> *channel_{nullptr};
    T value_;
    bool close_{false};

    template <typename U>
      requires std::is_constructible_v<T, U &&>
    SenderAwaiter(Channel<T> *channel, U &&value)
        : channel_(channel), value_(std::forward<U>(value)) {}

    bool await_ready() noexcept { return false; }

    template <typename U>
    bool await_suspend(std::coroutine_handle<U> handle) {
      std::coroutine_handle<> resume_receiver;
      bool suspend_sender = false;

      {
        std::lock_guard<std::mutex> lock(channel_->mtx_);

        if (channel_->close_) {
          close_ = true;
        } else if (!channel_->receiver_queue_.empty()) {
          // Direct handoff to a waiting receiver; sender should continue.
          auto [receiver_handle, receiver_awaiter] =
              channel_->receiver_queue_.front();
          channel_->receiver_queue_.pop();
          receiver_awaiter->value_ = std::move(value_);
          resume_receiver = receiver_handle;
        } else if (!channel_->isFull()) {
          // Buffered send completes immediately.
          channel_->buffer_[channel_->tail_] = std::move(value_);
          channel_->tail_ = (channel_->tail_ + 1) % channel_->capacity_;
          channel_->count_++;
        } else {
          // Buffer full: park sender until receiver makes progress.
          channel_->sender_queue_.push({handle, this});
          suspend_sender = true;
        }
      }

      if (resume_receiver) {
        resume_receiver.resume();
      }

      return suspend_sender;
    }

    void await_resume() {
      if (close_) {
        throw std::runtime_error("channel is closed");
      }
    }
  };

  struct ReceiverAwaiter {
    Channel<T> *channel_{nullptr};
    std::optional<T> value_;

    explicit ReceiverAwaiter(Channel<T> *channel) : channel_(channel) {}

    bool await_ready() noexcept { return false; }

    template <typename U>
    bool await_suspend(std::coroutine_handle<U> handle) noexcept {
      std::coroutine_handle<> resume_sender;
      bool suspend_receiver = false;

      {
        std::lock_guard<std::mutex> lock(channel_->mtx_);

        // buffer 有数据，直接读取
        if (!channel_->isEmpty()) {
          value_ = std::move(*channel_->buffer_[channel_->header_]);
          channel_->buffer_[channel_->header_].reset();
          channel_->header_ = (channel_->header_ + 1) % channel_->capacity_;
          channel_->count_--;

          // 唤醒一个等待的 sender，让它写入 buffer
          if (!channel_->sender_queue_.empty()) {
            auto [sender_handle, sender_awaiter] =
                channel_->sender_queue_.front();
            channel_->sender_queue_.pop();
            channel_->buffer_[channel_->tail_] = std::move(sender_awaiter->value_);
            channel_->tail_ = (channel_->tail_ + 1) % channel_->capacity_;
            channel_->count_++;
            resume_sender = sender_handle;
          }
        } else if (channel_->close_) {
          value_ = std::nullopt;
        } else if (!channel_->sender_queue_.empty()) {
          // buffer 空，有等待的 sender，直接取数据
          auto [sender_handle, sender_awaiter] = channel_->sender_queue_.front();
          channel_->sender_queue_.pop();
          value_ = std::move(sender_awaiter->value_);
          resume_sender = sender_handle;
        } else {
          // 无数据，挂起 receiver
          channel_->receiver_queue_.push({handle, this});
          suspend_receiver = true;
        }
      }

      if (resume_sender) {
        resume_sender.resume();
      }

      return suspend_receiver;
    }

    T await_resume() {
      if (value_) {
        return std::move(*value_);
      }
      throw std::runtime_error("channel is closed");
    }
  };

  Channel() = default;
  explicit Channel(size_t capacity) : capacity_(capacity) {
    buffer_.resize(capacity);
  }

  ~Channel() = default;

  bool isEmpty() noexcept { return count_ == 0; }
  bool isFull() noexcept { return count_ == capacity_; }

  // Fast-fail for sends after close; await_suspend() rechecks under lock.
  template <typename U>
    requires std::is_constructible_v<T, U &&>
  SenderAwaiter send(U &&value) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (close_) {
      throw std::runtime_error("channel is closed");
    }
    return SenderAwaiter{this, std::forward<U>(value)};
  }

  // Non-blocking send; returns false only when buffer is full and no receiver
  // can take the value immediately.
  template <typename U>
    requires std::is_constructible_v<T, U &&>
  bool try_send(U &&value) {
    std::coroutine_handle<> resume_receiver;

    {
      std::lock_guard<std::mutex> lock(mtx_);
      if (close_) {
        throw std::runtime_error("channel is closed");
      }

      if (!receiver_queue_.empty()) {
        auto [receiver_handle, receiver_awaiter] = receiver_queue_.front();
        receiver_queue_.pop();
        receiver_awaiter->value_.emplace(std::forward<U>(value));
        resume_receiver = receiver_handle;
      } else if (!isFull()) {
        buffer_[tail_].emplace(std::forward<U>(value));
        tail_ = (tail_ + 1) % capacity_;
        count_++;
      } else {
        return false;
      }
    }

    if (resume_receiver) {
      resume_receiver.resume();
    }
    return true;
  }


  // Non-blocking batch send from an iterator range.
  // Stops when the channel can no longer accept more values immediately.
  template <typename InputIt>
  size_t try_send_many(InputIt first, InputIt last) {
    if (first == last) {
      return 0;
    }

    std::vector<std::coroutine_handle<>> resume_receivers;
    size_t sent = 0;

    {
      std::lock_guard<std::mutex> lock(mtx_);
      if (close_) {
        throw std::runtime_error("channel is closed");
      }

      for (; first == last ? false : true; ++first) {
        auto &&item = *first;
        if (receiver_queue_.empty() == false) {
          auto [receiver_handle, receiver_awaiter] = receiver_queue_.front();
          receiver_queue_.pop();
          receiver_awaiter->value_.emplace(std::forward<decltype(item)>(item));
          resume_receivers.push_back(receiver_handle);
        } else if (isFull() == false) {
          buffer_[tail_].emplace(std::forward<decltype(item)>(item));
          tail_ = (tail_ + 1) % capacity_;
          count_++;
        } else {
          break;
        }
        sent++;
      }
    }

    for (auto handle : resume_receivers) {
      handle.resume();
    }
    return sent;
  }

  // Receive either returns a buffered value or throws "channel is closed".
  ReceiverAwaiter receive() noexcept { return ReceiverAwaiter{this}; }

  // Non-blocking receive; returns nullopt when channel is open and empty.
  // If channel is closed and empty, throws "channel is closed".
  std::optional<T> try_receive() {
    std::optional<T> value;
    std::coroutine_handle<> resume_sender;

    {
      std::lock_guard<std::mutex> lock(mtx_);

      if (!isEmpty()) {
        value = std::move(*buffer_[header_]);
        buffer_[header_].reset();
        header_ = (header_ + 1) % capacity_;
        count_--;

        if (!sender_queue_.empty()) {
          auto [sender_handle, sender_awaiter] = sender_queue_.front();
          sender_queue_.pop();
          buffer_[tail_] = std::move(sender_awaiter->value_);
          tail_ = (tail_ + 1) % capacity_;
          count_++;
          resume_sender = sender_handle;
        }
      } else if (!sender_queue_.empty()) {
        auto [sender_handle, sender_awaiter] = sender_queue_.front();
        sender_queue_.pop();
        value = std::move(sender_awaiter->value_);
        resume_sender = sender_handle;
      } else {
        if (close_) {
          throw std::runtime_error("channel is closed");
        }
        return std::nullopt;
      }
    }

    if (resume_sender) {
      resume_sender.resume();
    }
    return value;
  }


  // Non-blocking batch receive.
  // Returns up to max_items currently available values.
  // If channel is closed and empty, throws "channel is closed".
  std::vector<T> try_receive_many(size_t max_items) {
    std::vector<T> values;
    if (max_items == 0) {
      return values;
    }

    values.reserve(max_items);
    std::vector<std::coroutine_handle<>> resume_senders;

    {
      std::lock_guard<std::mutex> lock(mtx_);

      for (; values.size() < max_items;) {
        if (isEmpty() == false) {
          values.emplace_back(std::move(*buffer_[header_]));
          buffer_[header_].reset();
          header_ = (header_ + 1) % capacity_;
          count_--;

          if (sender_queue_.empty() == false) {
            auto [sender_handle, sender_awaiter] = sender_queue_.front();
            sender_queue_.pop();
            buffer_[tail_] = std::move(sender_awaiter->value_);
            tail_ = (tail_ + 1) % capacity_;
            count_++;
            resume_senders.push_back(sender_handle);
          }
        } else if (sender_queue_.empty() == false) {
          auto [sender_handle, sender_awaiter] = sender_queue_.front();
          sender_queue_.pop();
          values.emplace_back(std::move(sender_awaiter->value_));
          resume_senders.push_back(sender_handle);
        } else {
          break;
        }
      }

      if (values.empty() && close_) {
        throw std::runtime_error("channel is closed");
      }
    }

    for (auto handle : resume_senders) {
      handle.resume();
    }
    return values;
  }

  // close() wakes all blocked senders/receivers so none stays parked forever.
  void close() {
    std::unique_lock<std::mutex> lock(mtx_);
    if (close_) {
      throw std::runtime_error("channel is already closed");
    }
    close_ = true;

    std::vector<std::coroutine_handle<>> sender_handles;
    std::vector<std::coroutine_handle<>> receiver_handles;

    while (!sender_queue_.empty()) {
      auto [sender_handle, sender_awaiter] = sender_queue_.front();
      sender_awaiter->close_ = true;
      sender_queue_.pop();
      sender_handles.push_back(sender_handle);
    }

    while (!receiver_queue_.empty()) {
      receiver_handles.push_back(receiver_queue_.front().first);
      receiver_queue_.pop();
    }
    lock.unlock();

    for (auto handle : sender_handles) {
      handle.resume();
    }
    for (auto handle : receiver_handles) {
      handle.resume();
    }
  }

private:
  std::vector<std::optional<T>> buffer_;
  size_t header_{0};
  size_t tail_{0};
  size_t capacity_{0};
  size_t count_{0};
  bool close_{false};
  std::mutex mtx_;
  std::queue<std::pair<std::coroutine_handle<>, SenderAwaiter *>> sender_queue_;
  std::queue<std::pair<std::coroutine_handle<>, ReceiverAwaiter *>>
      receiver_queue_;
};

} // namespace tiny_coroutine
