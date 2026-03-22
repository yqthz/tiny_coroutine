#include <gtest/gtest.h>
#include "task.h"

#include <memory>
#include <stdexcept>

using namespace tiny_coroutine;

TEST(TaskTest, GetMovesResultForMoveOnlyType) {
  auto make_task = []() -> Task<std::unique_ptr<int>> {
    co_return std::make_unique<int>(42);
  };

  auto t = make_task();
  t.resume();

  auto ptr = t.get();
  ASSERT_NE(ptr, nullptr);
  EXPECT_EQ(*ptr, 42);
}

TEST(TaskTest, AwaitResumeMovesResultForMoveOnlyType) {
  auto inner = []() -> Task<std::unique_ptr<int>> {
    co_return std::make_unique<int>(7);
  };

  auto outer = [&]() -> Task<int> {
    auto ptr = co_await inner();
    co_return *ptr * 6;
  };

  auto t = outer();
  t.resume();
  EXPECT_EQ(t.get(), 42);
}

TEST(TaskTest, ExceptionPropagatesFromGet) {
  auto f = []() -> Task<int> {
    throw std::runtime_error("boom");
    co_return 0;
  };

  auto t = f();
  t.resume();
  EXPECT_THROW((void)t.get(), std::runtime_error);
}
