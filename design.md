# Scheduler
## init
Scheduler 初始化时会创建 worker_count 个 context, 并传递 pedding_task 和相应的回调, 然后启动所有的 context
```cpp
for (size_t i = 0; i < worker_count; ++i) {
  contexts_.push_back(std::make_unique<Context>(
      i, &pending_tasks_, [this]() noexcept { on_task_completed(); }));
}
for (auto &context : contexts_) {
  context->start();
}
```
然后初始化其他的配置
```cpp
  dispatcher_.init(worker_count);
  pending_tasks_.store(0, std::memory_order_relaxed);
  stop_requested_.store(false, std::memory_order_relaxed);
  accepting_ = true;
  initialized_ = true;
```
dispatcher 是调度分发器
```cpp
class Dispatcher {
public:
  void init(size_t context_count) noexcept {
    context_count_ = context_count == 0 ? 1 : context_count;
    next_.store(0, std::memory_order_relaxed);
  }

  size_t dispatch() noexcept {
    return next_.fetch_add(1, std::memory_order_relaxed) % context_count_;
  }

private:
  size_t context_count_{1};
  std::atomic<size_t> next_{0};
};
```
采用 Round-Robin 策略


## submit
提交一个任务到调度器
```cpp
void Scheduler::submit(Task<void> &&task) {
  auto handle = task.get_handle();
  task.detach();
  submit_new(handle);
}

void Scheduler::submit(std::coroutine_handle<> handle) { submit_new(handle); }
```

## submit_new
如果一个任务是新提交的，会调用 submit_new, submit_new 中会添加 pending_task 的计数，pending_task 为未完成的 task, 用于判断是否可以结束 Scheduler, 并获取目标 context 的 id， 将 handle 提交到 目标 context 中
```cpp
void Scheduler::submit_new(std::coroutine_handle<> handle) {
  if (!handle) {
    return;
  }

  Context *target = nullptr;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!initialized_ || !accepting_) {
      handle.destroy();
      return;
    }
    pending_tasks_.fetch_add(1, std::memory_order_release);
    target = contexts_[dispatcher_.dispatch()].get();
  }

  target->submit_task(handle);
}
```

## loop
Scheduler 初始化后，调用 loop 等待所有 context 的完成, 等待条件为 stop_requested 为 true 或者 pending_task 为 0
```cpp
void Scheduler::loop() {
  std::unique_lock<std::mutex> lock(state_mutex_);
  if (!initialized_) {
    throw std::logic_error("runtime::Scheduler::init must be called before loop");
  }

  completion_cv_.wait(lock, [this]() {
    return stop_requested_.load(std::memory_order_acquire) ||
           pending_tasks_.load(std::memory_order_acquire) == 0;
  });

  accepting_ = false;
  lock.unlock();

  shutdown_contexts();
}
```

## stop
当需要停止 Scheduler 时，调用 stop
```cpp
void Scheduler::stop() {
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!initialized_) {
      return;
    }
    accepting_ = false;
    stop_requested_.store(true, std::memory_order_release);
  }

  completion_cv_.notify_all();
  shutdown_contexts();
}
```

## on_task_completed
task 完成后的回调, 减少 pending_task 的计数
```cpp
void Scheduler::on_task_completed() {
  if (pending_tasks_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    completion_cv_.notify_all();
  }
}
```

## shutdown_context
关闭 context
```cpp
void Scheduler::shutdown_contexts() {
  std::vector<std::unique_ptr<Context>> contexts;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!initialized_) {
      return;
    }
    contexts.swap(contexts_);
    initialized_ = false;
  }

  for (auto &context : contexts) {
    context->notify_stop();
  }
  for (auto &context : contexts) {
    context->join();
  }
}
```

## yiedl awaiter
```cpp
struct YieldAwaiter {
  bool missing_context_{false};

  bool await_ready() const noexcept { return false; }

  template <typename Promise>
  bool await_suspend(std::coroutine_handle<Promise> handle) noexcept {
    auto *ctx = try_local_context();
    if (ctx == nullptr) {
      missing_context_ = true;
      return false;
    }

    ctx->submit_task(handle);
    return true;
  }

  void await_resume() const {
    if (missing_context_) {
      throw std::logic_error("runtime::yield() requires runtime context");
    }
  }
};

inline YieldAwaiter yield() noexcept { return YieldAwaiter{}; }
```

