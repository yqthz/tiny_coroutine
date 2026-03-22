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

  ch.reset_stats();
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

  const auto stats = ch.stats_snapshot();
  const double iters = static_cast<double>(state.iterations());

  state.SetItemsProcessed(state.iterations());
  state.SetLabel("single-thread try_send/try_receive");
  state.counters["chan.buffer_push/iter"] =
      benchmark::Counter(static_cast<double>(stats.buffer_pushes) / iters);
  state.counters["chan.buffer_pop/iter"] =
      benchmark::Counter(static_cast<double>(stats.buffer_pops) / iters);
  state.counters["chan.handoff/iter"] =
      benchmark::Counter(static_cast<double>(stats.direct_handoffs) / iters);
  state.counters["chan.try_send_full/iter"] =
      benchmark::Counter(static_cast<double>(stats.try_send_full_failures) /
                         iters);
  state.counters["chan.try_recv_empty/iter"] =
      benchmark::Counter(static_cast<double>(stats.try_receive_empty_returns) /
                         iters);
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

  std::uint64_t acc_enqueued = 0;
  std::uint64_t acc_dequeued = 0;
  std::uint64_t acc_resumed = 0;
  std::uint64_t acc_notify_calls = 0;
  std::uint64_t acc_worker_waits = 0;
  std::uint64_t acc_worker_wakeups = 0;

  std::uint64_t acc_buffer_pushes = 0;
  std::uint64_t acc_buffer_pops = 0;
  std::uint64_t acc_direct_handoffs = 0;
  std::uint64_t acc_sender_waits = 0;
  std::uint64_t acc_receiver_waits = 0;
  std::uint64_t acc_try_send_full_failures = 0;
  std::uint64_t acc_try_receive_empty_returns = 0;

  for (auto _ : state) {
    Scheduler scheduler(static_cast<size_t>(pairs * 2));
    Channel<int> ch(1024);
    std::atomic<int> produced{0};
    std::atomic<int> consumed{0};
    std::atomic<std::uint64_t> sum{0};

    scheduler.reset_stats();
    ch.reset_stats();

    for (int i = 0; i < pairs; i++) {
      scheduler.spawn(producer(ch, messages_per_producer, produced));
      scheduler.spawn(consumer(ch, messages_per_producer, consumed, sum));
    }

    while (consumed.load(std::memory_order_acquire) < total_messages) {
      std::this_thread::yield();
    }

    const auto sched_stats = scheduler.stats_snapshot();
    const auto chan_stats = ch.stats_snapshot();

    acc_enqueued += sched_stats.enqueued;
    acc_dequeued += sched_stats.dequeued;
    acc_resumed += sched_stats.resumed;
    acc_notify_calls += sched_stats.notify_calls;
    acc_worker_waits += sched_stats.worker_waits;
    acc_worker_wakeups += sched_stats.worker_wakeups;

    acc_buffer_pushes += chan_stats.buffer_pushes;
    acc_buffer_pops += chan_stats.buffer_pops;
    acc_direct_handoffs += chan_stats.direct_handoffs;
    acc_sender_waits += chan_stats.sender_waits;
    acc_receiver_waits += chan_stats.receiver_waits;
    acc_try_send_full_failures += chan_stats.try_send_full_failures;
    acc_try_receive_empty_returns += chan_stats.try_receive_empty_returns;

    benchmark::DoNotOptimize(produced.load(std::memory_order_acquire));
    benchmark::DoNotOptimize(sum.load(std::memory_order_acquire));
  }

  const double iters = static_cast<double>(state.iterations());
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) *
                          static_cast<int64_t>(total_messages));
  state.SetLabel("Scheduler + co_await send/receive");
  state.counters["sched.enqueued/iter"] =
      benchmark::Counter(static_cast<double>(acc_enqueued) / iters);
  state.counters["sched.dequeued/iter"] =
      benchmark::Counter(static_cast<double>(acc_dequeued) / iters);
  state.counters["sched.resumed/iter"] =
      benchmark::Counter(static_cast<double>(acc_resumed) / iters);
  state.counters["sched.notify/iter"] =
      benchmark::Counter(static_cast<double>(acc_notify_calls) / iters);
  state.counters["sched.waits/iter"] =
      benchmark::Counter(static_cast<double>(acc_worker_waits) / iters);
  state.counters["sched.wakeups/iter"] =
      benchmark::Counter(static_cast<double>(acc_worker_wakeups) / iters);

  state.counters["chan.buffer_push/iter"] =
      benchmark::Counter(static_cast<double>(acc_buffer_pushes) / iters);
  state.counters["chan.buffer_pop/iter"] =
      benchmark::Counter(static_cast<double>(acc_buffer_pops) / iters);
  state.counters["chan.handoff/iter"] =
      benchmark::Counter(static_cast<double>(acc_direct_handoffs) / iters);
  state.counters["chan.sender_wait/iter"] =
      benchmark::Counter(static_cast<double>(acc_sender_waits) / iters);
  state.counters["chan.receiver_wait/iter"] =
      benchmark::Counter(static_cast<double>(acc_receiver_waits) / iters);
  state.counters["chan.try_send_full/iter"] =
      benchmark::Counter(static_cast<double>(acc_try_send_full_failures) /
                         iters);
  state.counters["chan.try_recv_empty/iter"] =
      benchmark::Counter(static_cast<double>(acc_try_receive_empty_returns) /
                         iters);
}

