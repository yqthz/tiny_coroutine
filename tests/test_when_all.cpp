#include <gtest/gtest.h>
#include "runtime/scheduler.h"
#include "task.h"
#include "test_utils.h"
#include "when_all.h"

#include <atomic>
#include <memory>
#include <stdexcept>
#include <vector>

using namespace tiny_coroutine;

// when_all(vector<Task<T>>) 收集所有结果
TEST(WhenAllTest, VectorTasks) {
  runtime::Scheduler scheduler;
  scheduler.init(4);

  std::atomic<bool> done{false};
  std::vector<int> results;

  auto make_task = [](int v) -> Task<int> { co_return v * v; };

  auto driver = [&]() -> Task<void> {
    std::vector<Task<int>> tasks;
    for (int i = 1; i <= 5; i++) {
      tasks.push_back(make_task(i));
    }
    results = co_await when_all(std::move(tasks));
    done.store(true, std::memory_order_release);
    co_return;
  };

  scheduler.submit(driver());

  ASSERT_TRUE(wait_until(
      [&] { return done.load(std::memory_order_acquire); },
      std::chrono::milliseconds(1000)));
  scheduler.stop();

  ASSERT_EQ(results.size(), 5u);
  for (int i = 0; i < 5; i++) {
    EXPECT_EQ(results[i], (i + 1) * (i + 1));
  }
}

// when_all(variadic) 收集异构结果
TEST(WhenAllTest, VariadicTasks) {
  runtime::Scheduler scheduler;
  scheduler.init(4);

  std::atomic<bool> done{false};
  int ri = 0;
  double rd = 0.0;

  auto int_task = []() -> Task<int> { co_return 7; };
  auto dbl_task = []() -> Task<double> { co_return 3.14; };

  auto driver = [&]() -> Task<void> {
    auto [i, d] = co_await when_all(int_task(), dbl_task());
    ri = i;
    rd = d;
    done.store(true, std::memory_order_release);
    co_return;
  };

  scheduler.submit(driver());

  ASSERT_TRUE(wait_until(
      [&] { return done.load(std::memory_order_acquire); },
      std::chrono::milliseconds(1000)));
  scheduler.stop();

  EXPECT_EQ(ri, 7);
  EXPECT_DOUBLE_EQ(rd, 3.14);
}

// 空 vector 立即完成
TEST(WhenAllTest, EmptyVector) {
  runtime::Scheduler scheduler;
  scheduler.init(2);

  std::atomic<bool> done{false};
  std::vector<int> results;

  auto driver = [&]() -> Task<void> {
    std::vector<Task<int>> tasks;
    results = co_await when_all(std::move(tasks));
    done.store(true, std::memory_order_release);
    co_return;
  };

  scheduler.submit(driver());

  ASSERT_TRUE(wait_until(
      [&] { return done.load(std::memory_order_acquire); },
      std::chrono::milliseconds(1000)));
  scheduler.stop();

  EXPECT_EQ(results.empty(), true);
}

// 子任务异常时 when_all 不应卡死，应向上传播异常
TEST(WhenAllTest, VectorTaskExceptionPropagatesWithoutDeadlock) {
  runtime::Scheduler scheduler;
  scheduler.init(4);

  std::atomic<bool> done{false};
  std::atomic<bool> got_exception{false};

  auto ok_task = []() -> Task<int> { co_return 1; };
  auto bad_task = []() -> Task<int> {
    throw std::runtime_error("boom");
    co_return 0;
  };

  auto driver = [&]() -> Task<void> {
    std::vector<Task<int>> tasks;
    tasks.push_back(ok_task());
    tasks.push_back(bad_task());
    tasks.push_back(ok_task());

    try {
      auto values = co_await when_all(std::move(tasks));
      (void)values;
    } catch (const std::runtime_error &) {
      got_exception.store(true, std::memory_order_release);
    }

    done.store(true, std::memory_order_release);
    co_return;
  };

  scheduler.submit(driver());

  ASSERT_TRUE(wait_until(
      [&] { return done.load(std::memory_order_acquire); },
      std::chrono::milliseconds(1000)));
  scheduler.stop();

  EXPECT_EQ(got_exception.load(std::memory_order_acquire), true);
}

namespace {
struct MoveOnlyNonDefault {
  int value;

  MoveOnlyNonDefault() = delete;
  explicit MoveOnlyNonDefault(int v) : value(v) {}
  MoveOnlyNonDefault(const MoveOnlyNonDefault &) = delete;
  MoveOnlyNonDefault &operator=(const MoveOnlyNonDefault &) = delete;
  MoveOnlyNonDefault(MoveOnlyNonDefault &&other) noexcept : value(other.value) {
    other.value = -1;
  }
  MoveOnlyNonDefault &operator=(MoveOnlyNonDefault &&other) noexcept {
    if (this == &other) {
      return *this;
    }
    value = other.value;
    other.value = -1;
    return *this;
  }
};
} // namespace

// variadic when_all 不应要求默认构造
TEST(WhenAllTest, VariadicSupportsMoveOnlyNonDefaultConstructibleType) {
  runtime::Scheduler scheduler;
  scheduler.init(2);

  std::atomic<bool> done{false};
  int payload = 0;
  int plain = 0;

  auto move_only_task = []() -> Task<MoveOnlyNonDefault> {
    co_return MoveOnlyNonDefault{42};
  };
  auto int_task = []() -> Task<int> { co_return 7; };

  auto driver = [&]() -> Task<void> {
    auto [m, i] = co_await when_all(move_only_task(), int_task());
    payload = m.value;
    plain = i;
    done.store(true, std::memory_order_release);
    co_return;
  };

  scheduler.submit(driver());

  ASSERT_TRUE(wait_until(
      [&] { return done.load(std::memory_order_acquire); },
      std::chrono::milliseconds(1000)));
  scheduler.stop();

  EXPECT_EQ(payload, 42);
  EXPECT_EQ(plain, 7);
}
