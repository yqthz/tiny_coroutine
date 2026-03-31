#include "runtime/io_awaiter.h"
#include "runtime/scheduler.h"
#include "task.h"

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <iostream>
#include <poll.h>
#include <sys/socket.h>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>

using namespace tiny_coroutine;

namespace {

std::atomic<bool> g_stop{false};

struct ServerConfig {
  std::string host{"0.0.0.0"};
  uint16_t port{12345};
  int workers{0};
  size_t buffer_size{4096};
  int report_interval_sec{1};
};

struct ServerStats {
  std::atomic<uint64_t> accepted{0};
  std::atomic<uint64_t> closed{0};
  std::atomic<uint64_t> bytes_in{0};
  std::atomic<uint64_t> bytes_out{0};
  std::atomic<uint64_t> errors{0};
};

void onSignal(int) {
  g_stop.store(true, std::memory_order_release);
}

int makeListenSocket(const std::string &host, uint16_t port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }

  int one = 1;
  if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) != 0) {
    ::close(fd);
    return -1;
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
    ::close(fd);
    return -1;
  }

  if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return -1;
  }

  if (::listen(fd, 4096) != 0) {
    ::close(fd);
    return -1;
  }

  return fd;
}

Task<void> echoConnection(int fd, size_t buffer_size, ServerStats *stats) {
  std::string buffer(buffer_size, '\0');
  constexpr size_t kStreamOffset = static_cast<size_t>(-1);

  while (!g_stop.load(std::memory_order_acquire)) {
    const int32_t nread =
        co_await runtime::io::read(fd, buffer.data(), buffer.size(), kStreamOffset);
    if (nread == 0) {
      break;
    }
    if (nread < 0) {
      stats->errors.fetch_add(1, std::memory_order_relaxed);
      break;
    }

    stats->bytes_in.fetch_add(static_cast<uint64_t>(nread),
                              std::memory_order_relaxed);

    size_t sent = 0;
    while (sent < static_cast<size_t>(nread)) {
      const auto *data = buffer.data() + sent;
      const size_t remain = static_cast<size_t>(nread) - sent;
      const int32_t nwritten =
          co_await runtime::io::write(fd, data, remain, kStreamOffset);
      if (nwritten <= 0) {
        stats->errors.fetch_add(1, std::memory_order_relaxed);
        sent = 0;
        break;
      }
      sent += static_cast<size_t>(nwritten);
      stats->bytes_out.fetch_add(static_cast<uint64_t>(nwritten),
                                 std::memory_order_relaxed);
    }

    if (sent == 0) {
      break;
    }
  }

  ::close(fd);
  stats->closed.fetch_add(1, std::memory_order_relaxed);
  co_return;
}

bool parseArgs(int argc, char **argv, ServerConfig &cfg) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    if (arg == "--host" && i + 1 < argc) {
      cfg.host = argv[++i];
    } else if (arg == "--port" && i + 1 < argc) {
      cfg.port = static_cast<uint16_t>(std::strtoul(argv[++i], nullptr, 10));
    } else if (arg == "--workers" && i + 1 < argc) {
      cfg.workers = static_cast<int>(std::strtol(argv[++i], nullptr, 10));
    } else if (arg == "--buffer" && i + 1 < argc) {
      cfg.buffer_size = static_cast<size_t>(std::strtoull(argv[++i], nullptr, 10));
    } else if (arg == "--report" && i + 1 < argc) {
      cfg.report_interval_sec = static_cast<int>(std::strtol(argv[++i], nullptr, 10));
    } else if (arg == "--help") {
      return false;
    } else {
      std::cerr << "Unknown argument: " << arg << "\n";
      return false;
    }
  }

  if (cfg.port == 0 || cfg.buffer_size == 0 || cfg.report_interval_sec <= 0) {
    return false;
  }
  return true;
}

void printUsage(const char *prog) {
  std::cout << "Usage: " << prog
            << " [--host 0.0.0.0] [--port 12345] [--workers N] "
               "[--buffer 4096] [--report 1]\n";
}