## submit_to_scheduler
获取当前线程的 context, 直接提交到 context, 注意这里是调用的 submit_tracked_task
```cpp
inline void submit_to_scheduler(Task<void> &&task) {
  auto handle = task.get_handle();
  task.detach();

  auto *ctx = try_local_context();
  if (ctx != nullptr) {
    ctx->submit_tracked_task(handle);
    return;
  }

  handle.destroy();
  throw std::logic_error("runtime::submit_to_scheduler() requires runtime context");
}

inline void submit_to_scheduler(std::coroutine_handle<> handle) {
  if (!handle) {
    return;
  }

  auto *ctx = try_local_context();
  if (ctx != nullptr) {
    ctx->submit_tracked_task(handle);
    return;
  }

  handle.destroy();
  throw std::logic_error("runtime::submit_to_scheduler() requires runtime context");
}
```

# context
## 初始化
context 构造时，传入 context_id, pending_tasks 和 task 完成时的回调函数，并初始化任务队列
```cpp
Context::Context(size_t id, std::atomic<size_t> *pending_tasks,
                 OnTaskCompleted on_task_completed)
    : id_(id), pending_tasks_(pending_tasks),
      on_task_completed_(std::move(on_task_completed)) {
  work_batch_.reserve(kProcessBatchSize);
}
```

## start
context 启动时，创建一个线程并运行
```cpp
void Context::start() {
  worker_ = std::jthread([this](std::stop_token token) { run(token); });
}
```

## sumbit
- submit_task 调用 engine_ 提供的 submit_task
- submit_io_waitting 模拟提交一个 io 任务
- submit_tracked_task 增加 pending_task 计数, 并调用 engine_ 提供的 submit_task
```cpp
void Context::submit_task(std::coroutine_handle<> handle) {
  engine_.submit_task(handle);
}

void Context::submit_io_waiting(std::coroutine_handle<> handle) {
  engine_.submit_io_waiting(handle);
}

void Context::submit_io_read(IoReadAwaiter *awaiter) {
  engine_.submit_io_read(awaiter);
}

void Context::submit_io_write(IoWriteAwaiter *awaiter) {
  engine_.submit_io_write(awaiter);
}

void Context::submit_tracked_task(std::coroutine_handle<> handle) {
  if (handle == std::coroutine_handle<>()) {
    return;
  }
  pending_tasks_->fetch_add(1, std::memory_order_release);
  engine_.submit_task(handle);
}
```

## run
由 context 启动时创建的线程运行  
先绑定 local_context_ptr, 然后在一个循环中执行  
process_work_once 每次从 engine 中批量弹出一批任务，然后 resume  
poll_io 调用 engine_ 的接口
```cpp
void Context::run(std::stop_token token) {
  local_context_ptr = this;

  while (true) {
    if (process_work_once()) {
      continue;
    }

    if (poll_io_once()) {
      continue;
    }

    if (engine_.can_stop(token)) {
      break;
    }

    wait_or_idle(token);
  }

  local_context_ptr = nullptr; 
}

bool Context::process_work_once() {
  const auto popped = engine_.pop_batch(work_batch_, kProcessBatchSize);
  if (popped == 0) {
    return false;
  }

  for (auto handle : work_batch_) {
    engine_.on_task_resume_begin();
    handle.resume();
    engine_.on_task_resume_end();

    if (handle.done()) {
      handle.destroy();
      on_task_completed_();
    }
  }

  return true;
}

bool Context::poll_io_once() {
  return engine_.poll_io(kIoPollBatchSize) > 0;
}
```

## wait_or_idle
调用 engine_ 的 wait_for_work_or_stop 接口
```cpp
void Context::wait_or_idle(std::stop_token token) {
  engine_.wait_for_work_or_stop(token);
}
```


# Engine
提交到 context 中的 task 最终会调用 engine 中的 submit 方法  
engine 会将 task 放入  task_queue, io_waitting 放入 io_waitting_queue, io_read 会放入 io_read_submit_queue, io_write 会放入 io_write_queue  
io 相关的 submit 接口在放入相关队列前会先调用 on_io_inflight_begin(), on_io_inflight_begin() 增加了 inflight_io_ 的计数, 然后调用 cv_.notify_one

