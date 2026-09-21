#ifndef FASTEXEC_FUTURE_HPP
#define FASTEXEC_FUTURE_HPP

#include <atomic>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace fastexec {

class future_error : public std::logic_error {
 public:
  explicit future_error(const char* message) : std::logic_error(message) {}
};

namespace detail {

// worker 等待线程池任务时，通过线程局部钩子继续执行队列中的任务。
inline thread_local void* future_help_context{nullptr};
inline thread_local void (*future_help)(void*, const std::atomic<bool>&){nullptr};

// void 用 bool 占位；引用保存地址；普通结果直接保存在共享状态中。
template <class T>
using future_storage_t = std::conditional_t<
    std::is_void_v<T>, bool,
    std::conditional_t<std::is_reference_v<T>, std::reference_wrapper<std::remove_reference_t<T>>, T>>;

// 生产者先写入结果或异常，再以 release 发布 ready。
// 消费者以 acquire 读取 ready 后，便能安全读取其余非原子字段。
template <class T>
struct future_state {
  std::atomic<bool> ready{false};                     // 结果是否已发布
  std::atomic<bool> future_taken{false};              // future 是否已被领取
  std::atomic<bool> worker_waiting{false};            // 是否有 worker 正在协作等待
  std::optional<future_storage_t<T>> value;           // 成功结果
  std::exception_ptr error;                           // 任务异常
  bool broken{false};                                 // promise 未履约就销毁
  void* completion_context{nullptr};                  // 线程池唤醒回调的上下文
  void (*completion_hook)(void*) noexcept {nullptr};  // 线程池唤醒回调

  void publish() noexcept {
    ready.store(true, std::memory_order_release);
    ready.notify_all();

    // 只有 worker 正在等待这个 future 时，才唤醒线程池的工作循环。
    if (completion_hook && worker_waiting.load(std::memory_order_acquire)) {
      completion_hook(completion_context);
    }
  }
};

}

template <class T>
class promise;

template <class T>
class future {
  friend class promise<T>;

  explicit future(std::shared_ptr<detail::future_state<T>> state) : state_(std::move(state)) {}

 public:
  future() = default;
  future(future&&) noexcept = default;
  future& operator=(future&&) noexcept = default;
  future(const future&) = delete;
  future& operator=(const future&) = delete;

  [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(state_); }

  void wait() const {
    if (!state_) throw future_error("future has no state");

    if (detail::future_help && state_->completion_hook) {
      // 先登记等待，再检查 ready，避免完成通知发生在登记之前。
      state_->worker_waiting.store(true, std::memory_order_release);
      detail::future_help(detail::future_help_context, state_->ready);
      return;
    }

    // 外部线程直接等待；循环用于处理虚假唤醒。
    while (!state_->ready.load(std::memory_order_acquire)) {
      state_->ready.wait(false, std::memory_order_acquire);
    }
  }

  T get() {
    wait();
    // get 消耗 future；即使后续抛异常，也不允许再次读取。
    auto state = std::move(state_);
    if (state->broken) throw future_error("broken promise");
    if (state->error) std::rethrow_exception(state->error);

    if constexpr (std::is_void_v<T>) {
      return;
    } else if constexpr (std::is_reference_v<T>) {
      return state->value->get();
    } else {
      return std::move(*state->value);
    }
  }

 private:
  std::shared_ptr<detail::future_state<T>> state_;  // 与生产者共享结果状态
};

template <class T>
class promise {
 public:
  promise() : state_(std::make_shared<detail::future_state<T>>()) {}
  promise(promise&&) noexcept = default;
  promise& operator=(promise&& other) noexcept {
    if (this != &other) {
      abandon();
      state_ = std::move(other.state_);
    }
    return *this;
  }
  promise(const promise&) = delete;
  promise& operator=(const promise&) = delete;
  ~promise() { abandon(); }

  future<T> get_future() {
    check_state();
    if (state_->future_taken.exchange(true, std::memory_order_acq_rel)) {
      throw future_error("future already retrieved");
    }
    return future<T>(state_);
  }

  // 线程池在任务发布前设置回调；普通 promise 不需要设置。
  void set_completion_hook(void* context, void (*hook)(void*) noexcept) noexcept {
    state_->completion_context = context;
    state_->completion_hook = hook;
  }

  // packaged_task 完成后读取异常，不消耗对应的 future。
  [[nodiscard]] std::exception_ptr exception() const noexcept { return state_->error; }

  template <class U = T>
    requires(!std::is_void_v<T> && !std::is_reference_v<T> && std::is_constructible_v<T, U &&>)
  void set_value(U&& value) {
    check_unready();
    state_->value.emplace(std::forward<U>(value));
    state_->publish();
  }

  void set_value()
    requires std::is_void_v<T>
  {
    check_unready();
    state_->value.emplace(true);
    state_->publish();
  }

  template <class U = T>
  void set_value(std::remove_reference_t<U>& value)
    requires std::is_reference_v<T>
  {
    check_unready();
    state_->value.emplace(value);
    state_->publish();
  }

