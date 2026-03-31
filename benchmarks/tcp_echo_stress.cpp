#include <arpa/inet.h>
#include <atomic>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <cstddef>
#include <limits>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

struct StressConfig {
  std::string host{"127.0.0.1"};
  uint16_t port{12345};
  int connections{128};
  int threads{0};
  int payload_bytes{64};
  int duration_sec{10};
  int warmup_sec{2};
  int report_interval_sec{1};
  int sample_rate{100};
  bool nodelay{true};
};

struct ThreadStats {
  uint64_t requests{0};
  uint64_t bytes{0};
  uint64_t errors{0};
  uint64_t latency_sum_ns{0};
  uint64_t latency_max_ns{0};
  std::vector<uint32_t> latency_samples_us;
};

std::atomic<uint64_t> g_reqs{0};
std::atomic<uint64_t> g_bytes{0};
std::atomic<uint64_t> g_errs{0};
std::atomic<bool> g_stop{false};

void printUsage(const char *prog) {
  std::cout
      << "Usage: " << prog << " [options]\n"
      << "  --host 127.0.0.1\n"
      << "  --port 12345\n"
      << "  --connections 128\n"
      << "  --threads N (default: hardware concurrency)\n"
      << "  --payload 64\n"
      << "  --duration 10\n"
      << "  --warmup 2\n"
      << "  --report 1\n"
      << "  --sample-rate 100\n"
      << "  --no-nodelay\n";
}

bool parseArgs(int argc, char **argv, StressConfig &cfg) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    if (arg == "--host" && i + 1 < argc) {
      cfg.host = argv[++i];
    } else if (arg == "--port" && i + 1 < argc) {
      cfg.port = static_cast<uint16_t>(std::strtoul(argv[++i], nullptr, 10));
    } else if (arg == "--connections" && i + 1 < argc) {
      cfg.connections = static_cast<int>(std::strtol(argv[++i], nullptr, 10));
    } else if (arg == "--threads" && i + 1 < argc) {
      cfg.threads = static_cast<int>(std::strtol(argv[++i], nullptr, 10));
    } else if (arg == "--payload" && i + 1 < argc) {
      cfg.payload_bytes = static_cast<int>(std::strtol(argv[++i], nullptr, 10));
    } else if (arg == "--duration" && i + 1 < argc) {
      cfg.duration_sec = static_cast<int>(std::strtol(argv[++i], nullptr, 10));
    } else if (arg == "--warmup" && i + 1 < argc) {
      cfg.warmup_sec = static_cast<int>(std::strtol(argv[++i], nullptr, 10));
    } else if (arg == "--report" && i + 1 < argc) {
      cfg.report_interval_sec = static_cast<int>(std::strtol(argv[++i], nullptr, 10));
    } else if (arg == "--sample-rate" && i + 1 < argc) {
      cfg.sample_rate = static_cast<int>(std::strtol(argv[++i], nullptr, 10));
    } else if (arg == "--no-nodelay") {
      cfg.nodelay = false;
    } else if (arg == "--help") {
      return false;
    } else {
      std::cerr << "Unknown arg: " << arg << "\n";
      return false;
    }
  }

  if (cfg.port == 0 || cfg.connections <= 0 || cfg.payload_bytes <= 0 ||
      cfg.duration_sec <= 0 || cfg.warmup_sec < 0 || cfg.report_interval_sec <= 0 ||
      cfg.sample_rate <= 0) {
    return false;
  }
  return true;
}

int connectOne(const StressConfig &cfg) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }

  if (cfg.nodelay) {
    int one = 1;
    (void)::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(cfg.port);
  if (::inet_pton(AF_INET, cfg.host.c_str(), &addr.sin_addr) != 1) {
    ::close(fd);
    return -1;
  }

  if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return -1;
  }

  return fd;
}

bool writeAll(int fd, const char *buf, int len) {
  int sent = 0;
  while (sent < len) {
    const ssize_t n = ::send(fd, buf + sent, static_cast<size_t>(len - sent), 0);
    if (n <= 0) {
      return false;
    }
    sent += static_cast<int>(n);
  }
  return true;
}