## submit_task
```cpp
void Engine::submit_task(std::coroutine_handle<> handle) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    task_queue_.push(handle);
  }
  cv_.notify_one();
}

void Engine::submit_io_waiting(std::coroutine_handle<> handle) {
  on_io_inflight_begin();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    io_waiting_queue_.push(handle);
  }
  cv_.notify_one();
}

void Engine::submit_io_read(IoReadAwaiter *awaiter) {
  if (awaiter == nullptr) {
    return;
  }

  on_io_inflight_begin();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    io_read_submit_queue_.push(awaiter);
  }
  cv_.notify_one();
}

void Engine::submit_io_write(IoWriteAwaiter *awaiter) {
  if (awaiter == nullptr) {
    return;
  }

  on_io_inflight_begin();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    io_write_submit_queue_.push(awaiter);
  }
  cv_.notify_one();
}
```

## try_pop_task
从 task_queue 中取出一个 task 后返回
```cpp
std::coroutine_handle<> Engine::try_pop_task() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (task_queue_.empty()) {
    return {};
  }

  auto handle = task_queue_.front();
  task_queue_.pop();
  return handle;
}
```

## pop_batch
批量弹出 task
```cpp
size_t Engine::pop_batch(std::vector<std::coroutine_handle<>> &out,
                         size_t max_n) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (task_queue_.empty() || max_n == 0) {
    return 0;
  }

  out.clear();
  while (out.size() < max_n && !task_queue_.empty()) {
    out.push_back(task_queue_.front());
    task_queue_.pop();
  }

  return out.size();
}
```

## poll_io
1. 处理 io_waiting_queue_（io_suspend_once 的恢复队列）

- 条件：progressed < max_n
- 把等待队列里的 handle 搬到 task_queue_，表示“可以重新执行了”
- 每搬一个：
    - on_io_inflight_end()（对应之前 submit_io_waiting 时加过 inflight）
    - ++progressed

2. 准备读请求到 io_uring SQE

- 用 prepared 计数，最多准备 max_n 个
- 从 io_read_submit_queue_ 取 awaiter，调用 uring_.submit_read(...)
- pending_submit_count_++ 表示“已写入 SQE，但还没真正 submit 到内核”

3. 准备写请求到 io_uring SQE

- 同上，只是换成 submit_write(...)
- 也受 prepared < max_n 限制，读写合计最多准备 max_n

4. 真正提交到内核

- while (pending_submit_count_ > 0) 循环调用 uring_.submit()
- submit() 返回本次实际提交了多少 SQE
- 做健壮性检查后，从 pending_submit_count_ 中扣掉
- 同时 progressed += submitted_size，表示本轮确实推进了工作

5. Io 完成后取出 handle 放入 task_queue


## inflight
增加和减少 inflight_task 和 inflight_io 计数
```cpp
void Engine::on_task_resume_begin() noexcept {
  inflight_tasks_.fetch_add(1, std::memory_order_acq_rel);
}

void Engine::on_task_resume_end() noexcept {
  inflight_tasks_.fetch_sub(1, std::memory_order_acq_rel);
}

void Engine::on_io_inflight_begin(size_t count) noexcept {
  if (count == 0) {
    return;
  }
  inflight_io_.fetch_add(count, std::memory_order_acq_rel);
}

void Engine::on_io_inflight_end(size_t count) noexcept {
  if (count == 0) {
    return;
  }
  inflight_io_.fetch_sub(count, std::memory_order_acq_rel);
}
```

## stop
停止条件为各个队列为空, 并且没有在处理的任务
```cpp
bool Engine::can_stop(std::stop_token token) const {
  if (token.stop_requested() == false) {
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (task_queue_.empty() == false || io_waiting_queue_.empty() == false ||
        io_read_submit_queue_.empty() == false ||
        io_write_submit_queue_.empty() == false || pending_submit_count_ != 0) {
      return false;
    }
  }

  if (inflight_tasks_.load(std::memory_order_acquire) != 0) {
    return false;
  }

  if (inflight_io_.load(std::memory_order_acquire) != 0) {
    return false;
  }

  return true;
}
```

## wait_or_stop
等待 work 或者 停止
```cpp
void Engine::wait_for_work_or_stop(std::stop_token token) {
  std::unique_lock<std::mutex> lock(mutex_);
  cv_.wait(lock, [this, &token]() {
    return token.stop_requested() || task_queue_.empty() == false ||
           io_waiting_queue_.empty() == false ||
           io_read_submit_queue_.empty() == false ||
           io_write_submit_queue_.empty() == false ||
           pending_submit_count_ != 0 ||
           inflight_io_.load(std::memory_order_acquire) != 0;
  });
}
```



