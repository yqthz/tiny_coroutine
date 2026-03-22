#include <gtest/gtest.h>
#include "scheduler.h"
#include "task.h"
#include "test_utils.h"
#include "when_all.h"

#include <atomic>
#include <vector>

using namespace tiny_coroutine;

// when_all(vector<Task<T>>) 收集所有结果
TEST(WhenAllTest, VectorTasks) {
  Scheduler scheduler(4);
  std::atomic<bool> done{false};
  std::vector<int> results;

  auto make_task = [](int v) -> Task<int> { co_return v * v; };

  auto driver = [&]() -> Task<void> {
    std::vector<Task<int>> tasks;
    for (int i = 1; i <= 5; i++) {
      tasks.push_back(make_task(i));
    }
    results = co_await when_all(scheduler, std::move(tasks));
    done.store(true);
    co_return;
  };

  scheduler.spawn(driver());

  ASSERT_TRUE(wait_until([&] { return done.load(); }));
  ASSERT_EQ(results.size(), 5u);
  // 结果顺序对应输入顺序
  for (int i = 0; i < 5; i++) {
    EXPECT_EQ(results[i], (i + 1) * (i + 1));
  }
}

// when_all(variadic) 收集异构结果
TEST(WhenAllTest, VariadicTasks) {
  Scheduler scheduler(4);
  std::atomic<bool> done{false};
  int ri = 0;
  double rd = 0.0;

  auto int_task = []() -> Task<int> { co_return 7; };
  auto dbl_task = []() -> Task<double> { co_return 3.14; };

  auto driver = [&]() -> Task<void> {
    auto [i, d] = co_await when_all(scheduler, int_task(), dbl_task());
    ri = i;
    rd = d;
    done.store(true);
    co_return;
  };

  scheduler.spawn(driver());

  ASSERT_TRUE(wait_until([&] { return done.load(); }));
  EXPECT_EQ(ri, 7);
  EXPECT_DOUBLE_EQ(rd, 3.14);
}

// 空 vector 立即完成
TEST(WhenAllTest, EmptyVector) {
  Scheduler scheduler(2);
  std::atomic<bool> done{false};
  std::vector<int> results;

  auto driver = [&]() -> Task<void> {
    std::vector<Task<int>> tasks;
    results = co_await when_all(scheduler, std::move(tasks));
    done.store(true);
    co_return;
  };

  scheduler.spawn(driver());

  ASSERT_TRUE(wait_until([&] { return done.load(); }));
  EXPECT_TRUE(results.empty());
}
