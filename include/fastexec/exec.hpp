#ifndef __FASTEXEC_EXEC_HPP
#define __FASTEXEC_EXEC_HPP
#include <tuple>
#include <variant>

#include "detail/pool.hpp"

// 内部创建线程池实例
namespace fastexec::__inner {
inline auto &_fastexec_inner_thread_pool =
    fastexec::detail::thread_pool::instance();

namespace detail {
// 定义一个类型特征元函数
template <typename T> struct future_result {
  using type = T;
};
// void特化版本
template <> struct future_result<void> {
  using type = std::monostate;
};
// 重命名类型，进行类型提取
template <typename T> using future_result_t = typename future_result<T>::type;
// 等待future的值
template <typename T>
future_result_t<T> get_future_value(fastexec::future<T> &f) {
  if constexpr (std::is_void_v<T>) {
    f.get();
    return std::monostate{};
  } else {
    return f.get();
  }
}
} // namespace detail
} // namespace fastexec::__inner

// 外部接口
namespace fastexec {
// 非阻塞创建异步任务，返回future
template <typename F, typename... Args> auto spawn(F &&f, Args &&...args) {
  return __inner::_fastexec_inner_thread_pool.submit(
      std::forward<F>(f), std::forward<Args>(args)...);
}

// 可窃取性是提交时的任务属性。false 适合依赖 worker 本地状态的任务。
template <typename F, typename... Args>
auto spawn_with_options(bool stealable, F &&f, Args &&...args) {
  return __inner::_fastexec_inner_thread_pool.submit_with_options(
      stealable, std::forward<F>(f), std::forward<Args>(args)...);
}

// 主动关闭线程池并且等待线程回收
inline void close_and_join() {
  __inner::_fastexec_inner_thread_pool.close();
  __inner::_fastexec_inner_thread_pool.wait_for_all();
}

// 阻塞等待多个任务，返回 tuple
template <typename... Ts>
std::tuple<__inner::detail::future_result_t<Ts>...>
wait(fastexec::future<Ts>... futures) {
  return std::make_tuple(__inner::detail::get_future_value(futures)...);
}

// 阻塞一个任务，等待他及其所有子任务完成
template <typename F, typename... Args>
static inline decltype(auto) block_on(F &&f, Args &&...args) {
  using result_type =
      std::invoke_result_t<std::decay_t<F> &, std::decay_t<Args>...>;
  auto &pool = __inner::_fastexec_inner_thread_pool;
  // 只有 worker
  // 内等待才需要在组归零时通知线程池；外部等待直接使用组的原子通知。
  std::function<void()> on_zero;
  if (detail::t_worker)
    on_zero = [&pool] { pool.notify_waiters(); };
  auto group = std::make_shared<detail::TaskGroup>(std::move(on_zero));

  // TLS 恢复由 RAII 负责；提交失败、分配失败也不会把任务组留在当前线程。
  auto root = [&]() {
    struct GroupScope {
      detail::TaskGroup *previous;
      explicit GroupScope(detail::TaskGroup *group)
          : previous(detail::t_current_task_group) {
        detail::t_current_task_group = group;
      }
      ~GroupScope() { detail::t_current_task_group = previous; }
    } scope(group.get());
    return pool.submit(std::forward<F>(f), std::forward<Args>(args)...);
  }();

  // 外部线程阻塞等待；worker 则执行本地、全局或窃取来的任务，防止嵌套等待死锁。
  pool.help_until(*group);
  // 先读取根任务结果，再检查子任务异常；根任务失败时优先传播它的异常。
  if constexpr (std::is_void_v<result_type>) {
    root.get();
    group->rethrow_first_error();
    return;
  } else if constexpr (std::is_reference_v<result_type>) {
    auto &result = root.get();
    group->rethrow_first_error();
    return result;
  } else {
    auto result = root.get();
    group->rethrow_first_error();
    return result;
  }
}
} // namespace fastexec

#endif