# IoAwaiter
IoReadAwaiter 和 IoWriteAwaiter 都继承了 IoOpAwaiterBase
```cpp
class IoOpAwaiterBase {
public:
  bool await_ready() const noexcept { return false; }
  int32_t await_resume() const noexcept { return result_; }

  int fd() const noexcept { return fd_; }
  size_t len() const noexcept { return len_; }
  size_t offset() const noexcept { return offset_; }

  std::coroutine_handle<> handle() const noexcept { return handle_; }
  void set_result(int32_t result) noexcept { result_ = result; }

protected:
  IoOpAwaiterBase(int fd, size_t len, size_t offset)
      : fd_(fd), len_(len), offset_(offset) {}

  void set_handle(std::coroutine_handle<> handle) noexcept { handle_ = handle; }

private:
  int fd_;
  size_t len_;
  size_t offset_;

  int32_t result_{0};
  std::coroutine_handle<> handle_{};
};
```
IoReadAwaiter 和 IoWriteAwaiter 在 suspend 时都会设置 handle, 并通过 Context 提交 Io 操作
```cpp
bool await_suspend(std::coroutine_handle<> handle) noexcept {
  set_handle(handle);

  auto *context = try_local_context();
  if (context == nullptr) {
    set_result(-1);
    return false;
  }

  context->submit_io_read(this);
  return true;
}
```

# AsyncMutex

## Awaiter
Awaiter 持有 AsyncMutex 的指针, 是否挂起统一在 await_suspend 中进行判断  
await_suspend 中对 lock 进行判断，如果为 false, 说明没有上锁，直接加锁，并返回不挂起, 如果为 true，说明已经上锁了，将当前协程放入等待队列中  
await_resume 协程恢复后，返回一个 LockGuard, 即协程恢复后， 重新获取锁

```cpp
  struct Awaiter {
    AsyncMutex *async_mutex_{nullptr};

    explicit Awaiter(AsyncMutex *async_mutex) : async_mutex_(async_mutex) {}

    bool await_ready() noexcept { return false; }

    template <typename T>
    bool await_suspend(std::coroutine_handle<T> handle) noexcept {
      std::lock_guard<std::mutex> lock(async_mutex_->mtx_);

      if (async_mutex_->lock_ == false) {
        async_mutex_->lock_ = true;
        return false;
      }
      async_mutex_->wait_queue_.push(handle);
      return true;
    }

    LockGuard await_resume() noexcept { return LockGuard{async_mutex_}; }
  };
```

## LockGuard
lockguard 持有 async_mutex 的 指针, 内部维护一个 ReLockAwaiter  
ReLockAwaiter 持有 LockGuard 的指针, 是否挂起统一在 await_suspend 中进行判断  
await_suspend 首先判断 async_mutex 的锁是否已经被获取，如果没有，获取锁，不挂起，如果已经被获取了，重新放入等待队列中, 挂起  
LockGuard 在析构时会释放锁，实现 RAII 机制


```cpp
  struct LockGuard {
    AsyncMutex *async_mutex_{nullptr};
    bool owns_lock_{false};

    struct ReLockAwaiter {
      LockGuard *guard_{nullptr};

      explicit ReLockAwaiter(LockGuard *guard) : guard_(guard) {}

      bool await_ready() noexcept { return false; }

      template <typename T>
      bool await_suspend(std::coroutine_handle<T> handle) noexcept {
        std::lock_guard<std::mutex> lock(guard_->async_mutex_->mtx_);

        if (guard_->async_mutex_->lock_ == false) {
          guard_->async_mutex_->lock_ = true;
          return false;
        }
        guard_->async_mutex_->wait_queue_.push(handle);
        return true;
      }

      void await_resume() noexcept { guard_->owns_lock_ = true; }
    };

    LockGuard() = delete;
    explicit LockGuard(AsyncMutex *async_mutex)
        : async_mutex_(async_mutex), owns_lock_(true) {}

    LockGuard(const LockGuard &) = delete;
    LockGuard &operator=(const LockGuard &) = delete;

    LockGuard(LockGuard &&other) noexcept
        : async_mutex_(other.async_mutex_), owns_lock_(other.owns_lock_) {
      other.async_mutex_ = nullptr;
      other.owns_lock_ = false;
    }

    LockGuard &operator=(LockGuard &&other) noexcept {
      if (this == &other) {
        return *this;
      }
      if (owns_lock_ && async_mutex_ != nullptr) {
        async_mutex_->unlock();
      }
      async_mutex_ = other.async_mutex_;
      owns_lock_ = other.owns_lock_;
      other.async_mutex_ = nullptr;
      other.owns_lock_ = false;
      return *this;
    }

    ~LockGuard() {
      if (owns_lock_ && async_mutex_ != nullptr) {
        async_mutex_->unlock();
      }
    }

    void unlock() noexcept {
      if (owns_lock_ && async_mutex_ != nullptr) {
        async_mutex_->unlock();
        owns_lock_ = false;
      }
    }

    ReLockAwaiter relock() noexcept { return ReLockAwaiter{this}; }
  };
```



