#include <gtest/gtest.h>
#include "async_mutex.h"
#include "runtime/scheduler.h"
#include "task.h"
#include "test_utils.h"

#include <atomic>

using namespace tiny_coroutine;

// 单个协程能正常 lock/unlock
TEST(AsyncMutexTest, BasicLockUnlock) {
  AsyncMutex mtx;
  std::atomic<int> counter{0};

  auto task = [&]() -> Task<void> {
    auto guard = co_await mtx.lock();
    counter.fetch_add(1);
    co_return;
  };

  auto t = task();
  t.resume();
  EXPECT_TRUE(t.done());
  EXPECT_EQ(counter.load(), 1);
}

// 多个协程互斥访问共享变量，结果应等于任务数
TEST(AsyncMutexTest, MutualExclusion) {
  runtime::Scheduler scheduler;
  scheduler.init(4);
  AsyncMutex mtx;
  std::atomic<int> counter{0};
  const int N = 100;

  auto task = [&]() -> Task<void> {
    auto guard = co_await mtx.lock();
    counter.fetch_add(1, std::memory_order_relaxed);
    co_return;
  };

  for (int i = 0; i < N; i++) {
    scheduler.submit(task());
  }

  ASSERT_TRUE(wait_until([&] { return counter.load(std::memory_order_relaxed) == N; }));
  scheduler.stop();
  EXPECT_EQ(counter.load(std::memory_order_relaxed), N);
}

// LockGuard 析构时自动 unlock，后续协程可以继续获取锁
TEST(AsyncMutexTest, LockGuardReleases) {
  AsyncMutex mtx;
  std::atomic<int> order{0};

  auto first = [&]() -> Task<void> {
    auto guard = co_await mtx.lock();
    order.fetch_add(1);
    co_return; // guard 析构，unlock
  };

  auto second = [&]() -> Task<void> {
    auto guard = co_await mtx.lock();
    order.fetch_add(10);
    co_return;
  };

  // 串行执行两个任务
  auto t1 = first();
  t1.resume();
  EXPECT_TRUE(t1.done());

  auto t2 = second();
  t2.resume();
  EXPECT_TRUE(t2.done());

  EXPECT_EQ(order.load(), 11);
}
