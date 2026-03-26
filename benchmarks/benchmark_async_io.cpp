#include "runtime/io_awaiter.h"
#include "runtime/scheduler.h"
#include "task.h"

#include <benchmark/benchmark.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

using namespace tiny_coroutine;

namespace {

struct TempFile {
  int fd{-1};
  std::string path;

  explicit TempFile(std::size_t size_bytes) {
    std::array<char, 64> tmpl{};
    std::snprintf(tmpl.data(), tmpl.size(), "/tmp/tiny_coroutine_bench_XXXXXX");
    fd = ::mkstemp(tmpl.data());
    if (fd < 0) {
      throw std::runtime_error("mkstemp failed");
    }
    path = tmpl.data();

    if (::ftruncate(fd, static_cast<off_t>(size_bytes)) != 0) {
      ::close(fd);
      ::unlink(path.c_str());
      throw std::runtime_error("ftruncate failed");
    }
  }

  ~TempFile() {
    if (fd >= 0) {
      ::close(fd);
    }
    if (!path.empty()) {
      ::unlink(path.c_str());
    }
  }
};

Task<void> ioWriteWorker(int fd, const void *buf, std::size_t block_size,
                         int ops, std::size_t offset,
                         std::atomic<int> &completed,
                         std::atomic<int> &errors) {
  for (int i = 0; i < ops; ++i) {
    const auto ret =
        co_await runtime::io::write(fd, buf, block_size, offset + i * block_size);
    if (ret < 0 || static_cast<std::size_t>(ret) != block_size) {
      errors.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    completed.fetch_add(1, std::memory_order_relaxed);
  }
  co_return;
}

Task<void> ioReadWorker(int fd, void *buf, std::size_t block_size, int ops,
                        std::size_t offset, std::atomic<int> &completed,
                        std::atomic<int> &errors) {
  for (int i = 0; i < ops; ++i) {
    const auto ret =
        co_await runtime::io::read(fd, buf, block_size, offset + i * block_size);
    if (ret < 0 || static_cast<std::size_t>(ret) != block_size) {
      errors.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    completed.fetch_add(1, std::memory_order_relaxed);
  }
  co_return;
}

void setThroughputCounters(benchmark::State &state, int total_ops,
                           std::size_t total_bytes) {
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * total_ops);
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                          static_cast<int64_t>(total_bytes));

  state.counters["io.qps"] = benchmark::Counter(
      static_cast<double>(total_ops),
      benchmark::Counter::kIsIterationInvariantRate);
  state.counters["io.mb_per_s"] = benchmark::Counter(
      static_cast<double>(total_bytes) / (1024.0 * 1024.0),
      benchmark::Counter::kIsIterationInvariantRate);
}

std::string makeBatchLabel(const char *prefix, int submit_batch_size,
                           int completion_batch_size) {
  return std::string(prefix) + " submit_batch=" +
         std::to_string(submit_batch_size) + " completion_batch=" +
         std::to_string(completion_batch_size);
}

} // namespace

static void BM_AsyncWriteThroughput(benchmark::State &state) {
  constexpr std::size_t kBlockSize = 4096;
  const int concurrency = static_cast<int>(state.range(0));
  const int ops_per_worker = static_cast<int>(state.range(1));
  const int submit_batch_size = static_cast<int>(state.range(2));
  const int completion_batch_size = static_cast<int>(state.range(3));
  const int total_ops = concurrency * ops_per_worker;
  const std::size_t total_bytes =
      static_cast<std::size_t>(total_ops) * kBlockSize;

  std::vector<char> write_buf(kBlockSize, 'x');

  for (auto _ : state) {
    TempFile file(total_bytes);
    runtime::Scheduler scheduler;
    scheduler.init(static_cast<size_t>(concurrency));
    std::atomic<int> completed{0};
    std::atomic<int> errors{0};

    for (int worker = 0; worker < concurrency; ++worker) {
      const std::size_t base_offset =
          static_cast<std::size_t>(worker) * ops_per_worker * kBlockSize;
      scheduler.submit(ioWriteWorker(file.fd, write_buf.data(), kBlockSize,
                                     ops_per_worker, base_offset, completed,
                                     errors));
    }

    scheduler.loop();

    if (completed.load(std::memory_order_acquire) != total_ops ||
        errors.load(std::memory_order_acquire) != 0) {
      state.SkipWithError("async_write completion count mismatch");
      break;
    }
  }

  state.SetLabel(
      makeBatchLabel("async_write throughput", submit_batch_size,
                     completion_batch_size));
  setThroughputCounters(state, total_ops, total_bytes);
}

BENCHMARK(BM_AsyncWriteThroughput)
    ->Args({1, 5000, 8, 8})
    ->Args({1, 5000, 32, 32})
    ->Args({1, 5000, 64, 64})
    ->Args({4, 5000, 8, 8})
    ->Args({4, 5000, 32, 32})
    ->Args({4, 5000, 64, 64})
    ->Args({16, 5000, 8, 8})
    ->Args({16, 5000, 32, 32})
    ->Args({16, 5000, 64, 64});

static void BM_AsyncReadThroughput(benchmark::State &state) {
  constexpr std::size_t kBlockSize = 4096;
  const int concurrency = static_cast<int>(state.range(0));
  const int ops_per_worker = static_cast<int>(state.range(1));
  const int submit_batch_size = static_cast<int>(state.range(2));
  const int completion_batch_size = static_cast<int>(state.range(3));
  const int total_ops = concurrency * ops_per_worker;
  const std::size_t total_bytes =
      static_cast<std::size_t>(total_ops) * kBlockSize;

  std::vector<std::vector<char>> read_buffers(
      static_cast<std::size_t>(concurrency), std::vector<char>(kBlockSize));

  for (auto _ : state) {
    TempFile file(total_bytes);
    runtime::Scheduler scheduler;
    scheduler.init(static_cast<size_t>(concurrency));
    std::atomic<int> completed{0};
    std::atomic<int> errors{0};

    for (int worker = 0; worker < concurrency; ++worker) {
      const std::size_t base_offset =
          static_cast<std::size_t>(worker) * ops_per_worker * kBlockSize;
      scheduler.submit(ioReadWorker(file.fd, read_buffers[worker].data(),
                                    kBlockSize, ops_per_worker, base_offset,
                                    completed, errors));
    }

    scheduler.loop();

    if (completed.load(std::memory_order_acquire) != total_ops ||
        errors.load(std::memory_order_acquire) != 0) {
      state.SkipWithError("async_read completion count mismatch");
      break;
    }
  }

  state.SetLabel(
      makeBatchLabel("async_read throughput", submit_batch_size,
                     completion_batch_size));
  setThroughputCounters(state, total_ops, total_bytes);
}

BENCHMARK(BM_AsyncReadThroughput)
    ->Args({1, 5000, 8, 8})
    ->Args({1, 5000, 32, 32})
    ->Args({1, 5000, 64, 64})
    ->Args({4, 5000, 8, 8})
    ->Args({4, 5000, 32, 32})
    ->Args({4, 5000, 64, 64})
    ->Args({16, 5000, 8, 8})
    ->Args({16, 5000, 32, 32})
    ->Args({16, 5000, 64, 64});