## lock
返回一个 Awaiter
```cpp
  Awaiter lock() noexcept { return Awaiter{this}; }
```


## unlock
从等待队列中获取一个 handle, 调用 rescheduler_or_resume 提交到 context 中
```cpp
  void unlock() noexcept {
    std::unique_lock<std::mutex> lock(mtx_);
    if (wait_queue_.empty() == false) {
      auto handle = wait_queue_.front();
      wait_queue_.pop();
      lock.unlock();
      runtime::reschedule_or_resume(handle);
    } else {
      lock_ = false;
    }
  }
```

# channel
## SendAWaiter
- await_ready 返回 false, 统一在 await_suspend 中判断
- await_suspend
  - 如果 channel 关闭了， 将 close_ 设置为 true
  - 如果 receiver_queue 不为 空，从 receiver_queue 中获取一个 handle 和 Awaiter，直接设置 Awaiter 的 value 为 value
  - 如果 channel 未满，将 value 放入 buffer 中, 将 suspend_sender 设置为 true， 即不挂起
  - 将当前协程放入等待队列中
  - 如果有 resume_receiver, 直接调度
  - 返回 suspend_sender
- await_resume

```cpp
  struct SenderAwaiter {
    Channel<T> *channel_{nullptr};
    T value_;
    bool close_{false};

    template <typename U>
    requires std::is_constructible_v<T, U &&> SenderAwaiter(Channel<T> *channel,
                                                            U &&value)
        : channel_(channel), value_(std::forward<U>(value)) {}

    bool await_ready() noexcept { return false; }

    template <typename U> bool await_suspend(std::coroutine_handle<U> handle) {
      std::coroutine_handle<> resume_receiver;
      bool suspend_sender = false;

      {
        std::lock_guard<std::mutex> lock(channel_->mtx_);

        if (channel_->close_) {
          close_ = true;
          channel_->stats_add(channel_->stats_closed_failures_, 1);
        } else if (!channel_->receiver_queue_.empty()) {
          // Direct handoff to a waiting receiver; sender should continue.
          auto [receiver_handle, receiver_awaiter] =
              channel_->receiver_queue_.front();
          channel_->receiver_queue_.pop();
          receiver_awaiter->value_ = std::move(value_);
          resume_receiver = receiver_handle;
          channel_->stats_add(channel_->stats_direct_handoffs_, 1);
        } else if (!channel_->isFull()) {
          // Buffered send completes immediately.
          channel_->buffer_[channel_->tail_] = std::move(value_);
          channel_->tail_ = (channel_->tail_ + 1) % channel_->capacity_;
          channel_->count_++;
          channel_->stats_add(channel_->stats_buffer_pushes_, 1);
        } else {
          // Buffer full: park sender until receiver makes progress.
          channel_->sender_queue_.push({handle, this});
          suspend_sender = true;
          channel_->stats_add(channel_->stats_sender_waits_, 1);
        }
      }

      if (resume_receiver) {
        runtime::reschedule_or_resume(resume_receiver);
      }

      return suspend_sender;
    }

    void await_resume() {
      if (close_) {
        throw std::runtime_error("channel is closed");
      }
    }
  };
```


## ReceiverAwaiter
与 SenderAwaiter 类似
- await_ready 返回 false, 统一由 await_suspend 判断
- await_suspend
  - 如果 buffer 有数据，直接读取
    - 如果 sender_queue 不为空，唤醒一个 sender, 让它写入 buffer, 并设置 resume_sender 为 sender
  - 如果 channel close 了，设置 value 为 nullopt
  - 如果 sender_queue 不为 空，直接从一个 sender awaiter 中取数据, 并设置 resume_sender 为 sender
  - 如果无数据，挂起 receiver
  - 如果存在 resume_sender, 直接调度
  - 返回 suspend_receiver
