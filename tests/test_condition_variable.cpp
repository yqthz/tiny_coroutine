#include <gtest/gtest.h>
#include "async_mutex.h"
#include "condition_variable.h"
#include "scheduler.h"
#include "task.h"

#include <atomic>
#include <chrono>
#include <thread>

using namespace tiny_coroutine;

// notify_one 唤醒单个等待者
TEST(ConditionVariableTest, NotifyOne) {
  Scheduler scheduler(2);
  AsyncMutex mtx;
  ConditionVariable cv;
  std::atomic<bool> flag{false};
  std::atomic<bool> waiter_done{false};

  auto waiter = [&]() -> Task<void> {
    auto guard = co_await mtx.lock();
    co_await cv.wait(mtx, [&] { return flag.load(); });
    waiter_done.store(true);
    co_return;
  };

  scheduler.spawn(waiter());
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  flag.store(true);
  cv.notify_one();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  EXPECT_TRUE(waiter_done.load());
}

// notify_all 唤醒所有等待者
TEST(ConditionVariableTest, NotifyAll) {
  Scheduler scheduler(4);
  AsyncMutex mtx;
  ConditionVariable cv;
  std::atomic<bool> flag{false};
  std::atomic<int> done_count{0};

  auto waiter = [&]() -> Task<void> {
    auto guard = co_await mtx.lock();
    co_await cv.wait(mtx, [&] { return flag.load(); });
    done_count.fetch_add(1);
    co_return;
  };

  scheduler.spawn(waiter());
  scheduler.spawn(waiter());
  scheduler.spawn(waiter());
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  flag.store(true);
  cv.notify_all();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  EXPECT_EQ(done_count.load(), 3);
}

// 生产者/消费者模式
TEST(ConditionVariableTest, ProducerConsumer) {
  Scheduler scheduler(2);
  AsyncMutex mtx;
  ConditionVariable cv;
  int value = 0;
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
    co_await cv.wait(mtx, [&] { return value != 0; });
    consumed.store(value);
    co_return;
  };

  scheduler.spawn(consumer());
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  scheduler.spawn(producer(123));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  EXPECT_EQ(consumed.load(), 123);
}
