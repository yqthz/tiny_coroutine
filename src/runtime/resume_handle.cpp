#include "runtime/resume_handle.h"

#include "runtime/context.h"

namespace tiny_coroutine::runtime {

void reschedule_or_resume(std::coroutine_handle<> handle) noexcept {
  if (!handle) {
    return;
  }

  if (auto *ctx = try_local_context(); ctx != nullptr) {
    ctx->submit_task(handle);
    return;
  }

  handle.resume();
}

} // namespace tiny_coroutine::runtime
