#include <gtest/gtest.h>

#include "runtime/scheduler.h"
#include "task.h"

#include <atomic>

using namespace tiny_coroutine;

TEST(RuntimeSchedulerTest, SchedulerLoopCompletesAllTasks) {
  runtime::Scheduler scheduler;
  scheduler.init(2);

  std::atomic<int> counter{0};
  constexpr int kTaskCount = 1000;

  auto make_task = [&]() -> Task<void> {
    counter.fetch_add(1, std::memory_order_relaxed);
    co_return;
  };

  for (int i = 0; i < kTaskCount; ++i) {
    scheduler.submit(make_task());
  }

  scheduler.loop();
  EXPECT_EQ(counter.load(std::memory_order_relaxed), kTaskCount);
}

TEST(RuntimeSchedulerTest, TaskCanSubmitSubTasksInsideCoroutine) {
  runtime::Scheduler scheduler;
  scheduler.init(2);

  std::atomic<int> counter{0};

  auto sub_task = [&]() -> Task<void> {
    counter.fetch_add(1, std::memory_order_relaxed);
    co_return;
  };

  auto parent_task = [&]() -> Task<void> {
    scheduler.submit(sub_task());
    scheduler.submit(sub_task());
    co_await scheduler.schedule();
    counter.fetch_add(1, std::memory_order_relaxed);
    co_return;
  };

  constexpr int kParentCount = 32;
  for (int i = 0; i < kParentCount; ++i) {
    scheduler.submit(parent_task());
  }

  scheduler.loop();

  const int expected = kParentCount * 3;
  EXPECT_EQ(counter.load(std::memory_order_relaxed), expected);
}

TEST(RuntimeSchedulerTest, SubmitToContextUsesLocalContextPath) {
  runtime::Scheduler scheduler;
  scheduler.init(2);

  std::atomic<int> counter{0};

  auto local_task = [&]() -> Task<void> {
    counter.fetch_add(1, std::memory_order_relaxed);
    co_return;
  };

  auto parent_task = [&]() -> Task<void> {
    runtime::submit_to_context(local_task());
    runtime::submit_to_context(local_task());
    co_await scheduler.schedule();
    counter.fetch_add(1, std::memory_order_relaxed);
    co_return;
  };

  constexpr int kParentCount = 64;
  for (int i = 0; i < kParentCount; ++i) {
    scheduler.submit(parent_task());
  }

  scheduler.loop();

  const int expected = kParentCount * 3;
  EXPECT_EQ(counter.load(std::memory_order_relaxed), expected);
}
