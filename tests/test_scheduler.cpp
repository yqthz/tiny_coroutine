#include <gtest/gtest.h>
#include "scheduler.h"
#include "task.h"
#include "test_utils.h"

#include <atomic>
#include <thread>

using namespace tiny_coroutine;

// 基本任务能在 scheduler 上执行
TEST(SchedulerTest, SpawnAndRun) {
  Scheduler scheduler(2);
  std::atomic<int> counter{0};

  auto task = [&]() -> Task<void> {
    counter.fetch_add(1);
    co_return;
  };

  scheduler.spawn(task());
  scheduler.spawn(task());
  scheduler.spawn(task());

  ASSERT_TRUE(wait_until([&] { return counter.load() == 3; }));
  EXPECT_EQ(counter.load(), 3);
}

// co_await scheduler.schedule() 能让出并继续执行
TEST(SchedulerTest, ScheduleYield) {
  Scheduler scheduler(2);
  std::atomic<int> steps{0};

  auto task = [&]() -> Task<void> {
    steps.fetch_add(1); // step 1
    co_await scheduler.schedule();
    steps.fetch_add(1); // step 2
    co_return;
  };

  scheduler.spawn(task());
  ASSERT_TRUE(wait_until([&] { return steps.load() == 2; }));
  EXPECT_EQ(steps.load(), 2);
}

// Scheduler 析构时应先执行完队列中的任务
TEST(SchedulerTest, DestructorDrainsQueuedTasks) {
  std::atomic<int> counter{0};
  constexpr int kTasks = 100;

  {
    Scheduler scheduler(1);
    auto make_task = [&counter]() -> Task<void> {
      counter.fetch_add(1, std::memory_order_relaxed);
      co_return;
    };

    for (int i = 0; i < kTasks; ++i) {
      scheduler.spawn(make_task());
    }
  }

  EXPECT_EQ(counter.load(std::memory_order_relaxed), kTasks);
}

// Task<T> 能返回值
TEST(SchedulerTest, TaskReturnValue) {
  auto compute = []() -> Task<int> { co_return 42; };

  auto t = compute();
  t.resume();
  EXPECT_TRUE(t.done());
  EXPECT_EQ(t.get(), 42);
}

// Task<void> done() 语义
TEST(SchedulerTest, TaskVoidDone) {
  auto noop = []() -> Task<void> { co_return; };

  auto t = noop();
  EXPECT_FALSE(t.done());
  t.resume();
  EXPECT_TRUE(t.done());
}

// 嵌套 co_await
TEST(SchedulerTest, ChainedTasks) {
  auto inner = []() -> Task<int> { co_return 7; };

  auto outer = [&inner]() -> Task<int> {
    int v = co_await inner();
    co_return v * 6;
  };

  auto t = outer();
  t.resume();
  EXPECT_TRUE(t.done());
  EXPECT_EQ(t.get(), 42);
}

// 大量突发任务在单线程下也应全部执行完（覆盖批量出队路径）
TEST(SchedulerTest, BurstTasksSingleWorker) {
  constexpr int kTasks = 5000;
  std::atomic<int> counter{0};
  Scheduler scheduler(1);

  auto make_task = [&counter]() -> Task<void> {
    counter.fetch_add(1, std::memory_order_relaxed);
    co_return;
  };

  for (int i = 0; i < kTasks; ++i) {
    scheduler.spawn(make_task());
  }

  ASSERT_TRUE(wait_until(
      [&] { return counter.load(std::memory_order_relaxed) == kTasks; },
      std::chrono::milliseconds(1000)));
  EXPECT_EQ(counter.load(std::memory_order_relaxed), kTasks);
}

// thread_count=0 时应回退到 1 个 worker，避免任务无法执行
TEST(SchedulerTest, ZeroThreadCountFallsBackToOneWorker) {
  std::atomic<int> counter{0};
  Scheduler scheduler(0);

  auto task = [&counter]() -> Task<void> {
    counter.fetch_add(1, std::memory_order_relaxed);
    co_return;
  };

  scheduler.spawn(task());

  ASSERT_TRUE(wait_until(
      [&] { return counter.load(std::memory_order_relaxed) == 1; },
      std::chrono::milliseconds(500)));
  EXPECT_EQ(counter.load(std::memory_order_relaxed), 1);
}

// 统计接口：基本计数应与任务提交/执行一致
TEST(SchedulerTest, StatsSnapshotTracksBasicWorkload) {
  std::atomic<int> done{0};
  Scheduler scheduler(1);

  scheduler.reset_stats();

  auto make_task = [&done]() -> Task<void> {
    done.fetch_add(1, std::memory_order_relaxed);
    co_return;
  };

  constexpr int kTasks = 8;
  for (int i = 0; i < kTasks; ++i) {
    scheduler.spawn(make_task());
  }

  ASSERT_TRUE(wait_until(
      [&] { return done.load(std::memory_order_relaxed) == kTasks; },
      std::chrono::milliseconds(500)));

  const auto stats = scheduler.stats_snapshot();
  EXPECT_GE(stats.enqueued, static_cast<std::uint64_t>(kTasks));
  EXPECT_GE(stats.dequeued, static_cast<std::uint64_t>(kTasks));
  EXPECT_GE(stats.resumed, static_cast<std::uint64_t>(kTasks));
  EXPECT_LE(stats.notify_calls, stats.enqueued);
  EXPECT_GE(stats.worker_waits, static_cast<std::uint64_t>(1));
  EXPECT_GE(stats.worker_wakeups, static_cast<std::uint64_t>(1));
}

// worker 忙碌时批量入队，不应为每个任务都触发 notify
TEST(SchedulerTest, BusyWorkerSuppressesNotifyPerEnqueue) {
  Scheduler scheduler(1);
  std::atomic<bool> blocker_started{false};
  std::atomic<bool> unblock{false};
  std::atomic<int> done{0};

  auto blocker = [&]() -> Task<void> {
    blocker_started.store(true, std::memory_order_release);
    while (unblock.load(std::memory_order_acquire) == false) {
      std::this_thread::yield();
    }
    done.fetch_add(1, std::memory_order_relaxed);
    co_return;
  };

  scheduler.spawn(blocker());
  ASSERT_TRUE(wait_until(
      [&] { return blocker_started.load(std::memory_order_acquire); },
      std::chrono::milliseconds(500)));

  scheduler.reset_stats();

  constexpr int kTasks = 1000;
  auto make_task = [&]() -> Task<void> {
    done.fetch_add(1, std::memory_order_relaxed);
    co_return;
  };

  for (int i = 0; i < kTasks; ++i) {
    scheduler.spawn(make_task());
  }

  unblock.store(true, std::memory_order_release);

  ASSERT_TRUE(wait_until(
      [&] { return done.load(std::memory_order_relaxed) == kTasks + 1; },
      std::chrono::milliseconds(1500)));

  const auto stats = scheduler.stats_snapshot();
  EXPECT_GE(stats.enqueued, static_cast<std::uint64_t>(kTasks));
  EXPECT_EQ(stats.notify_calls, 0);
}
