#ifndef __FASTEXEC_EXEC_HPP
#define __FASTEXEC_EXEC_HPP
#include <tuple>

#include "detail/pool.hpp"

// 内部创建线程池实例
namespace fastexec::__inner {
inline auto& _fastexec_inner_thread_pool =
    fastexec::detail::thread_pool::instance();

namespace detail {
// 定义一个类型特征元函数
template <typename T>
struct future_result {
  using type = T;
};
// void特化版本
template <>
struct future_result<void> {
  using type = std::monostate;
};
// 重命名类型，进行类型提取
template <typename T>
using future_result_t = typename future_result<T>::type;
// 等待future的值
template <typename T>
future_result_t<T> get_future_value(std::future<T>& f) {
  if constexpr (std::is_void_v<T>) {
    f.get();
    return std::monostate{};
  } else {
    return f.get();
  }
}
}  // namespace detail
}  // namespace fastexec::__inner

// 外部接口
namespace fastexec {
// 非阻塞创建异步任务，返回future
template <typename F, typename... Args>
std::future<std::invoke_result_t<F, Args...>> spawn(F&& f, Args&&... args) {
  return __inner::_fastexec_inner_thread_pool.submit(
      std::forward<F>(f), std::forward<Args>(args)...);
}

// 主动关闭线程池并且等待线程回收
inline void close_and_join() {
  __inner::_fastexec_inner_thread_pool.close();
  __inner::_fastexec_inner_thread_pool.wait_for_all();
}

// 阻塞等待多个任务，返回 tuple
template <typename... Ts>
std::tuple<__inner::detail::future_result_t<Ts>...> wait(
    std::future<Ts>... futures) {
  return std::make_tuple(__inner::detail::get_future_value(futures)...);
}

// 阻塞一个任务，等待他及其所有子任务完成
template <typename F, typename... Args>
static inline void block_on(F&& f, Args&&... args) {
  // 1. 创建一个新的任务组记分牌
  auto group = std::make_shared<detail::TaskGroup>();

  {
    // 2. 设置当前线程的 TLS 上下文
    // 这样做的目的是：当我们紧接着调用 submit 时，submit 能够看到这个
    // group，从而将第一个任务关联到这个 group 中。
    auto prev_group = detail::t_current_task_group;
    detail::t_current_task_group = group;

    // 3. 提交任务
    // submit 内部会检测到 t_current_task_group 不为空，执行
    // group->increment()，并将 group 打包进任务闭包。
    __inner::_fastexec_inner_thread_pool.submit(std::forward<F>(f),
                                                std::forward<Args>(args)...);
    // 4. 恢复上下文
    // 避免影响后续在本线程提交的其他无关任务
    detail::t_current_task_group = prev_group;
  }

  // 5. 阻塞等待记分牌归零
  // 此时主线程会在这里挂起，直到所有关联了该
  // group的任务（包括子任务）全部执行完毕。
  group->wait();
}
}  // namespace fastexec

#endif
