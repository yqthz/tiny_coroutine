#include "channel.h"
#include "scheduler.h"
#include "task.h"
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace tiny_coroutine;

// 单个 send/receive，buffer 容量为 1
TEST(ChannelTest, BasicSendReceive) {
  Channel<int> ch(1);
  int received = -1;

  auto sender = [&]() -> Task<void> {
    co_await ch.send(42);
    co_return;
  };

  auto receiver = [&]() -> Task<void> {
    received = co_await ch.receive();
    co_return;
  };

  auto s = sender();
  s.resume();
  EXPECT_TRUE(s.done());

  auto r = receiver();
  r.resume();
  EXPECT_TRUE(r.done());

  EXPECT_EQ(received, 42);
}

// buffer 满后 sender 挂起，receiver 消费后 sender 继续
TEST(ChannelTest, SenderBlocksWhenFull) {
  Scheduler scheduler(2);
  Channel<int> ch(1);
  std::atomic<bool> sender2_done{false};

  auto sender1 = [&]() -> Task<void> {
    co_await ch.send(1);
    co_return;
  };

  auto sender2 = [&]() -> Task<void> {
    co_await ch.send(2); // buffer 满，应被挂起
    sender2_done.store(true);
    co_return;
  };

  auto receiver = [&]() -> Task<void> {
    co_await ch.receive(); // 消费 1，唤醒 sender2
    co_await ch.receive(); // 消费 2
    co_return;
  };

  scheduler.spawn(sender1());
  scheduler.spawn(sender2());
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  scheduler.spawn(receiver());
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  EXPECT_TRUE(sender2_done.load());
}

// receiver 先于 sender 挂起，sender 到来后直接交付
TEST(ChannelTest, ReceiverBlocksWhenEmpty) {
  Scheduler scheduler(2);
  Channel<int> ch(1);
  std::atomic<int> received{-1};

  auto receiver = [&]() -> Task<void> {
    received.store(co_await ch.receive());
    co_return;
  };

  auto sender = [&]() -> Task<void> {
    co_await ch.send(99);
    co_return;
  };

  scheduler.spawn(receiver());
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  scheduler.spawn(sender());
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  EXPECT_EQ(received.load(), 99);
}

// 多生产者多消费者，所有值都被消费
TEST(ChannelTest, MPMC) {
  Scheduler scheduler(4);
  Channel<int> ch(4);
  const int N = 20;
  std::atomic<int> sum{0};

  auto producer = [&](int val) -> Task<void> {
    co_await ch.send(val);
    co_return;
  };

  auto consumer = [&]() -> Task<void> {
    int v = co_await ch.receive();
    sum.fetch_add(v);
    co_return;
  };

  for (int i = 1; i <= N; i++) {
    scheduler.spawn(producer(i));
    scheduler.spawn(consumer());
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  // 1+2+...+N = N*(N+1)/2
  EXPECT_EQ(sum.load(), N * (N + 1) / 2);
}

// close() 后再 send 抛异常
TEST(ChannelTest, SendAfterCloseThrows) {
  Channel<int> ch(1);
  ch.close();
  EXPECT_THROW(ch.send(1), std::runtime_error);
}

// close() 两次抛异常
TEST(ChannelTest, DoubleCloseThrows) {
  Channel<int> ch(1);
  ch.close();
  EXPECT_THROW(ch.close(), std::runtime_error);
}