BENCHMARK(BM_ChannelCoroutineMPMC)
    ->Args({1, 10000})
    ->Args({2, 10000})
    ->Args({4, 10000});

static void BM_SchedulerSpawnDrain(benchmark::State &state) {
  const int workers = static_cast<int>(state.range(0));
  const int tasks = static_cast<int>(state.range(1));

  std::uint64_t acc_enqueued = 0;
  std::uint64_t acc_dequeued = 0;
  std::uint64_t acc_resumed = 0;
  std::uint64_t acc_notify_calls = 0;
  std::uint64_t acc_worker_waits = 0;
  std::uint64_t acc_worker_wakeups = 0;

  for (auto _ : state) {
    Scheduler scheduler(static_cast<size_t>(workers));
    std::atomic<int> done{0};

    auto make_task = [&done]() -> Task<void> {
      done.fetch_add(1, std::memory_order_relaxed);
      co_return;
    };

    scheduler.reset_stats();

    for (int i = 0; i < tasks; ++i) {
      scheduler.spawn(make_task());
    }

    while (done.load(std::memory_order_acquire) < tasks) {
      std::this_thread::yield();
    }

    const auto stats = scheduler.stats_snapshot();
    acc_enqueued += stats.enqueued;
    acc_dequeued += stats.dequeued;
    acc_resumed += stats.resumed;
    acc_notify_calls += stats.notify_calls;
    acc_worker_waits += stats.worker_waits;
    acc_worker_wakeups += stats.worker_wakeups;
  }

  const double iters = static_cast<double>(state.iterations());
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * tasks);
  state.SetLabel("Scheduler spawn/drain observability");
  state.counters["sched.enqueued/iter"] =
      benchmark::Counter(static_cast<double>(acc_enqueued) / iters);
  state.counters["sched.dequeued/iter"] =
      benchmark::Counter(static_cast<double>(acc_dequeued) / iters);
  state.counters["sched.resumed/iter"] =
      benchmark::Counter(static_cast<double>(acc_resumed) / iters);
  state.counters["sched.notify/iter"] =
      benchmark::Counter(static_cast<double>(acc_notify_calls) / iters);
  state.counters["sched.waits/iter"] =
      benchmark::Counter(static_cast<double>(acc_worker_waits) / iters);
  state.counters["sched.wakeups/iter"] =
      benchmark::Counter(static_cast<double>(acc_worker_wakeups) / iters);
}

BENCHMARK(BM_SchedulerSpawnDrain)
    ->Args({1, 10000})
    ->Args({2, 10000})
    ->Args({4, 10000});
