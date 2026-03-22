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

// sender 已经挂起时 close()，sender 应被唤醒并失败返回
TEST(ChannelTest, CloseWakesBlockedSenderWithError) {
  Channel<int> ch(1);
  bool sender2_ok = false;
  bool sender2_failed = false;

  auto sender1 = [&]() -> Task<void> {
    co_await ch.send(1); // 填满 buffer
    co_return;
  };

  auto sender2 = [&]() -> Task<void> {
    try {
      co_await ch.send(2); // buffer 满，挂起
      sender2_ok = true;
    } catch (const std::runtime_error &) {
      sender2_failed = true;
    }
    co_return;
  };

  auto s1 = sender1();
  s1.resume();
  EXPECT_TRUE(s1.done());

  auto s2 = sender2();
  s2.resume();
  EXPECT_FALSE(s2.done());

  ch.close();

  EXPECT_TRUE(s2.done());
  EXPECT_FALSE(sender2_ok);
  EXPECT_TRUE(sender2_failed);
}

// close() 后新来的 receiver 不应永久挂起，应立即感知关闭
TEST(ChannelTest, ReceiveAfterCloseFailsImmediately) {
  Scheduler scheduler(2);
  Channel<int> ch(1);
  std::atomic<bool> receiver_ok{false};
  std::atomic<bool> receiver_failed{false};

  ch.close();

  auto receiver = [&]() -> Task<void> {
    try {
      (void)co_await ch.receive();
      receiver_ok.store(true, std::memory_order_release);
    } catch (const std::runtime_error &) {
      receiver_failed.store(true, std::memory_order_release);
    }
    co_return;
  };

  scheduler.spawn(receiver());
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  EXPECT_FALSE(receiver_ok.load(std::memory_order_acquire));
  EXPECT_TRUE(receiver_failed.load(std::memory_order_acquire));
}

// close() 后仍可先 drain 缓冲区数据；耗尽后 receive 应失败
TEST(ChannelTest, CloseAllowsDrainThenReceiveFails) {
  Channel<int> ch(2);
  int first = -1;
  bool second_failed = false;

  auto sender = [&]() -> Task<void> {
    co_await ch.send(7);
    co_return;
  };
  auto s = sender();
  s.resume();
  EXPECT_TRUE(s.done());

  ch.close();

  auto receiver1 = [&]() -> Task<void> {
    first = co_await ch.receive();
    co_return;
  };
  auto r1 = receiver1();
  r1.resume();
  EXPECT_TRUE(r1.done());
  EXPECT_EQ(first, 7);

  auto receiver2 = [&]() -> Task<void> {
    try {
      (void)co_await ch.receive();
    } catch (const std::runtime_error &) {
      second_failed = true;
    }
    co_return;
  };
  auto r2 = receiver2();
  r2.resume();
  EXPECT_TRUE(r2.done());
  EXPECT_TRUE(second_failed);
}

// close() 时应唤醒所有已挂起 sender，并全部失败返回
// close() 时应唤醒所有已挂起 sender，并全部失败返回
TEST(ChannelTest, CloseWakesAllBlockedSenders) {
  Channel<int> ch(1);
  constexpr int kBlocked = 3;
  int failed = 0;
  int succeeded = 0;

  auto seed = [&]() -> Task<void> {
    co_await ch.send(100); // 先填满 buffer
    co_return;
  };
  auto seed_task = seed();
  seed_task.resume();
  EXPECT_TRUE(seed_task.done());

  auto make_sender = [&](int v) -> Task<void> {
    try {
      co_await ch.send(v);
      ++succeeded;
    } catch (const std::runtime_error &) {
      ++failed;
    }
    co_return;
  };

  std::vector<Task<void>> blocked;
  blocked.reserve(kBlocked);
  for (int i = 0; i < kBlocked; ++i) {
    blocked.push_back(make_sender(i));
  }

  for (auto &t : blocked) {
    t.resume();
    EXPECT_FALSE(t.done());
  }

  ch.close();

  for (auto &t : blocked) {
    EXPECT_TRUE(t.done());
  }
  EXPECT_EQ(succeeded, 0);
  EXPECT_EQ(failed, kBlocked);
}

// close() 时应唤醒所有已挂起 receiver，并全部失败返回
TEST(ChannelTest, CloseWakesAllBlockedReceivers) {
  Channel<int> ch(1);
  constexpr int kBlocked = 3;
  int failed = 0;
  int succeeded = 0;

  auto make_receiver = [&]() -> Task<void> {
    try {
      (void)co_await ch.receive();
      ++succeeded;
    } catch (const std::runtime_error &) {
      ++failed;
    }
    co_return;
  };

  std::vector<Task<void>> blocked;
  blocked.reserve(kBlocked);
  for (int i = 0; i < kBlocked; ++i) {
    (void)i;
    blocked.push_back(make_receiver());
  }

  for (auto &t : blocked) {
    t.resume();
    EXPECT_FALSE(t.done());
  }

  ch.close();

  for (auto &t : blocked) {
    EXPECT_TRUE(t.done());
  }
  EXPECT_EQ(succeeded, 0);
  EXPECT_EQ(failed, kBlocked);
}

// close() 后应按 FIFO 继续 drain 已有 buffer，之后 receive 失败
TEST(ChannelTest, CloseDrainsBufferedValuesInOrder) {
  Channel<int> ch(3);

  auto send_value = [&](int v) -> Task<void> {
    co_await ch.send(v);
    co_return;
  };

  auto s1 = send_value(1);
  auto s2 = send_value(2);

  s1.resume();
  s2.resume();
  EXPECT_TRUE(s1.done());
  EXPECT_TRUE(s2.done());

  ch.close();

  int a = -1;
  int b = -1;
  bool failed = false;

  auto recv_into = [&](int &out) -> Task<void> {
    out = co_await ch.receive();
    co_return;
  };
  auto recv_fail = [&]() -> Task<void> {
    try {
      (void)co_await ch.receive();
    } catch (const std::runtime_error &) {
      failed = true;
    }
    co_return;
  };

  auto r1 = recv_into(a);
  auto r2 = recv_into(b);
  auto r3 = recv_fail();

  r1.resume();
  r2.resume();
  r3.resume();

  EXPECT_TRUE(r1.done());
  EXPECT_TRUE(r2.done());
  EXPECT_TRUE(r3.done());
  EXPECT_EQ(a, 1);
  EXPECT_EQ(b, 2);
  EXPECT_TRUE(failed);
}

// close() 之后不应影响已缓冲数据的读取
TEST(ChannelTest, CloseDoesNotDropBufferedData) {
  Channel<int> ch(2);

  auto sender_fn = [&]() -> Task<void> {
    co_await ch.send(42);
    co_return;
  };
  auto sender = sender_fn();
  sender.resume();
  EXPECT_TRUE(sender.done());

  ch.close();

  int got = -1;
  auto receiver_fn = [&]() -> Task<void> {
    got = co_await ch.receive();
    co_return;
  };
  auto receiver = receiver_fn();
  receiver.resume();

  EXPECT_TRUE(receiver.done());
  EXPECT_EQ(got, 42);
}
