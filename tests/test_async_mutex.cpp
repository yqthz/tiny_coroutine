#include <gtest/gtest.h>
#include "async_mutex.h"
#include "scheduler.h"
#include "task.h"

#include <atomic>
#include <chrono>
#include <thread>

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
  Scheduler scheduler(4);
  AsyncMutex mtx;
  int counter = 0;
  const int N = 100;

  auto task = [&]() -> Task<void> {
    auto guard = co_await mtx.lock();
    int tmp = counter;
    // 模拟一点工作
    counter = tmp + 1;
    co_return;
  };

  for (int i = 0; i < N; i++) {
    scheduler.spawn(task());
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_EQ(counter, N);
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
