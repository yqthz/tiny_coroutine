#include <gtest/gtest.h>

#include "runtime/io_awaiter.h"
#include "runtime/scheduler.h"
#include "task.h"

#include <array>
#include <atomic>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

using namespace tiny_coroutine;

TEST(RuntimeSchedulerTest, SchedulerLoopCompletesAllTasks) {
  runtime::Scheduler scheduler;
  scheduler.init(2);

  std::atomic<int> counter{0};
  constexpr int kTaskCount = 1000;

  auto make_task = [&]() -> Task<void> {
    counter.fetch_add(1, std::memory_order_relaxed);
    co_return;
  };

  for (int i = 0; i < kTaskCount; ++i) {
    scheduler.submit(make_task());
  }

  scheduler.loop();
  EXPECT_EQ(counter.load(std::memory_order_relaxed), kTaskCount);
}

TEST(RuntimeSchedulerTest, TaskCanSubmitSubTasksInsideCoroutine) {
  runtime::Scheduler scheduler;
  scheduler.init(2);

  std::atomic<int> counter{0};

  auto sub_task = [&]() -> Task<void> {
    counter.fetch_add(1, std::memory_order_relaxed);
    co_return;
  };

  auto parent_task = [&]() -> Task<void> {
    scheduler.submit(sub_task());
    scheduler.submit(sub_task());
    co_await scheduler.schedule();
    counter.fetch_add(1, std::memory_order_relaxed);
    co_return;
  };

  constexpr int kParentCount = 32;
  for (int i = 0; i < kParentCount; ++i) {
    scheduler.submit(parent_task());
  }

  scheduler.loop();

  const int expected = kParentCount * 3;
  EXPECT_EQ(counter.load(std::memory_order_relaxed), expected);
}

TEST(RuntimeSchedulerTest, SubmitToContextUsesLocalContextPath) {
  runtime::Scheduler scheduler;
  scheduler.init(2);

  std::atomic<int> counter{0};

  auto local_task = [&]() -> Task<void> {
    counter.fetch_add(1, std::memory_order_relaxed);
    co_return;
  };

  auto parent_task = [&]() -> Task<void> {
    runtime::submit_to_context(local_task());
    runtime::submit_to_context(local_task());
    co_await scheduler.schedule();
    counter.fetch_add(1, std::memory_order_relaxed);
    co_return;
  };

  constexpr int kParentCount = 64;
  for (int i = 0; i < kParentCount; ++i) {
    scheduler.submit(parent_task());
  }

  scheduler.loop();

  const int expected = kParentCount * 3;
  EXPECT_EQ(counter.load(std::memory_order_relaxed), expected);
}

TEST(RuntimeSchedulerTest, SubmitToSchedulerUsesLocalSchedulerPath) {
  runtime::Scheduler scheduler;
  scheduler.init(2);

  std::atomic<int> counter{0};

  auto sub_task = [&]() -> Task<void> {
    counter.fetch_add(1, std::memory_order_relaxed);
    co_return;
  };

  auto parent_task = [&]() -> Task<void> {
    runtime::submit_to_scheduler(sub_task());
    runtime::submit_to_scheduler(sub_task());
    co_await scheduler.schedule();
    counter.fetch_add(1, std::memory_order_relaxed);
    co_return;
  };

  constexpr int kParentCount = 64;
  for (int i = 0; i < kParentCount; ++i) {
    scheduler.submit(parent_task());
  }

  scheduler.loop();

  const int expected = kParentCount * 3;
  EXPECT_EQ(counter.load(std::memory_order_relaxed), expected);
}

TEST(RuntimeSchedulerTest, IoSuspendOnceResumesViaPollPhase) {
  runtime::Scheduler scheduler;
  scheduler.init(2);

  std::atomic<int> before_suspend{0};
  std::atomic<int> after_resume{0};

  auto io_task = [&]() -> Task<void> {
    before_suspend.fetch_add(1, std::memory_order_relaxed);
    co_await runtime::io_suspend_once();
    after_resume.fetch_add(1, std::memory_order_relaxed);
    co_return;
  };

  constexpr int kTaskCount = 128;
  for (int i = 0; i < kTaskCount; ++i) {
    scheduler.submit(io_task());
  }

  scheduler.loop();

  EXPECT_EQ(before_suspend.load(std::memory_order_relaxed), kTaskCount);
  EXPECT_EQ(after_resume.load(std::memory_order_relaxed), kTaskCount);
}

TEST(RuntimeSchedulerTest, IoReadAwaiterReadsFileContent) {
  char filename[] = "/tmp/tinycoro_runtime_io_XXXXXX";
  const int fd = mkstemp(filename);
  ASSERT_GE(fd, 0);

  const char kPayload[] = "hello-runtime-io";
  const ssize_t written =
      ::write(fd, kPayload, static_cast<size_t>(sizeof(kPayload) - 1));
  ASSERT_EQ(written, static_cast<ssize_t>(sizeof(kPayload) - 1));

  runtime::Scheduler scheduler;
  scheduler.init(2);

  std::array<char, sizeof(kPayload)> buffer{};
  std::atomic<int> read_result{0};

  auto read_task = [&]() -> Task<void> {
    const int32_t n =
        co_await runtime::io::read(fd, buffer.data(), sizeof(kPayload) - 1, 0);
    read_result.store(n, std::memory_order_relaxed);
    co_return;
  };

  scheduler.submit(read_task());
  scheduler.loop();

  EXPECT_EQ(read_result.load(std::memory_order_relaxed),
            static_cast<int>(sizeof(kPayload) - 1));
  EXPECT_EQ(std::memcmp(buffer.data(), kPayload, sizeof(kPayload) - 1), 0);

  ::close(fd);
  ::unlink(filename);
}

TEST(RuntimeSchedulerTest, IoWriteAwaiterWritesFileContent) {
  char filename[] = "/tmp/tinycoro_runtime_io_write_XXXXXX";
  const int fd = mkstemp(filename);
  ASSERT_GE(fd, 0);

  const char kPayload[] = "runtime-write-ok";

  runtime::Scheduler scheduler;
  scheduler.init(2);

  std::atomic<int> write_result{0};

  auto write_task = [&]() -> Task<void> {
    const int32_t n =
        co_await runtime::io::write(fd, kPayload, sizeof(kPayload) - 1, 0);
    write_result.store(n, std::memory_order_relaxed);
    co_return;
  };

  scheduler.submit(write_task());
  scheduler.loop();

  std::array<char, sizeof(kPayload)> buffer{};
  const ssize_t read_back =
      ::pread(fd, buffer.data(), sizeof(kPayload) - 1, 0);

  EXPECT_EQ(write_result.load(std::memory_order_relaxed),
            static_cast<int>(sizeof(kPayload) - 1));
  EXPECT_EQ(read_back, static_cast<ssize_t>(sizeof(kPayload) - 1));
  EXPECT_EQ(std::memcmp(buffer.data(), kPayload, sizeof(kPayload) - 1), 0);

  ::close(fd);
  ::unlink(filename);
}
