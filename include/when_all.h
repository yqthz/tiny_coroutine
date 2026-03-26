#pragma once

#include "runtime/scheduler.h"
#include "task.h"
#include "wait_group.h"

#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

namespace tiny_coroutine {

namespace detail {
template <typename T> struct VectorState {
  std::vector<Task<T>> tasks;
  std::vector<std::optional<T>> res;
  std::mutex exception_mtx;
  std::exception_ptr exception;
  WaitGroup wg;
};

template <typename... Ts> struct TupleState {
  std::tuple<Task<Ts>...> tasks;
  std::tuple<std::optional<Ts>...> res;
  std::mutex exception_mtx;
  std::exception_ptr exception;
  WaitGroup wg;
};

template <typename State>
void store_first_exception(State &state, std::exception_ptr ex) {
  if (!ex) {
    return;
  }
  std::lock_guard<std::mutex> lock(state.exception_mtx);
  if (!state.exception) {
    state.exception = ex;
  }
}

template <size_t N, typename... Ts>
Task<void> wrapper(std::shared_ptr<TupleState<Ts...>> state) {
  try {
    std::get<N>(state->res).emplace(co_await std::get<N>(state->tasks));
  } catch (...) {
    store_first_exception(*state, std::current_exception());
  }
  state->wg.done();
}

template <typename... Ts, size_t... Is>
std::tuple<Ts...> unwrap_tuple_result(std::tuple<std::optional<Ts>...> &res,
                                      std::index_sequence<Is...>) {
  return std::tuple<Ts...>{std::move(*std::get<Is>(res))...};
}

template <typename... Ts, size_t... Is>
Task<std::tuple<Ts...>> when_all_impl(std::shared_ptr<TupleState<Ts...>> state,
                                      std::index_sequence<Is...>) {

  (runtime::submit_to_scheduler(wrapper<Is>(state)), ...);

  co_await state->wg.wait();

  if (state->exception) {
    std::rethrow_exception(state->exception);
  }

  co_return unwrap_tuple_result<Ts...>(state->res,
                                       std::index_sequence<Is...>{});
}
} // namespace detail

template <typename T>
Task<std::vector<T>> when_all(std::vector<Task<T>> tasks) {
  const size_t n = tasks.size();
  std::shared_ptr<detail::VectorState<T>> state =
      std::make_shared<detail::VectorState<T>>();
  state->tasks = std::move(tasks);
  state->res.resize(n);
  state->wg.add(static_cast<int>(n));

  auto wrapper = [state](size_t i) -> Task<void> {
    try {
      state->res[i].emplace(co_await state->tasks[i]);
    } catch (...) {
      detail::store_first_exception(*state, std::current_exception());
    }
    state->wg.done();
  };

  for (size_t i = 0; i < n; i++) {
    runtime::submit_to_scheduler(wrapper(i));
  }

  co_await state->wg.wait();

  if (state->exception) {
    std::rethrow_exception(state->exception);
  }

  std::vector<T> out;
  out.reserve(n);
  for (auto &v : state->res) {
    out.push_back(std::move(*v));
  }

  co_return out;
}

template <typename... Ts> Task<std::tuple<Ts...>> when_all(Task<Ts>... tasks) {
  constexpr int n = static_cast<int>(sizeof...(tasks));
  std::shared_ptr<detail::TupleState<Ts...>> state =
      std::make_shared<detail::TupleState<Ts...>>();
  state->tasks = std::make_tuple(std::move(tasks)...);
  state->wg.add(n);

  return when_all_impl(state, std::make_index_sequence<sizeof...(Ts)>{});
}
} // namespace tiny_coroutine