bool readAll(int fd, char *buf, int len) {
  int recved = 0;
  while (recved < len) {
    const ssize_t n = ::recv(fd, buf + recved, static_cast<size_t>(len - recved), 0);
    if (n <= 0) {
      return false;
    }
    recved += static_cast<int>(n);
  }
  return true;
}

uint64_t percentileUs(std::vector<uint32_t> &samples, double p) {
  if (samples.empty()) {
    return 0;
  }
  const double rank = p * static_cast<double>(samples.size() - 1);
  const size_t idx = static_cast<size_t>(rank);
  std::nth_element(samples.begin(), samples.begin() + static_cast<std::ptrdiff_t>(idx),
                   samples.end());
  return samples[idx];
}

void reporterLoop(int interval_sec) {
  uint64_t prev_reqs = 0;
  uint64_t prev_bytes = 0;
  while (!g_stop.load(std::memory_order_acquire)) {
    std::this_thread::sleep_for(std::chrono::seconds(interval_sec));

    const uint64_t reqs = g_reqs.load(std::memory_order_relaxed);
    const uint64_t bytes = g_bytes.load(std::memory_order_relaxed);
    const uint64_t errs = g_errs.load(std::memory_order_relaxed);

    const uint64_t d_reqs = reqs - prev_reqs;
    const uint64_t d_bytes = bytes - prev_bytes;
    prev_reqs = reqs;
    prev_bytes = bytes;

    const double qps = static_cast<double>(d_reqs) / interval_sec;
    const double mibps = static_cast<double>(d_bytes) / (1024.0 * 1024.0) / interval_sec;

    std::cout << "[stress] qps=" << qps << " throughput=" << mibps
              << " MiB/s total_reqs=" << reqs << " errors=" << errs << "\n";
  }
}

} // namespace

