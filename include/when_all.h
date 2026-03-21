#pragma once

#include "scheduler.h"
#include "task.h"
#include "wait_group.h"

#include <climits>
#include <memory>
#include <tuple>
#include <utility>
#include <vector>

namespace tiny_coroutine {

namespace detail {
template <typename T> struct VectorState {
  std::vector<Task<T>> tasks;
  std::vector<T> res;
  WaitGroup wg;
};

template <typename... Ts> struct TupleState {
  std::tuple<Task<Ts>...> tasks;
  std::tuple<Ts...> res;
  WaitGroup wg;
};

template <size_t N, typename... Ts>
Task<void> wrapper(std::shared_ptr<TupleState<Ts...>> state) {
  std::get<N>(state->res) = co_await std::get<N>(state->tasks);
  state->wg.done();
}

template <typename... Ts, size_t... Is>
Task<std::tuple<Ts...>> when_all_impl(std::shared_ptr<TupleState<Ts...>> state,
                                      Scheduler &scheduler,
                                      std::index_sequence<Is...>) {

  (scheduler.spawn(wrapper<Is>(state)), ...);

  co_await state->wg.wait();

  co_return state->res;
}
} // namespace detail

template <typename T>
Task<std::vector<T>> when_all(Scheduler &scheduler,
                              std::vector<Task<T>> tasks) {
  int n = tasks.size();
  std::shared_ptr<detail::VectorState<T>> state =
      std::make_shared<detail::VectorState<T>>();
  state->tasks = std::move(tasks);
  state->res.resize(n);
  state->wg.add(n);

  auto wrapper = [state](int i) -> Task<void> {
    state->res[i] = co_await state->tasks[i];
    state->wg.done();
  };

  for (int i = 0; i < n; i++) {
    scheduler.spawn(wrapper(i));
  }

  co_await state->wg.wait();

  co_return state->res;
}

template <typename... Ts>
Task<std::tuple<Ts...>> when_all(Scheduler &scheduler, Task<Ts>... tasks) {
  int n = sizeof...(tasks);
  std::shared_ptr<detail::TupleState<Ts...>> state =
      std::make_shared<detail::TupleState<Ts...>>();
  state->tasks = std::make_tuple(std::move(tasks)...);
  state->res = std::make_tuple(Ts{}...);
  state->wg.add(n);

  return when_all_impl(state, scheduler,
                       std::make_index_sequence<sizeof...(Ts)>{});
}
} // namespace tiny_coroutine