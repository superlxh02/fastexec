#ifndef __FASTSTDEXEC_DETAIL_POOL_HPP
#define __FASTSTDEXEC_DETAIL_POOL_HPP

#include "fastexec/future.hpp"
#include <cstddef>
#include <functional>
#include <latch>
#include <memory>
#include <thread>

#include "taskgroup.hpp"
#include "worker.hpp"
namespace fastexec::detail {
// 线程局部存储，当前任务所属的任务组指针
inline thread_local TaskGroup *t_current_task_group{nullptr};
// 线程池类，管理工作线程和任务分发
class thread_pool : public util::Singleton<thread_pool> {
  friend class util::Singleton<thread_pool>;

public:
  // 析构函数，停止线程池并等待所有线程完成
  ~thread_pool() {
    if (!_shared.get_global_queue().closed()) {
      close();
    }
    for (auto &thread : _threads) {
      if (thread.joinable()) {
        thread.join();
      }
    }
  }

  void close() { _shared.global_queue_close(); }

  // block_on 从 worker 内调用时使用；继续处理任务直至该组完成。
  void help_until(TaskGroup &group) {
    if (t_worker)
      t_worker->help_until(group);
    else
      group.wait();
  }

  void notify_waiters() { _shared.signal_work(true); }

  // 等待所有任务完成
  void wait_for_all() {
    // 等待所有 Worker 线程完成任务
    for (auto &worker : _threads) {
      if (worker.joinable())
        worker.join();
    }
  }

public:
  // 提交任务到线程池
  template <typename F, typename... Args> auto submit(F &&f, Args &&...args) {
    return submit_with_options(true, std::forward<F>(f),
                               std::forward<Args>(args)...);
  }

  // stealable=false 使 worker 本地任务进入私有队列，不会被其他 worker 窃取。
  template <typename F, typename... Args>
  auto submit_with_options(bool stealable, F &&f, Args &&...args) {
    // 获取任务的返回类型
    using return_type =
        std::invoke_result_t<std::decay_t<F> &, std::decay_t<Args>...>;

    // 当前任务可能属于 block_on：取得该任务组的所有权，保证排队期间仍有效。
    auto current_group = t_current_task_group
                             ? t_current_task_group->shared_from_this()
                             : std::shared_ptr<TaskGroup>{};

    // lambda 只捕获任务组的非拥有指针；job 持有 shared_ptr 管理生命周期。
    auto task_ptr = std::make_shared<fastexec::packaged_task<return_type()>>(
        [func = std::forward<F>(f), ... args = std::forward<Args>(args),
         group = current_group.get()]() -> return_type {
          // 这是一个 RAII 辅助类，用于在任务执行期间临时设置 TLS
          struct ContextGuard {
            TaskGroup *_prev_group;
            explicit ContextGuard(TaskGroup *g)
                : _prev_group(t_current_task_group) {
              // 保存旧上下文并设置当前任务组；嵌套等待后可恢复父任务组。
              t_current_task_group = g;
            }
            ~ContextGuard() {
              // 恢复之前的上下文
              t_current_task_group = _prev_group;
            }
          };

          // 执行用户函数期间，子任务从 TLS 继承该组。
          ContextGuard guard(group);

          // 执行用户实际的函数
          return std::invoke(func, std::move(args)...);
        });
    // 若 worker 正在等待该 future，完成时唤醒其协作执行循环。
    task_ptr->set_completion_hook(&_shared, [](void *shared) noexcept {
      static_cast<Shared *>(shared)->signal_work(true);
    });

    // 获取 future 对象，用于后续获取任务执行结果
    auto fut = task_ptr->get_future();
    // 创建一个函数对象，用于将任务包装成 void() 类型，方便 Worker 执行
    auto job = std::function<void()>(
        [this, task_ptr = std::move(task_ptr), current_group]() mutable {
          try {
            (*task_ptr)();
            // packaged_task 已完成共享状态；即使调用方丢弃子任务 future，
            // 任务组仍能收到用户异常或结果构造异常。
            if (current_group) {
              if (auto error = task_ptr->exception())
                current_group->record_exception(std::move(error));
            }
          } catch (...) {
            // 保护 worker 循环；正常的用户异常已由 packaged_task 捕获。
            if (current_group)
              current_group->record_exception(std::current_exception());
          }
          // 先销毁 callable 与它捕获的资源，再发布任务组完成。
          task_ptr.reset();
          _shared.task_finished();
          if (current_group)
            current_group->decrement();
        });
    // 构造包装器可能抛异常，计数只在包装完成后增加。
    if (current_group)
      current_group->increment();
    _shared.task_added();

    // 检查当前线程是否是 Worker 线程
    try {
      if (t_worker != nullptr) {
        // 如果是 Worker 线程，直接加入到自己的本地队列
        t_worker->push_back_task_to_local(Task{std::move(job), stealable},
                                          _shared.get_global_queue());
      } else {
        // 外部线程，加入到全局队列
        _shared.push_back_task_to_global(Task{std::move(job), stealable});
      }
    } catch (...) {
      _shared.task_finished();
      if (current_group)
        current_group->decrement();
      throw;
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

private:
  std::size_t _thread_num{
      std::max(1u, std::thread::hardware_concurrency())}; // 线程数
  std::vector<std::jthread> _threads{};                   // 线程池
  Shared _shared{_thread_num};                            // 共享状态
  std::latch sync_start{
      static_cast<std::ptrdiff_t>(_thread_num + 1)}; // 同步标志
};

} // namespace fastexec::detail

#endif
