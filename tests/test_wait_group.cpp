#include <gtest/gtest.h>
#include "scheduler.h"
#include "task.h"
#include "wait_group.h"

#include <atomic>
#include <chrono>
#include <thread>

using namespace tiny_coroutine;

// add(0) 后 wait() 应立即完成
TEST(WaitGroupTest, ZeroCount) {
  WaitGroup wg;
  wg.add(0);

  auto t = [&]() -> Task<void> {
    co_await wg.wait();
    co_return;
  }();

  t.resume();
  EXPECT_TRUE(t.done());
}

// done() 过多时抛出异常
TEST(WaitGroupTest, DoneTooManyThrows) {
  WaitGroup wg;
  wg.add(1);
  wg.done();
  EXPECT_THROW(wg.done(), std::runtime_error);
}

// N 个任务各调用一次 done()，waiter 恰好在全部完成后继续
TEST(WaitGroupTest, WaitForNTasks) {
  Scheduler scheduler(4);
  WaitGroup wg;
  const int N = 5;
  std::atomic<int> finished{0};
  std::atomic<bool> waiter_resumed{false};

  wg.add(N);

  auto worker = [&]() -> Task<void> {
    finished.fetch_add(1);
    wg.done();
    co_return;
  };

  auto waiter = [&]() -> Task<void> {
    co_await wg.wait();
    waiter_resumed.store(true);
    co_return;
  };

  scheduler.spawn(waiter());
  for (int i = 0; i < N; i++) {
    scheduler.spawn(worker());
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_EQ(finished.load(), N);
  EXPECT_TRUE(waiter_resumed.load());
}

// 多个 waiter 都能被唤醒
TEST(WaitGroupTest, MultipleWaiters) {
  Scheduler scheduler(4);
  WaitGroup wg;
  wg.add(1);
  std::atomic<int> resumed{0};

  auto waiter = [&]() -> Task<void> {
    co_await wg.wait();
    resumed.fetch_add(1);
    co_return;
  };

  scheduler.spawn(waiter());
  scheduler.spawn(waiter());
  scheduler.spawn(waiter());

  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  wg.done();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(resumed.load(), 3);
}
