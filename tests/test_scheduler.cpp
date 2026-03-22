#include <gtest/gtest.h>
#include "scheduler.h"
#include "task.h"
#include "test_utils.h"

#include <atomic>

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
    for (int i = 0; i < kTasks; ++i) {
      scheduler.spawn([&]() -> Task<void> {
        counter.fetch_add(1, std::memory_order_relaxed);
        co_return;
      }());
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
  Scheduler scheduler(1);
  constexpr int kTasks = 5000;
  std::atomic<int> counter{0};

  for (int i = 0; i < kTasks; ++i) {
    scheduler.spawn([&]() -> Task<void> {
      counter.fetch_add(1, std::memory_order_relaxed);
      co_return;
    }());
  }

  ASSERT_TRUE(wait_until(
      [&] { return counter.load(std::memory_order_relaxed) == kTasks; },
      std::chrono::milliseconds(1000)));
  EXPECT_EQ(counter.load(std::memory_order_relaxed), kTasks);
}