  void set_exception(std::exception_ptr error) {
    check_unready();
    if (!error) throw std::invalid_argument("null exception");
    state_->error = std::move(error);
    state_->publish();
  }

 private:
  void check_state() const {
    if (!state_) throw future_error("promise has no state");
  }

  void check_unready() const {
    check_state();
    if (state_->ready.load(std::memory_order_acquire)) {
      throw future_error("promise already satisfied");
    }
  }

  void abandon() noexcept {
    if (state_ && !state_->ready.load(std::memory_order_acquire) &&
        state_->future_taken.load(std::memory_order_acquire)) {
      // 析构函数不能分配异常对象；由 future::get 在调用方构造错误。
      state_->broken = true;
      state_->publish();
    }
  }

  std::shared_ptr<detail::future_state<T>> state_;  // 唯一生产者持有的共享状态
};

template <class Signature>
class packaged_task;

template <class R, class... Args>
class packaged_task<R(Args...)> {
  static constexpr std::size_t inline_size = 64;                  // 小对象缓冲区大小
  alignas(std::max_align_t) unsigned char storage_[inline_size];  // 原位存储 callable
  void* object_{nullptr};                                         // 当前 callable 的地址
  R (*invoke_)(void*, Args&&...){nullptr};                        // 类型擦除的调用入口
  void (*destroy_)(void*) noexcept {nullptr};                     // callable 销毁入口
  void (*move_)(void*, void*) noexcept {nullptr};                 // 原位对象的移动入口
  bool heap_{false};                                              // 是否使用堆存储

 public:
  template <class F>
    requires(!std::is_same_v<std::remove_cvref_t<F>, packaged_task>)
  explicit packaged_task(F&& f) {
    using Fn = std::decay_t<F>;
    constexpr bool local = sizeof(Fn) <= inline_size && alignof(Fn) <= alignof(std::max_align_t) &&
                           std::is_nothrow_move_constructible_v<Fn>;
    if constexpr (local) {
      object_ = storage_;
      new (object_) Fn(std::forward<F>(f));
      move_ = [](void* dst, void* src) noexcept {
        new (dst) Fn(std::move(*static_cast<Fn*>(src)));
        static_cast<Fn*>(src)->~Fn();
      };
    } else {
      object_ = new Fn(std::forward<F>(f));
    }

    heap_ = !local;
    invoke_ = [](void* ptr, Args&&... args) -> R {
      return std::invoke(*static_cast<Fn*>(ptr), std::forward<Args>(args)...);
    };
    if constexpr (local) {
      destroy_ = [](void* ptr) noexcept { static_cast<Fn*>(ptr)->~Fn(); };
    } else {
      // 使用与 new Fn 配对的 delete，兼容过度对齐的 Fn。
      destroy_ = [](void* ptr) noexcept { delete static_cast<Fn*>(ptr); };
    }
  }

  packaged_task(packaged_task&& other) noexcept
      : invoke_(other.invoke_),
        destroy_(other.destroy_),
        move_(other.move_),
        heap_(other.heap_),
        result_(std::move(other.result_)) {
    if (other.object_) {
      if (heap_) {
        object_ = std::exchange(other.object_, nullptr);
      } else {
        object_ = storage_;
        move_(object_, other.object_);
        other.object_ = nullptr;
      }
    }
    if (other.invoked_.test(std::memory_order_relaxed)) {
      invoked_.test_and_set(std::memory_order_relaxed);
    }
  }

  packaged_task& operator=(packaged_task&& other) noexcept {
    if (this != &other) {
      this->~packaged_task();
      new (this) packaged_task(std::move(other));
    }
    return *this;
  }
  packaged_task(const packaged_task&) = delete;
  packaged_task& operator=(const packaged_task&) = delete;

  ~packaged_task() {
    if (object_) destroy_(object_);
  }

  [[nodiscard]] bool valid() const noexcept { return object_ != nullptr; }
  future<R> get_future() { return result_.get_future(); }

  void set_completion_hook(void* context, void (*hook)(void*) noexcept) noexcept {
    result_.set_completion_hook(context, hook);
  }

  [[nodiscard]] std::exception_ptr exception() const noexcept { return result_.exception(); }

  void operator()(Args... args) {
    if (!object_) throw future_error("packaged_task has no callable");
    if (invoked_.test_and_set(std::memory_order_acq_rel)) {
      throw future_error("packaged_task already invoked");
    }

    try {
      if constexpr (std::is_void_v<R>) {
        invoke_(object_, std::forward<Args>(args)...);
        result_.set_value();
      } else {
        result_.set_value(invoke_(object_, std::forward<Args>(args)...));
      }
    } catch (...) {
      // 用户函数及结果构造的异常都交给 future，不穿过 worker 循环。
      result_.set_exception(std::current_exception());
    }
  }

 private:
  promise<R> result_;                            // 任务的结果生产者
  std::atomic_flag invoked_ = ATOMIC_FLAG_INIT;  // 保证任务只执行一次
};

}
#endif
