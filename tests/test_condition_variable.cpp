#include <gtest/gtest.h>
#include "async_mutex.h"
#include "condition_variable.h"
#include "scheduler.h"
#include "task.h"
#include "test_utils.h"

#include <atomic>

using namespace tiny_coroutine;

// notify_one 唤醒单个等待者
TEST(ConditionVariableTest, NotifyOne) {
  Scheduler scheduler(2);
  AsyncMutex mtx;
  ConditionVariable cv;
  std::atomic<bool> flag{false};
  std::atomic<bool> waiter_ready{false};
  std::atomic<bool> waiter_done{false};

  auto waiter = [&]() -> Task<void> {
    auto guard = co_await mtx.lock();
    waiter_ready.store(true, std::memory_order_release);
    co_await cv.wait(mtx, [&] { return flag.load(std::memory_order_acquire); });
    waiter_done.store(true, std::memory_order_release);
    co_return;
  };

  scheduler.spawn(waiter());
  ASSERT_TRUE(wait_until([&] { return waiter_ready.load(std::memory_order_acquire); }));

  flag.store(true, std::memory_order_release);
  cv.notify_one();

  ASSERT_TRUE(wait_until([&] { return waiter_done.load(std::memory_order_acquire); }));
  EXPECT_TRUE(waiter_done.load(std::memory_order_acquire));
}

// notify_all 唤醒所有等待者
TEST(ConditionVariableTest, NotifyAll) {
  Scheduler scheduler(4);
  AsyncMutex mtx;
  ConditionVariable cv;
  std::atomic<bool> flag{false};
  std::atomic<int> ready_count{0};
  std::atomic<int> done_count{0};

  auto waiter = [&]() -> Task<void> {
    auto guard = co_await mtx.lock();
    ready_count.fetch_add(1, std::memory_order_release);
    co_await cv.wait(mtx, [&] { return flag.load(std::memory_order_acquire); });
    done_count.fetch_add(1, std::memory_order_release);
    co_return;
  };

  scheduler.spawn(waiter());
  scheduler.spawn(waiter());
  scheduler.spawn(waiter());

  ASSERT_TRUE(wait_until([&] { return ready_count.load(std::memory_order_acquire) == 3; }));

  flag.store(true, std::memory_order_release);
  cv.notify_all();

  ASSERT_TRUE(wait_until([&] { return done_count.load(std::memory_order_acquire) == 3; }));
  EXPECT_EQ(done_count.load(std::memory_order_acquire), 3);
}

// 生产者/消费者模式
TEST(ConditionVariableTest, ProducerConsumer) {
  Scheduler scheduler(2);
  AsyncMutex mtx;
  ConditionVariable cv;
  int value = 0;
  std::atomic<bool> consumer_ready{false};
  std::atomic<int> consumed{0};

  auto producer = [&](int v) -> Task<void> {
    {
      auto guard = co_await mtx.lock();
      value = v;
    }
    cv.notify_one();
    co_return;
  };

  auto consumer = [&]() -> Task<void> {
    auto guard = co_await mtx.lock();
    consumer_ready.store(true, std::memory_order_release);
    co_await cv.wait(mtx, [&] { return value != 0; });
    consumed.store(value, std::memory_order_release);
    co_return;
  };

  scheduler.spawn(consumer());
  ASSERT_TRUE(wait_until([&] { return consumer_ready.load(std::memory_order_acquire); }));
  scheduler.spawn(producer(123));

  ASSERT_TRUE(wait_until([&] { return consumed.load(std::memory_order_acquire) == 123; }));
  EXPECT_EQ(consumed.load(std::memory_order_acquire), 123);
}