- await_resume

```cpp
  struct ReceiverAwaiter {
    Channel<T> *channel_{nullptr};
    std::optional<T> value_;

    explicit ReceiverAwaiter(Channel<T> *channel) : channel_(channel) {}

    bool await_ready() noexcept { return false; }

    template <typename U>
    bool await_suspend(std::coroutine_handle<U> handle) noexcept {
      std::coroutine_handle<> resume_sender;
      bool suspend_receiver = false;

      {
        std::lock_guard<std::mutex> lock(channel_->mtx_);

        // buffer 有数据，直接读取
        if (!channel_->isEmpty()) {
          value_ = std::move(*channel_->buffer_[channel_->header_]);
          channel_->buffer_[channel_->header_].reset();
          channel_->header_ = (channel_->header_ + 1) % channel_->capacity_;
          channel_->count_--;
          channel_->stats_add(channel_->stats_buffer_pops_, 1);

          // 唤醒一个等待的 sender，让它写入 buffer
          if (!channel_->sender_queue_.empty()) {
            auto [sender_handle, sender_awaiter] =
                channel_->sender_queue_.front();
            channel_->sender_queue_.pop();
            channel_->buffer_[channel_->tail_] =
                std::move(sender_awaiter->value_);
            channel_->tail_ = (channel_->tail_ + 1) % channel_->capacity_;
            channel_->count_++;
            resume_sender = sender_handle;
            channel_->stats_add(channel_->stats_buffer_pushes_, 1);
          }
        } else if (channel_->close_) {
          value_ = std::nullopt;
          channel_->stats_add(channel_->stats_closed_failures_, 1);
        } else if (!channel_->sender_queue_.empty()) {
          // buffer 空，有等待的 sender，直接取数据
          auto [sender_handle, sender_awaiter] =
              channel_->sender_queue_.front();
          channel_->sender_queue_.pop();
          value_ = std::move(sender_awaiter->value_);
          resume_sender = sender_handle;
          channel_->stats_add(channel_->stats_direct_handoffs_, 1);
        } else {
          // 无数据，挂起 receiver
          channel_->receiver_queue_.push({handle, this});
          suspend_receiver = true;
          channel_->stats_add(channel_->stats_receiver_waits_, 1);
        }
      }

      if (resume_sender) {
        runtime::reschedule_or_resume(resume_sender);
      }

      return suspend_receiver;
    }

    T await_resume() {
      if (value_) {
        return std::move(*value_);
      }
      throw std::runtime_error("channel is closed");
    }
  };
```

## send
返回一个 SendAwaiter
```cpp
  template <typename U>
  requires std::is_constructible_v<T, U &&> SenderAwaiter send(U &&value) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (close_) {
      stats_add(stats_closed_failures_, 1);
      throw std::runtime_error("channel is closed");
    }
    return SenderAwaiter{this, std::forward<U>(value)};
  }
```

## receiver
返回 ReceiverAwaiter
```cpp
  ReceiverAwaiter receive() noexcept { return ReceiverAwaiter{this}; }
```

# ConditionVariable
## Awaiter
- await_ready 返回 false, 统一在 await_suspend 中进行判断
- await_suspend 将当前协程放到等待队列中，并调用 guard_->unlock()，挂起
- await_resume
```cpp
  struct Awaiter {
    ConditionVariable *cv_{nullptr};
    AsyncMutex::LockGuard *guard_{nullptr};

    Awaiter() = delete;
    Awaiter(ConditionVariable *cv, AsyncMutex::LockGuard &guard)
        : cv_(cv), guard_(&guard) {}

    bool await_ready() noexcept { return false; }

    template <typename T>
    bool await_suspend(std::coroutine_handle<T> handle) noexcept {
      {
        std::lock_guard<std::mutex> lock(cv_->mtx_);
        cv_->wait_queue_.push(handle);
      }
      guard_->unlock();
      return true;
    }

    void await_resume() noexcept {}
  };
```

## wait
- 带有谓词的 wait 会在 while  循环中对 op 进行判断， 如果条件不满足，挂起，返回后重新获取锁，重新判断
```cpp
  template <typename Callable>
  [[nodiscard]] Task<void> wait(AsyncMutex::LockGuard &guard,
                                Callable op) noexcept {
    while (op() == false) {
      co_await Awaiter{this, guard};
      co_await guard.relock();
    }
  }

  [[nodiscard]] Task<void> wait(AsyncMutex::LockGuard &guard) noexcept {
    co_await Awaiter{this, guard};
    co_await guard.relock();
  }
```

