#include "async_mutex.h"
#include "channel.h"
#include "scheduler.h"
#include "task.h"
#include "wait_group.h"
#include "when_all.h"
#include <benchmark/benchmark.h>

#include <atomic>
#include <vector>

using namespace tiny_coroutine;

// ──────────────────────────────────────────────
// Scheduler: spawn 吞吐量
// ──────────────────────────────────────────────

// 测量 spawn N 个无操作任务并等待全部完成的吞吐
static void BM_SpawnThroughput(benchmark::State &state) {
  const int n = state.range(0);
  Scheduler scheduler;
  for (auto _ : state) {
    std::atomic<int> done{0};
    for (int i = 0; i < n; i++) {
      scheduler.spawn([&]() -> Task<void> {
        done.fetch_add(1, std::memory_order_relaxed);
        co_return;
      }());
    }
    while (done.load(std::memory_order_relaxed) < n) {
      std::this_thread::yield();
    }
  }
  state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(BM_SpawnThroughput)->Arg(100)->Arg(1000)->Arg(10000)->UseRealTime();

// ──────────────────────────────────────────────
// Scheduler: co_await schedule() yield 延迟
// ──────────────────────────────────────────────

// 测量单个任务做一次 yield 再恢复的往返延迟
static void BM_YieldLatency(benchmark::State &state) {
  Scheduler scheduler(1);
  for (auto _ : state) {
    std::atomic<bool> done{false};
    scheduler.spawn([&]() -> Task<void> {
      co_await scheduler.schedule();
      done.store(true, std::memory_order_release);
      co_return;
    }());
    while (!done.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
  }
}
BENCHMARK(BM_YieldLatency)->UseRealTime();

// ──────────────────────────────────────────────
// Scheduler: N 层链式 co_await
// ──────────────────────────────────────────────

static Task<int> chain(int depth) {
  if (depth <= 0)
    co_return 0;
  int v = co_await chain(depth - 1);
  co_return v + 1;
}

static void BM_ChainedTasks(benchmark::State &state) {
  const int depth = state.range(0);
  for (auto _ : state) {
    auto t = chain(depth);
    t.resume();
    benchmark::DoNotOptimize(t.get());
  }
}
BENCHMARK(BM_ChainedTasks)->Arg(10)->Arg(100)->Arg(1000);

// ──────────────────────────────────────────────
// AsyncMutex: 竞争锁的吞吐
// ──────────────────────────────────────────────

// N 个协程竞争同一把 AsyncMutex，每人加锁/解锁一次
static void BM_AsyncMutexContention(benchmark::State &state) {
  const int n = state.range(0);
  Scheduler scheduler;
  for (auto _ : state) {
    AsyncMutex mtx;
    std::atomic<int> done{0};
    for (int i = 0; i < n; i++) {
      scheduler.spawn([&]() -> Task<void> {
        auto guard = co_await mtx.lock();
        done.fetch_add(1, std::memory_order_relaxed);
        co_return;
      }());
    }
    while (done.load(std::memory_order_relaxed) < n) {
      std::this_thread::yield();
    }
  }
  state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(BM_AsyncMutexContention)->Arg(100)->Arg(1000)->UseRealTime();

// ──────────────────────────────────────────────
// Channel: ping-pong 往返延迟
// ──────────────────────────────────────────────

// 单对 sender/receiver，每次迭代发送并接收一个值
static void BM_ChannelPingPong(benchmark::State &state) {
  Scheduler scheduler(2);
  Channel<int> ch(1);
  std::atomic<bool> ready{false};

  // 持续 receiver
  std::atomic<bool> stop{false};
  std::atomic<long long> received{0};
  scheduler.spawn([&]() -> Task<void> {
    while (!stop.load(std::memory_order_relaxed)) {
      co_await scheduler.schedule();
      if (stop.load(std::memory_order_relaxed))
        co_return;
      int v = co_await ch.receive();
      benchmark::DoNotOptimize(v);
      received.fetch_add(1, std::memory_order_relaxed);
    }
    co_return;
  }());

  for (auto _ : state) {
    long long before = received.load(std::memory_order_relaxed);
  co_await_sync : {
    // 用普通方式 spawn 一个 send 任务
    std::atomic<bool> sent{false};
    scheduler.spawn([&]() -> Task<void> {
      co_await ch.send(42);
      sent.store(true, std::memory_order_release);
      co_return;
    }());
    while (!sent.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    while (received.load(std::memory_order_relaxed) <= before) {
      std::this_thread::yield();
    }
  }
  }

  stop.store(true, std::memory_order_relaxed);
  // 送一个值唤醒 receiver 让它退出
  scheduler.spawn([&]() -> Task<void> {
    co_await ch.send(0);
    co_return;
  }());
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
}
BENCHMARK(BM_ChannelPingPong)->UseRealTime();

// ──────────────────────────────────────────────
// Channel: 多 sender 吞吐
// ──────────────────────────────────────────────

// P 个 sender 各发 M 条消息，1 个 receiver 消费全部
static void BM_ChannelThroughput(benchmark::State &state) {
  const int senders = state.range(0);
  const int msgs_per_sender = state.range(1);
  const int total = senders * msgs_per_sender;

  Scheduler scheduler;
  for (auto _ : state) {
    Channel<int> ch(64);
    std::atomic<int> received{0};

    // receiver
    scheduler.spawn([&]() -> Task<void> {
      while (received.load(std::memory_order_relaxed) < total) {
        int v = co_await ch.receive();
        benchmark::DoNotOptimize(v);
        received.fetch_add(1, std::memory_order_relaxed);
      }
      co_return;
    }());

    // senders
    std::atomic<int> senders_done{0};
    for (int s = 0; s < senders; s++) {
      scheduler.spawn([&, s]() -> Task<void> {
        for (int i = 0; i < msgs_per_sender; i++) {
          co_await ch.send(s * msgs_per_sender + i);
        }
        senders_done.fetch_add(1, std::memory_order_relaxed);
        co_return;
      }());
    }

    while (received.load(std::memory_order_relaxed) < total) {
      std::this_thread::yield();
    }
  }
  state.SetItemsProcessed(state.iterations() * total);
}
BENCHMARK(BM_ChannelThroughput)
    ->Args({1, 1000})
    ->Args({4, 250})
    ->Args({8, 125})
    ->UseRealTime();

// ──────────────────────────────────────────────
// WaitGroup: add/done/wait 吞吐
// ──────────────────────────────────────────────

// N 个 worker 协程各调用一次 done()，主协程等待全部完成
static void BM_WaitGroup(benchmark::State &state) {
  const int n = state.range(0);
  Scheduler scheduler;
  for (auto _ : state) {
    WaitGroup wg;
    wg.add(n);
    std::atomic<bool> wait_spawned{false};

    // worker tasks
    for (int i = 0; i < n; i++) {
      scheduler.spawn([&]() -> Task<void> {
        wg.done();
        co_return;
      }());
    }

    // waiter
    std::atomic<bool> done{false};
    scheduler.spawn([&]() -> Task<void> {
      co_await wg.wait();
      done.store(true, std::memory_order_release);
      co_return;
    }());

    while (!done.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
  }
  state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(BM_WaitGroup)->Arg(10)->Arg(100)->Arg(1000)->UseRealTime();

// ──────────────────────────────────────────────
// when_all: 并行 N 个任务
// ──────────────────────────────────────────────

static void BM_WhenAll(benchmark::State &state) {
  const int n = state.range(0);
  Scheduler scheduler;
  for (auto _ : state) {
    std::atomic<bool> done{false};
    scheduler.spawn([&]() -> Task<void> {
      std::vector<Task<int>> tasks;
      tasks.reserve(n);
      for (int i = 0; i < n; i++) {
        tasks.push_back([](int v) -> Task<int> { co_return v; }(i));
      }
      auto results = co_await when_all(scheduler, std::move(tasks));
      benchmark::DoNotOptimize(results);
      done.store(true, std::memory_order_release);
      co_return;
    }());
    while (!done.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
  }
  state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(BM_WhenAll)->Arg(10)->Arg(100)->Arg(1000)->UseRealTime();
