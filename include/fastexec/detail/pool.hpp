#ifndef __FASTSTDEXEC_DETAIL_POOL_HPP
#define __FASTSTDEXEC_DETAIL_POOL_HPP

#include <cstddef>
#include <functional>
#include <future>
#include <latch>
#include <memory>
#include <thread>

#include "taskgroup.hpp"
#include "worker.hpp"
namespace fastexec::detail {
// 线程局部存储，当前任务所属的任务组指针
static inline thread_local std::shared_ptr<TaskGroup> t_current_task_group{
    nullptr};
// 线程池类，管理工作线程和任务分发
class thread_pool : public util::Singleton<thread_pool> {
  friend class util::Singleton<thread_pool>;

 public:
  // 析构函数，停止线程池并等待所有线程完成
  ~thread_pool() {
    if (!_shared.get_global_queue().closed()) {
      close();
    }
    for (auto& thread : _threads) {
      if (thread.joinable()) {
        thread.join();
      }
    }
  }

  void close() { _shared.global_queue_close(); }

  // 等待所有任务完成
  void wait_for_all() {
    // 等待所有 Worker 线程完成任务
    for (auto& worker : _threads) {
      worker.join();
    }
  }

 public:
  // 提交任务到线程池
  template <typename F, typename... Args>
  std::future<std::invoke_result_t<F, Args...>> submit(F&& f, Args&&... args) {
    // 获取任务的返回类型
    using return_type = std::invoke_result_t<F, Args...>;

    // 1. 捕获当前上下文：检查当前线程是否隶属于某个 TaskGroup,sptr计数器+1
    auto current_group = t_current_task_group;

    if (current_group) {
      // 如果属于某个组，该组的活跃任务数 +1
      current_group->increment();
    }
    // 使用智能指针创建 packaged_task 包装器，用于存储任务和其返回值的 future
    // 注意：我们将 current_group 捕获到了 lambda
    // 中（值传递，增加引用计数）sptr计数器+1
    auto task_ptr = std::make_shared<std::packaged_task<return_type()>>(
        [func = std::forward<F>(f), ... args = std::forward<Args>(args),
         group = current_group]() -> return_type {
          // 这是一个 RAII 辅助类，用于在任务执行期间临时设置 TLS
          struct ContextGuard {
            std::shared_ptr<TaskGroup> _group;
            std::shared_ptr<TaskGroup> _prev_group;
            explicit ContextGuard(std::shared_ptr<TaskGroup> g) : _group(g) {
              // 保存之前的上下文（虽然通常 Worker
              // 线程之前是空的，但为了健壮性）
              _prev_group = t_current_task_group;

              // 设置当前任务的上下文
              t_current_task_group = _group;
            }
            ~ContextGuard() {
              // 恢复之前的上下文
              t_current_task_group = _prev_group;

              // 任务结束，计数器 -1
              if (_group) _group->decrement();
            }
          };

          // 2. 恢复上下文：在任务开始执行前，设置 TLS
          ContextGuard guard(group);

          // 执行用户实际的函数
          return func(args...);
        });

    // 获取 future 对象，用于后续获取任务执行结果
    auto fut = task_ptr->get_future();
    // 创建一个函数对象，用于将任务包装成 void() 类型，方便 Worker 执行
    auto job = std::function<void()>([task_ptr]() { (*task_ptr)(); });

    // 检查当前线程是否是 Worker 线程
    if (t_worker != nullptr) {
      // 如果是 Worker 线程，直接加入到自己的本地队列
      t_worker->push_back_task_to_local(std::move(job),
                                        _shared.get_global_queue());
    } else {
      // 外部线程，加入到全局队列
      t_shared->push_back_task_to_global(std::move(job));
    }
    return fut;
  }

 private:
  // 构造函数，创建线程池并初始化工作者
  explicit thread_pool() noexcept {
    // 启动工作线程
    work();
  }

  // 工作函数，创建线程并运行工作者
  void work() {
    for (std::size_t i = 0; i < _thread_num; ++i) {
      _threads.emplace_back([this, i]() {
        detail::Worker worker{&_shared, i};
        // 等待所有的worker全部创建完成(shared内的worker数组完整注册好)
        sync_start.arrive_and_wait();
        // 统一启动run
        worker.run();
      });
    }
    // 等待所有线程启动完成，此函数才执行完成
    sync_start.arrive_and_wait();
  }

  bool task_complete() {
    auto workers = _shared.get_workers();
    for (auto w : workers) {
      if (w->is_worker_has_task() || !_shared.is_global_queue_empty()) {
        return false;
      }
    }
    return true;
  }

 private:
  std::size_t _thread_num{std::thread::hardware_concurrency()};  // 线程数
  std::vector<std::jthread> _threads{};                          // 线程池
  Shared _shared{_thread_num};                                   // 共享状态
  std::atomic<std::size_t> _rr_index{0};                         // 轮询索引
  std::latch sync_start{
      static_cast<std::ptrdiff_t>(_thread_num + 1)};  // 同步标志
};

}  // namespace fastexec::detail

#endif
