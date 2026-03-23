#pragma once

#include <atomic>
#include <cstddef>

namespace tiny_coroutine::runtime {

class Dispatcher {
public:
  void init(size_t context_count) noexcept {
    context_count_ = context_count == 0 ? 1 : context_count;
    next_.store(0, std::memory_order_relaxed);
  }

  size_t dispatch() noexcept {
    return next_.fetch_add(1, std::memory_order_relaxed) % context_count_;
  }

private:
  size_t context_count_{1};
  std::atomic<size_t> next_{0};
};

} // namespace tiny_coroutine::runtime
