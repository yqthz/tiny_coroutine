#pragma once

#include <atomic>
#include <coroutine>
#include <cstdint>
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

  struct StatsSnapshot {
    std::uint64_t buffer_pushes{0};
    std::uint64_t buffer_pops{0};
    std::uint64_t direct_handoffs{0};
    std::uint64_t sender_waits{0};
    std::uint64_t receiver_waits{0};
    std::uint64_t try_send_full_failures{0};
    std::uint64_t try_receive_empty_returns{0};
    std::uint64_t closed_failures{0};
    std::uint64_t close_calls{0};
    std::uint64_t close_wake_senders{0};
    std::uint64_t close_wake_receivers{0};
  };

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
          channel_->stats_closed_failures_.fetch_add(1,
                                                     std::memory_order_relaxed);
        } else if (!channel_->receiver_queue_.empty()) {
          // Direct handoff to a waiting receiver; sender should continue.
          auto [receiver_handle, receiver_awaiter] =
              channel_->receiver_queue_.front();
          channel_->receiver_queue_.pop();
          receiver_awaiter->value_ = std::move(value_);
          resume_receiver = receiver_handle;
          channel_->stats_direct_handoffs_.fetch_add(
              1, std::memory_order_relaxed);
        } else if (!channel_->isFull()) {
          // Buffered send completes immediately.
          channel_->buffer_[channel_->tail_] = std::move(value_);
          channel_->tail_ = (channel_->tail_ + 1) % channel_->capacity_;
          channel_->count_++;
          channel_->stats_buffer_pushes_.fetch_add(1,
                                                   std::memory_order_relaxed);
        } else {
          // Buffer full: park sender until receiver makes progress.
          channel_->sender_queue_.push({handle, this});
          suspend_sender = true;
          channel_->stats_sender_waits_.fetch_add(1, std::memory_order_relaxed);
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
          channel_->stats_buffer_pops_.fetch_add(1, std::memory_order_relaxed);

          // 唤醒一个等待的 sender，让它写入 buffer
          if (!channel_->sender_queue_.empty()) {
            auto [sender_handle, sender_awaiter] =
                channel_->sender_queue_.front();
            channel_->sender_queue_.pop();
            channel_->buffer_[channel_->tail_] = std::move(sender_awaiter->value_);
            channel_->tail_ = (channel_->tail_ + 1) % channel_->capacity_;
            channel_->count_++;
            resume_sender = sender_handle;
            channel_->stats_buffer_pushes_.fetch_add(
                1, std::memory_order_relaxed);
          }
        } else if (channel_->close_) {
          value_ = std::nullopt;
          channel_->stats_closed_failures_.fetch_add(1,
                                                     std::memory_order_relaxed);
        } else if (!channel_->sender_queue_.empty()) {
          // buffer 空，有等待的 sender，直接取数据
          auto [sender_handle, sender_awaiter] = channel_->sender_queue_.front();
          channel_->sender_queue_.pop();
          value_ = std::move(sender_awaiter->value_);
          resume_sender = sender_handle;
          channel_->stats_direct_handoffs_.fetch_add(
              1, std::memory_order_relaxed);
        } else {
          // 无数据，挂起 receiver
          channel_->receiver_queue_.push({handle, this});
          suspend_receiver = true;
          channel_->stats_receiver_waits_.fetch_add(1,
                                                    std::memory_order_relaxed);
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
  explicit Channel(size_t capacity) : capacity_(capacity) { buffer_.resize(capacity); }

  ~Channel() = default;

  bool isEmpty() noexcept { return count_ == 0; }
  bool isFull() noexcept { return count_ == capacity_; }

  // Fast-fail for sends after close; await_suspend() rechecks under lock.
  template <typename U>
    requires std::is_constructible_v<T, U &&>
  SenderAwaiter send(U &&value) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (close_) {
      stats_closed_failures_.fetch_add(1, std::memory_order_relaxed);
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
        stats_closed_failures_.fetch_add(1, std::memory_order_relaxed);
        throw std::runtime_error("channel is closed");
      }

      if (!receiver_queue_.empty()) {
        auto [receiver_handle, receiver_awaiter] = receiver_queue_.front();
        receiver_queue_.pop();
        receiver_awaiter->value_.emplace(std::forward<U>(value));
        resume_receiver = receiver_handle;
        stats_direct_handoffs_.fetch_add(1, std::memory_order_relaxed);
      } else if (!isFull()) {
        buffer_[tail_].emplace(std::forward<U>(value));
        tail_ = (tail_ + 1) % capacity_;
        count_++;
        stats_buffer_pushes_.fetch_add(1, std::memory_order_relaxed);
      } else {
        stats_try_send_full_failures_.fetch_add(1, std::memory_order_relaxed);
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
        stats_closed_failures_.fetch_add(1, std::memory_order_relaxed);
        throw std::runtime_error("channel is closed");
      }

      resume_receivers.reserve(receiver_queue_.size());

      for (; first != last; ++first) {
        auto &&item = *first;
        if (receiver_queue_.empty() == false) {
          auto [receiver_handle, receiver_awaiter] = receiver_queue_.front();
          receiver_queue_.pop();
          receiver_awaiter->value_.emplace(std::forward<decltype(item)>(item));
          resume_receivers.push_back(receiver_handle);
          stats_direct_handoffs_.fetch_add(1, std::memory_order_relaxed);
        } else if (isFull() == false) {
          buffer_[tail_].emplace(std::forward<decltype(item)>(item));
          tail_ = (tail_ + 1) % capacity_;
          count_++;
          stats_buffer_pushes_.fetch_add(1, std::memory_order_relaxed);
        } else {
          stats_try_send_full_failures_.fetch_add(1, std::memory_order_relaxed);
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
        stats_buffer_pops_.fetch_add(1, std::memory_order_relaxed);

        if (!sender_queue_.empty()) {
          auto [sender_handle, sender_awaiter] = sender_queue_.front();
          sender_queue_.pop();
          buffer_[tail_] = std::move(sender_awaiter->value_);
          tail_ = (tail_ + 1) % capacity_;
          count_++;
          resume_sender = sender_handle;
          stats_buffer_pushes_.fetch_add(1, std::memory_order_relaxed);
        }
      } else if (!sender_queue_.empty()) {
        auto [sender_handle, sender_awaiter] = sender_queue_.front();
        sender_queue_.pop();
        value = std::move(sender_awaiter->value_);
        resume_sender = sender_handle;
        stats_direct_handoffs_.fetch_add(1, std::memory_order_relaxed);
      } else {
        if (close_) {
          stats_closed_failures_.fetch_add(1, std::memory_order_relaxed);
          throw std::runtime_error("channel is closed");
        }
        stats_try_receive_empty_returns_.fetch_add(1, std::memory_order_relaxed);
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
    bool closed_and_empty = false;

    {
      std::lock_guard<std::mutex> lock(mtx_);
      const size_t waiting_senders = sender_queue_.size();
      resume_senders.reserve(waiting_senders < max_items ? waiting_senders
                                                         : max_items);

      for (; values.size() < max_items;) {
        if (isEmpty() == false) {
          values.emplace_back(std::move(*buffer_[header_]));
          buffer_[header_].reset();
          header_ = (header_ + 1) % capacity_;
          count_--;
          stats_buffer_pops_.fetch_add(1, std::memory_order_relaxed);

          if (sender_queue_.empty() == false) {
            auto [sender_handle, sender_awaiter] = sender_queue_.front();
            sender_queue_.pop();
            buffer_[tail_] = std::move(sender_awaiter->value_);
            tail_ = (tail_ + 1) % capacity_;
            count_++;
            resume_senders.push_back(sender_handle);
            stats_buffer_pushes_.fetch_add(1, std::memory_order_relaxed);
          }
        } else if (sender_queue_.empty() == false) {
          auto [sender_handle, sender_awaiter] = sender_queue_.front();
          sender_queue_.pop();
          values.emplace_back(std::move(sender_awaiter->value_));
          resume_senders.push_back(sender_handle);
          stats_direct_handoffs_.fetch_add(1, std::memory_order_relaxed);
        } else {
          break;
        }
      }

      if (values.empty() && close_) {
        closed_and_empty = true;
        stats_closed_failures_.fetch_add(1, std::memory_order_relaxed);
      } else if (values.empty()) {
        stats_try_receive_empty_returns_.fetch_add(1, std::memory_order_relaxed);
      }
    }

    if (closed_and_empty) {
      throw std::runtime_error("channel is closed");
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
    stats_close_calls_.fetch_add(1, std::memory_order_relaxed);

    std::vector<std::coroutine_handle<>> sender_handles;
    std::vector<std::coroutine_handle<>> receiver_handles;
    sender_handles.reserve(sender_queue_.size());
    receiver_handles.reserve(receiver_queue_.size());

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
    stats_close_wake_senders_.fetch_add(
        static_cast<std::uint64_t>(sender_handles.size()),
        std::memory_order_relaxed);
    stats_close_wake_receivers_.fetch_add(
        static_cast<std::uint64_t>(receiver_handles.size()),
        std::memory_order_relaxed);
    lock.unlock();

    for (auto handle : sender_handles) {
      handle.resume();
    }
    for (auto handle : receiver_handles) {
      handle.resume();
    }
  }

  StatsSnapshot stats_snapshot() const noexcept {
    return StatsSnapshot{
        .buffer_pushes = stats_buffer_pushes_.load(std::memory_order_relaxed),
        .buffer_pops = stats_buffer_pops_.load(std::memory_order_relaxed),
        .direct_handoffs =
            stats_direct_handoffs_.load(std::memory_order_relaxed),
        .sender_waits = stats_sender_waits_.load(std::memory_order_relaxed),
        .receiver_waits = stats_receiver_waits_.load(std::memory_order_relaxed),
        .try_send_full_failures =
            stats_try_send_full_failures_.load(std::memory_order_relaxed),
        .try_receive_empty_returns =
            stats_try_receive_empty_returns_.load(std::memory_order_relaxed),
        .closed_failures = stats_closed_failures_.load(std::memory_order_relaxed),
        .close_calls = stats_close_calls_.load(std::memory_order_relaxed),
        .close_wake_senders =
            stats_close_wake_senders_.load(std::memory_order_relaxed),
        .close_wake_receivers =
            stats_close_wake_receivers_.load(std::memory_order_relaxed),
    };
  }

  void reset_stats() noexcept {
    stats_buffer_pushes_.store(0, std::memory_order_relaxed);
    stats_buffer_pops_.store(0, std::memory_order_relaxed);
    stats_direct_handoffs_.store(0, std::memory_order_relaxed);
    stats_sender_waits_.store(0, std::memory_order_relaxed);
    stats_receiver_waits_.store(0, std::memory_order_relaxed);
    stats_try_send_full_failures_.store(0, std::memory_order_relaxed);
    stats_try_receive_empty_returns_.store(0, std::memory_order_relaxed);
    stats_closed_failures_.store(0, std::memory_order_relaxed);
    stats_close_calls_.store(0, std::memory_order_relaxed);
    stats_close_wake_senders_.store(0, std::memory_order_relaxed);
    stats_close_wake_receivers_.store(0, std::memory_order_relaxed);
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

  std::atomic<std::uint64_t> stats_buffer_pushes_{0};
  std::atomic<std::uint64_t> stats_buffer_pops_{0};
  std::atomic<std::uint64_t> stats_direct_handoffs_{0};
  std::atomic<std::uint64_t> stats_sender_waits_{0};
  std::atomic<std::uint64_t> stats_receiver_waits_{0};
  std::atomic<std::uint64_t> stats_try_send_full_failures_{0};
  std::atomic<std::uint64_t> stats_try_receive_empty_returns_{0};
  std::atomic<std::uint64_t> stats_closed_failures_{0};
  std::atomic<std::uint64_t> stats_close_calls_{0};
  std::atomic<std::uint64_t> stats_close_wake_senders_{0};
  std::atomic<std::uint64_t> stats_close_wake_receivers_{0};
};

} // namespace tiny_coroutine