## notify_one
从等待队列中获取一个 handle, 直接调度
```cpp
  void notify_one() noexcept {
    std::unique_lock<std::mutex> lock(mtx_);
    if (wait_queue_.empty() == false) {
      auto handle = wait_queue_.front();
      wait_queue_.pop();
      lock.unlock();
      runtime::reschedule_or_resume(handle);
    }
  }
```

## notify_all
重新调度等待队列中所有的 handle
```cpp
  void notify_all() noexcept {
    std::unique_lock<std::mutex> lock(mtx_);
    std::vector<std::coroutine_handle<>> handles;
    while (wait_queue_.empty() == false) {
      handles.push_back(wait_queue_.front());
      wait_queue_.pop();
    }

    lock.unlock();
    for (auto handle : handles) {
      runtime::reschedule_or_resume(handle);
    }
  }
```

# WaitGroup
## Awaiter
- await_ready 根据 count 进行判断
- await_suspend 再次根据 count 进行判断，然后将当前协程加入到等待队列中
- await_resume
```cpp
  struct Awaiter {
    WaitGroup *wait_group_;
    Awaiter() = delete;
    explicit Awaiter(WaitGroup *wait_group) : wait_group_(wait_group) {}

    bool await_ready() noexcept { return wait_group_->count_ == 0; }

    template <typename T>
    bool await_suspend(std::coroutine_handle<T> handle) noexcept {
      std::lock_guard<std::mutex> lock(wait_group_->mtx);
      if (wait_group_->count_ == 0) {
        return false;
      }
      wait_group_->wait_queue_.push(handle);
      return true;
    }

    void await_resume() noexcept {}
  };
```

## add
直接添加 count 计数
```cpp
  void add(int n) noexcept {
    std::lock_guard<std::mutex> lock(mtx);
    count_ += n;
  }
```

## done
直接减少 count 计数，如果 count 为零，唤醒所有的 handle
```cpp
  void done() {
    std::unique_lock<std::mutex> lock(mtx);
    if (count_ == 0) {
      throw std::runtime_error("count is zero");
    }
    count_ -= 1;
    if (count_ == 0) {
      std::vector<std::coroutine_handle<>> handles;
      while (!wait_queue_.empty()) {
        handles.push_back(wait_queue_.front());
        wait_queue_.pop();
      }

      lock.unlock();
      for (auto handle : handles) {
        runtime::reschedule_or_resume(handle);
      }
    }
  }
```

## wait
返回 AWaiter
```cpp
  Awaiter wait() { return Awaiter{this}; }
```

# when_all
基于 wait_group 实现，协程结束后调用 wg.done()
```cpp
template <typename T>
Task<std::vector<T>> when_all(std::vector<Task<T>> tasks) {
  const size_t n = tasks.size();
  std::shared_ptr<detail::VectorState<T>> state =
      std::make_shared<detail::VectorState<T>>();
  state->tasks = std::move(tasks);
  state->res.resize(n);
  state->wg.add(static_cast<int>(n));

  auto wrapper = [state](size_t i) -> Task<void> {
    try {
      state->res[i].emplace(co_await state->tasks[i]);
    } catch (...) {
      detail::store_first_exception(*state, std::current_exception());
    }
    state->wg.done();
  };

  for (size_t i = 0; i < n; i++) {
    runtime::submit_to_scheduler(wrapper(i));
  }

  co_await state->wg.wait();

  if (state->exception) {
    std::rethrow_exception(state->exception);
  }

  std::vector<T> out;
  out.reserve(n);
  for (auto &v : state->res) {
    out.push_back(std::move(*v));
  }

  co_return out;
}

template <typename... Ts> Task<std::tuple<Ts...>> when_all(Task<Ts>... tasks) {
  constexpr int n = static_cast<int>(sizeof...(tasks));
  std::shared_ptr<detail::TupleState<Ts...>> state =
      std::make_shared<detail::TupleState<Ts...>>();
  state->tasks = std::make_tuple(std::move(tasks)...);
  state->wg.add(n);

  return when_all_impl(state, std::make_index_sequence<sizeof...(Ts)>{});
}
```