int main(int argc, char **argv) {
  StressConfig cfg;
  if (!parseArgs(argc, argv, cfg)) {
    printUsage(argv[0]);
    return 1;
  }

  unsigned int hw = std::thread::hardware_concurrency();
  if (hw == 0) {
    hw = 1;
  }
  const int threads = cfg.threads > 0 ? cfg.threads : static_cast<int>(hw);

  std::cout << "tcpecho stress start host=" << cfg.host << ':' << cfg.port
            << " connections=" << cfg.connections
            << " threads=" << threads
            << " payload=" << cfg.payload_bytes
            << " duration=" << cfg.duration_sec << "s"
            << " warmup=" << cfg.warmup_sec << "s\n";

  std::vector<std::vector<int>> thread_fds(static_cast<size_t>(threads));
  for (int i = 0; i < cfg.connections; ++i) {
    const int fd = connectOne(cfg);
    if (fd < 0) {
      std::cerr << "connect failed at conn#" << i << ": " << std::strerror(errno)
                << "\n";
      for (auto &v : thread_fds) {
        for (int old_fd : v) {
          ::close(old_fd);
        }
      }
      return 1;
    }
    thread_fds[static_cast<size_t>(i % threads)].push_back(fd);
  }

  std::vector<char> payload(static_cast<size_t>(cfg.payload_bytes), 'x');
  std::atomic<bool> begin{false};
  std::atomic<int> ready_threads{0};

  std::vector<ThreadStats> stats(static_cast<size_t>(threads));
  std::vector<std::thread> workers;
  workers.reserve(static_cast<size_t>(threads));

  const auto warmup_begin = std::chrono::steady_clock::now();
  const auto warmup_end = warmup_begin + std::chrono::seconds(cfg.warmup_sec);
  const auto run_end = warmup_end + std::chrono::seconds(cfg.duration_sec);

  std::thread reporter(reporterLoop, cfg.report_interval_sec);

  for (int tid = 0; tid < threads; ++tid) {
    workers.emplace_back([&, tid]() {
      auto &my_stats = stats[static_cast<size_t>(tid)];
      auto &fds = thread_fds[static_cast<size_t>(tid)];
      std::vector<char> recv_buf(static_cast<size_t>(cfg.payload_bytes));

      ready_threads.fetch_add(1, std::memory_order_acq_rel);
      while (!begin.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }

      uint64_t local_req = 0;

      while (std::chrono::steady_clock::now() < run_end) {
        for (int fd : fds) {
          const auto t0 = std::chrono::steady_clock::now();
          if (!writeAll(fd, payload.data(), cfg.payload_bytes)) {
            ++my_stats.errors;
            continue;
          }
          if (!readAll(fd, recv_buf.data(), cfg.payload_bytes)) {
            ++my_stats.errors;
            continue;
          }
          const auto t1 = std::chrono::steady_clock::now();
          const auto ns = static_cast<uint64_t>(
              std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());

          ++local_req;
          if (t1 >= warmup_end) {
            ++my_stats.requests;
            my_stats.bytes += static_cast<uint64_t>(cfg.payload_bytes);
            my_stats.latency_sum_ns += ns;
            if (ns > my_stats.latency_max_ns) {
              my_stats.latency_max_ns = ns;
            }
            if ((local_req % static_cast<uint64_t>(cfg.sample_rate)) == 0) {
              const uint64_t us = ns / 1000;
              my_stats.latency_samples_us.push_back(
                  static_cast<uint32_t>(us > std::numeric_limits<uint32_t>::max()
                                            ? std::numeric_limits<uint32_t>::max()
                                            : us));
            }

            g_reqs.fetch_add(1, std::memory_order_relaxed);
            g_bytes.fetch_add(static_cast<uint64_t>(cfg.payload_bytes),
                              std::memory_order_relaxed);
          }
        }
      }

      g_errs.fetch_add(my_stats.errors, std::memory_order_relaxed);

      for (int fd : fds) {
        ::close(fd);
      }
    });
  }

  while (ready_threads.load(std::memory_order_acquire) != threads) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  begin.store(true, std::memory_order_release);

  for (auto &t : workers) {
    t.join();
  }

  g_stop.store(true, std::memory_order_release);
  if (reporter.joinable()) {
    reporter.join();
  }

  uint64_t total_reqs = 0;
  uint64_t total_bytes = 0;
  uint64_t total_errors = 0;
  uint64_t total_lat_sum_ns = 0;
  uint64_t max_lat_ns = 0;
  std::vector<uint32_t> merged_samples;

  for (auto &s : stats) {
    total_reqs += s.requests;
    total_bytes += s.bytes;
    total_errors += s.errors;
    total_lat_sum_ns += s.latency_sum_ns;
    if (s.latency_max_ns > max_lat_ns) {
      max_lat_ns = s.latency_max_ns;
    }
    merged_samples.insert(merged_samples.end(), s.latency_samples_us.begin(),
                          s.latency_samples_us.end());
  }

  const double seconds = static_cast<double>(cfg.duration_sec);
  const double qps = seconds > 0 ? static_cast<double>(total_reqs) / seconds : 0.0;
  const double mibps = seconds > 0
                           ? (static_cast<double>(total_bytes) / (1024.0 * 1024.0)) /
                                 seconds
                           : 0.0;
  const double avg_us = total_reqs > 0
                            ? (static_cast<double>(total_lat_sum_ns) / total_reqs) /
                                  1000.0
                            : 0.0;

  uint64_t p50 = 0;
  uint64_t p95 = 0;
  uint64_t p99 = 0;
  if (!merged_samples.empty()) {
    p50 = percentileUs(merged_samples, 0.50);
    p95 = percentileUs(merged_samples, 0.95);
    p99 = percentileUs(merged_samples, 0.99);
  }

  std::cout << "\n=== tcpecho stress summary ===\n"
            << "requests: " << total_reqs << "\n"
            << "errors: " << total_errors << "\n"
            << "qps: " << qps << "\n"
            << "throughput: " << mibps << " MiB/s\n"
            << "latency avg: " << avg_us << " us\n"
            << "latency p50/p95/p99: " << p50 << "/" << p95 << "/" << p99
            << " us\n"
            << "latency max: " << (max_lat_ns / 1000.0) << " us\n"
            << "samples: " << merged_samples.size() << " (rate=1/" << cfg.sample_rate
            << ")\n";

  return 0;
}
