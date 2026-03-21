#pragma once

#include <coroutine>
#include <cstdlib>
#include <exception>
#include <optional>
#include <type_traits>
#include <utility>

namespace tiny_coroutine {

template <typename T> class Task;

struct base_promise {
  std::coroutine_handle<> caller_{nullptr};
  std::exception_ptr exception_{nullptr};

  struct Awaiter {
    bool await_ready() noexcept { return false; }

    template <typename T>
    std::coroutine_handle<>
    await_suspend(std::coroutine_handle<T> callee) noexcept {
      auto caller = callee.promise().caller_;
      if (caller) {
        return caller;
      }
      return std::noop_coroutine();
    }

    void await_resume() noexcept {}
  };

  std::suspend_always initial_suspend() noexcept { return {}; }

  Awaiter final_suspend() noexcept { return {}; }

  void unhandled_exception() { exception_ = std::current_exception(); }
};

template <typename T> struct promise : public base_promise {
  Task<T> get_return_object() noexcept {
    return Task<T>{std::coroutine_handle<promise>::from_promise(*this)};
  }

  template <typename U> void return_value(U &&value) noexcept {
    value_ = std::forward<U>(value);
  }

  std::optional<T> value_;
};

template <> struct promise<void> : public base_promise {
  Task<void> get_return_object() noexcept;

  void return_void() noexcept {}
};

template <typename T> class Task {
public:
  using promise_type = promise<T>;

  Task() = default;
  explicit Task(std::coroutine_handle<promise_type> handle)
      : handler_(handle) {}
  ~Task() {
    if (handler_) {
      handler_.destroy();
    }
  }
  Task(const Task &) = delete;
  Task &operator=(const Task &) = delete;
  Task(Task &&t) noexcept : handler_(t.handler_) { t.handler_ = nullptr; }
  Task &operator=(Task &&t) noexcept {
    if (this != &t) {
      if (handler_) {
        handler_.destroy();
      }
      handler_ = t.handler_;
      t.handler_ = nullptr;
    }
    return *this;
  }

  void resume() noexcept {
    if (handler_) {
      handler_.resume();
    }
  }

  bool done() const noexcept {
    if (handler_) {
      return handler_.done();
    }
    return true;
  }

  T get() const {
    assert(handler_ && handler_.done());
    if (handler_.promise().exception_) {
      std::rethrow_exception(handler_.promise().exception_);
    }
    if constexpr (!std::is_void_v<T>) {
      return *handler_.promise().value_;
    }
  }

  std::coroutine_handle<promise_type> get_handle() const noexcept {
    return handler_;
  }

  void detach() noexcept { handler_ = nullptr; }

  bool await_ready() const noexcept { return !handler_ || handler_.done(); }

  template <typename U>
  std::coroutine_handle<>
  await_suspend(std::coroutine_handle<U> caller) noexcept {
    handler_.promise().caller_ = caller;
    return handler_;
  }

  T await_resume() {
    if (handler_.promise().exception_) {
      std::rethrow_exception(handler_.promise().exception_);
    }
    if constexpr (!std::is_void_v<T>) {
      return *handler_.promise().value_;
    }
  }

private:
  std::coroutine_handle<promise_type> handler_{nullptr};
};

inline Task<void> promise<void>::get_return_object() noexcept {
  return Task<void>{std::coroutine_handle<promise>::from_promise(*this)};
}

} // namespace tiny_coroutine