void reportLoop(const ServerStats *stats, int interval_sec) {
  uint64_t prev_bytes = 0;
  uint64_t prev_accepted = 0;

  while (!g_stop.load(std::memory_order_acquire)) {
    std::this_thread::sleep_for(std::chrono::seconds(interval_sec));

    const uint64_t bytes = stats->bytes_out.load(std::memory_order_relaxed);
    const uint64_t accepted = stats->accepted.load(std::memory_order_relaxed);
    const uint64_t closed = stats->closed.load(std::memory_order_relaxed);
    const uint64_t errors = stats->errors.load(std::memory_order_relaxed);

    const uint64_t delta_bytes = bytes - prev_bytes;
    const uint64_t delta_accepted = accepted - prev_accepted;
    prev_bytes = bytes;
    prev_accepted = accepted;

    const double mib = static_cast<double>(delta_bytes) / (1024.0 * 1024.0);
    std::cout << "[stat] conn(+" << delta_accepted << ", active="
              << (accepted - closed) << ") throughput=" << mib
              << " MiB/s total_out=" << bytes << "B errors=" << errors << "\n";
  }
}

} // namespace

int main(int argc, char **argv) {
  ServerConfig cfg;
  if (!parseArgs(argc, argv, cfg)) {
    printUsage(argv[0]);
    return 1;
  }

  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);

  const int listen_fd = makeListenSocket(cfg.host, cfg.port);
  if (listen_fd < 0) {
    std::cerr << "failed to create listen socket: " << std::strerror(errno)
              << "\n";
    return 1;
  }

  runtime::Scheduler scheduler;
  unsigned int default_workers = std::thread::hardware_concurrency();
  if (default_workers == 0) {
    default_workers = 1;
  }
  const int workers = cfg.workers > 0 ? cfg.workers : static_cast<int>(default_workers);
  scheduler.init(static_cast<size_t>(workers));

  ServerStats stats;
  std::thread reporter(reportLoop, &stats, cfg.report_interval_sec);

  std::cout << "tcpechoserver listening on " << cfg.host << ":" << cfg.port
            << " workers=" << workers
            << " buffer=" << cfg.buffer_size << "\n";

  while (!g_stop.load(std::memory_order_acquire)) {
    pollfd pfd{};
    pfd.fd = listen_fd;
    pfd.events = POLLIN;

    const int polled = ::poll(&pfd, 1, 200);
    if (polled < 0) {
      if (errno == EINTR) {
        continue;
      }
      std::cerr << "poll failed: " << std::strerror(errno) << "\n";
      break;
    }

    if (polled == 0 || (pfd.revents & POLLIN) == 0) {
      continue;
    }

    const int conn_fd = ::accept(listen_fd, nullptr, nullptr);
    if (conn_fd < 0) {
      if (errno == EINTR) {
        continue;
      }
      stats.errors.fetch_add(1, std::memory_order_relaxed);
      continue;
    }

    stats.accepted.fetch_add(1, std::memory_order_relaxed);
    scheduler.submit(echoConnection(conn_fd, cfg.buffer_size, &stats));
  }

  g_stop.store(true, std::memory_order_release);
  ::close(listen_fd);

  if (reporter.joinable()) {
    reporter.join();
  }

  scheduler.stop();

  const uint64_t accepted = stats.accepted.load(std::memory_order_relaxed);
  const uint64_t closed = stats.closed.load(std::memory_order_relaxed);
  const uint64_t bytes_in = stats.bytes_in.load(std::memory_order_relaxed);
  const uint64_t bytes_out = stats.bytes_out.load(std::memory_order_relaxed);
  const uint64_t errors = stats.errors.load(std::memory_order_relaxed);

  std::cout << "shutdown: accepted=" << accepted << " closed=" << closed
            << " bytes_in=" << bytes_in << " bytes_out=" << bytes_out
            << " errors=" << errors << "\n";

  return 0;
}
