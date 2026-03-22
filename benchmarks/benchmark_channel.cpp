#include "channel.h"
#include "scheduler.h"
#include "task.h"

#include <benchmark/benchmark.h>

#include <atomic>
#include <cstdint>
#include <thread>

using namespace tiny_coroutine;

static void BM_ChannelTrySendTryReceive(benchmark::State &state) {
  const int capacity = static_cast<int>(state.range(0));
  Channel<int> ch(static_cast<size_t>(capacity));
  std::uint64_t sum = 0;

  for (auto _ : state) {
    if (!ch.try_send(1)) {
      state.SkipWithError("try_send failed unexpectedly");
      break;
    }

    auto v = ch.try_receive();
    if (!v.has_value()) {
      state.SkipWithError("try_receive returned empty unexpectedly");
      break;
    }

    sum += static_cast<std::uint64_t>(*v);
    benchmark::DoNotOptimize(sum);
  }

  state.SetItemsProcessed(state.iterations());
  state.SetLabel("single-thread try_send/try_receive");
}

BENCHMARK(BM_ChannelTrySendTryReceive)->Arg(1)->Arg(64)->Arg(1024);

static Task<void> producer(Channel<int> &ch, int count,
                           std::atomic<int> &produced) {
  for (int i = 0; i < count; i++) {
    co_await ch.send(i);
    produced.fetch_add(1, std::memory_order_relaxed);
  }
  co_return;
}

static Task<void> consumer(Channel<int> &ch, int count,
                           std::atomic<int> &consumed,
                           std::atomic<std::uint64_t> &sum) {
  for (int i = 0; i < count; i++) {
    auto v = co_await ch.receive();
    sum.fetch_add(static_cast<std::uint64_t>(v), std::memory_order_relaxed);
    consumed.fetch_add(1, std::memory_order_relaxed);
  }
  co_return;
}

static void BM_ChannelCoroutineMPMC(benchmark::State &state) {
  const int pairs = static_cast<int>(state.range(0));
  const int messages_per_producer = static_cast<int>(state.range(1));
  const int total_messages = pairs * messages_per_producer;

  for (auto _ : state) {
    Scheduler scheduler(static_cast<size_t>(pairs * 2));
    Channel<int> ch(1024);
    std::atomic<int> produced{0};
    std::atomic<int> consumed{0};
    std::atomic<std::uint64_t> sum{0};

    for (int i = 0; i < pairs; i++) {
      scheduler.spawn(producer(ch, messages_per_producer, produced));
      scheduler.spawn(consumer(ch, messages_per_producer, consumed, sum));
    }

    while (consumed.load(std::memory_order_acquire) < total_messages) {
      std::this_thread::yield();
    }

    benchmark::DoNotOptimize(produced.load(std::memory_order_acquire));
    benchmark::DoNotOptimize(sum.load(std::memory_order_acquire));
  }

  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) *
                          static_cast<int64_t>(total_messages));
  state.SetLabel("Scheduler + co_await send/receive");
}

BENCHMARK(BM_ChannelCoroutineMPMC)
    ->Args({1, 10000})
    ->Args({2, 10000})
    ->Args({4, 10000});
