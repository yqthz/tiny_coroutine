#include "channel.h"
#include "scheduler.h"
#include "task.h"
#include "test_utils.h"
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
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
  std::atomic<bool> sender2_started{false};
  std::atomic<bool> sender2_done{false};

  auto sender1 = [&]() -> Task<void> {
    co_await ch.send(1);
    co_return;
  };

  auto sender2 = [&]() -> Task<void> {
    sender2_started.store(true, std::memory_order_release);
    co_await ch.send(2); // buffer 满，应被挂起
    sender2_done.store(true, std::memory_order_release);
    co_return;
  };

  auto receiver = [&]() -> Task<void> {
    co_await ch.receive(); // 消费 1，唤醒 sender2
    co_await ch.receive(); // 消费 2
    co_return;
  };

  scheduler.spawn(sender1());
  scheduler.spawn(sender2());

  ASSERT_TRUE(wait_until([&] {
    return sender2_started.load(std::memory_order_acquire);
  }));
  EXPECT_FALSE(sender2_done.load(std::memory_order_acquire));

  scheduler.spawn(receiver());
  ASSERT_TRUE(wait_until([&] {
    return sender2_done.load(std::memory_order_acquire);
  }));
}

// receiver 先于 sender 挂起，sender 到来后直接交付
TEST(ChannelTest, ReceiverBlocksWhenEmpty) {
  Scheduler scheduler(2);
  Channel<int> ch(1);
  std::atomic<int> received{-1};
  std::atomic<bool> receiver_started{false};
  std::atomic<bool> receiver_done{false};

  auto receiver = [&]() -> Task<void> {
    receiver_started.store(true, std::memory_order_release);
    received.store(co_await ch.receive(), std::memory_order_release);
    receiver_done.store(true, std::memory_order_release);
    co_return;
  };

  auto sender = [&]() -> Task<void> {
    co_await ch.send(99);
    co_return;
  };

  scheduler.spawn(receiver());
  ASSERT_TRUE(wait_until([&] {
    return receiver_started.load(std::memory_order_acquire);
  }));
  EXPECT_FALSE(receiver_done.load(std::memory_order_acquire));

  scheduler.spawn(sender());
  ASSERT_TRUE(wait_until([&] {
    return receiver_done.load(std::memory_order_acquire);
  }));

  EXPECT_EQ(received.load(std::memory_order_acquire), 99);
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

  // 1+2+...+N = N*(N+1)/2
  ASSERT_TRUE(wait_until([&] {
    return sum.load(std::memory_order_acquire) == N * (N + 1) / 2;
  }, std::chrono::milliseconds(500)));
  EXPECT_EQ(sum.load(std::memory_order_acquire), N * (N + 1) / 2);
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

  ASSERT_TRUE(wait_until([&] {
    return receiver_failed.load(std::memory_order_acquire);
  }));
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

namespace {
struct MoveOnlyNonDefault {
  int value;

  MoveOnlyNonDefault() = delete;
  explicit MoveOnlyNonDefault(int v) : value(v) {}
  MoveOnlyNonDefault(const MoveOnlyNonDefault &) = delete;
  MoveOnlyNonDefault &operator=(const MoveOnlyNonDefault &) = delete;
  MoveOnlyNonDefault(MoveOnlyNonDefault &&) noexcept = default;
  MoveOnlyNonDefault &operator=(MoveOnlyNonDefault &&) noexcept = default;
};
} // namespace

// Channel 不应要求 T 可默认构造
TEST(ChannelTest, SupportsMoveOnlyNonDefaultConstructibleType) {
  Channel<MoveOnlyNonDefault> ch(1);
  int received = -1;

  auto sender = [&]() -> Task<void> {
    co_await ch.send(MoveOnlyNonDefault{42});
    co_return;
  };

  auto receiver = [&]() -> Task<void> {
    auto v = co_await ch.receive();
    received = v.value;
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

namespace {
struct MoveCountingPayload {
  static inline int moves = 0;
  int value;

  explicit MoveCountingPayload(int v) : value(v) {}
  MoveCountingPayload(const MoveCountingPayload &) = delete;
  MoveCountingPayload &operator=(const MoveCountingPayload &) = delete;

  MoveCountingPayload(MoveCountingPayload &&other) noexcept : value(other.value) {
    ++moves;
    other.value = -1;
  }

  MoveCountingPayload &operator=(MoveCountingPayload &&other) noexcept {
    if (this != &other) {
      ++moves;
      value = other.value;
      other.value = -1;
    }
    return *this;
  }
};
} // namespace

// send(std::move(x)) 不应再多一次按值参数导致的额外移动
TEST(ChannelTest, SendPerfectForwardingAvoidsExtraMove) {
  Channel<MoveCountingPayload> ch(1);
  MoveCountingPayload::moves = 0;

  auto sender = [&]() -> Task<void> {
    MoveCountingPayload payload{7};
    co_await ch.send(std::move(payload));
    co_return;
  };

  auto s = sender();
  s.resume();

  EXPECT_TRUE(s.done());
  EXPECT_EQ(MoveCountingPayload::moves, 2);
}

// try_send/try_receive: 非阻塞基础路径
TEST(ChannelTest, TrySendAndTryReceiveBasic) {
  Channel<int> ch(1);

  EXPECT_TRUE(ch.try_send(11));
  auto v = ch.try_receive();
  ASSERT_TRUE(v.has_value());
  EXPECT_EQ(*v, 11);
}

// try_send: buffer 满时返回 false（不挂起）
TEST(ChannelTest, TrySendReturnsFalseWhenFull) {
  Channel<int> ch(1);

  EXPECT_TRUE(ch.try_send(1));
  EXPECT_FALSE(ch.try_send(2));

  auto v = ch.try_receive();
  ASSERT_TRUE(v.has_value());
  EXPECT_EQ(*v, 1);
}

// try_receive: 打开且空时返回 nullopt（不抛异常）
TEST(ChannelTest, TryReceiveReturnsNulloptWhenOpenAndEmpty) {
  Channel<int> ch(1);
  auto v = ch.try_receive();
  EXPECT_FALSE(v.has_value());
}

// try_receive: 关闭且空时抛异常
TEST(ChannelTest, TryReceiveThrowsWhenClosedAndEmpty) {
  Channel<int> ch(1);
  ch.close();
  EXPECT_THROW((void)ch.try_receive(), std::runtime_error);
}

// try_send: 关闭后抛异常
TEST(ChannelTest, TrySendThrowsAfterClose) {
  Channel<int> ch(1);
  ch.close();
  EXPECT_THROW((void)ch.try_send(1), std::runtime_error);
}

// try_receive 消费后应唤醒一个已阻塞 sender，并把其数据补回 buffer
TEST(ChannelTest, TryReceiveWakesBlockedSenderAndRefillsBuffer) {
  Channel<int> ch(1);
  std::atomic<bool> sender2_started{false};
  std::atomic<bool> sender2_done{false};

  auto sender1 = [&]() -> Task<void> {
    co_await ch.send(1); // 填满 buffer
    co_return;
  };

  auto sender2 = [&]() -> Task<void> {
    sender2_started.store(true, std::memory_order_release);
    co_await ch.send(2); // 应阻塞，直到 try_receive 消费 1
    sender2_done.store(true, std::memory_order_release);
    co_return;
  };

  auto s1 = sender1();
  s1.resume();
  EXPECT_TRUE(s1.done());

  auto s2 = sender2();
  s2.resume();
  ASSERT_TRUE(wait_until([&] {
    return sender2_started.load(std::memory_order_acquire);
  }));
  EXPECT_FALSE(sender2_done.load(std::memory_order_acquire));

  auto first = ch.try_receive();
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(*first, 1);

  ASSERT_TRUE(wait_until([&] {
    return sender2_done.load(std::memory_order_acquire);
  }));

  auto second = ch.try_receive();
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(*second, 2);
}
