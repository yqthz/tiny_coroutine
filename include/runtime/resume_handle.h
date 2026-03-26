#pragma once

#include <coroutine>

namespace tiny_coroutine::runtime {

void reschedule_or_resume(std::coroutine_handle<> handle) noexcept;

} // namespace tiny_coroutine::runtime
