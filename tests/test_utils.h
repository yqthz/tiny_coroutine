#pragma once

#include <chrono>
#include <functional>
#include <thread>

inline bool wait_until(const std::function<bool()> &predicate,
                       std::chrono::milliseconds timeout =
                           std::chrono::milliseconds(200)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return predicate();
}
