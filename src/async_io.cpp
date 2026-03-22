#include "../include/async_io.h"
#include "../include/io_context.h"

namespace tiny_coroutine {

AsyncIoBase::AsyncIoBase(IoContext &ctx, int fd, size_t len, size_t offset)
    : ctx_(ctx), fd_(fd), len_(len), offset_(offset) {}

bool AsyncIoBase::await_ready() { return false; }

int32_t AsyncIoBase::await_resume() { return result_; }

void AsyncIoBase::set_result(int32_t result) { result_ = result; }

AsyncRead::AsyncRead(IoContext &ctx, int fd, void *buf, size_t len,
                     size_t offset)
    : AsyncIoBase(ctx, fd, len, offset), buf_(buf) {}

AsyncWrite::AsyncWrite(IoContext &ctx, int fd, const void *buf, size_t len,
                       size_t offset)
    : AsyncIoBase(ctx, fd, len, offset), buf_(buf) {}

} // namespace tiny_coroutine