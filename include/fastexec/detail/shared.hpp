#ifndef __FASTSTDEXEC_DETAIL_SHARED_HPP
#define __FASTSTDEXEC_DETAIL_SHARED_HPP

#include <latch>
#include <span>
#include <vector>

#include "queue.hpp"
namespace fastexec::detail {
class Worker;
class Shared;

// 线程局部存储，当前共享类指针
static inline thread_local Shared* t_shared{nullptr};

class Shared : util::noncopyable {
  friend class Worker;

 public:
  explicit Shared(std::size_t worker_count) : _stop_latch(worker_count) {
    assert(t_shared == nullptr);
    t_shared = this;
    _workers.reserve(worker_count);
    _workers.resize(worker_count);
  }

  ~Shared() { t_shared = nullptr; }

 public:
  // 注册 worker
  void register_worker(int worker_id, Worker* worker) {
    _workers[worker_id] = worker;
  }

  // 获取所有注册的 worker
  [[nodiscard]]
  std::span<Worker*> get_workers() {
    return _workers;
  }

  // 获取注册的 worker 总数
  [[nodiscard]]
  std::size_t total_worker_count() const {
    return _workers.size();
  }

  // 全局队列关闭
  void global_queue_close() { _global_queue.close(); }

  // 获取全局任务队列中的下一个任务
  std::optional<std::function<void()>> get_next_global_task() {
    return _global_queue.try_pop();
  }

  // 获取全局任务队列中的多个任务
  std::optional<std::vector<std::function<void()>>> get_batch_global_tasks(
      std::size_t batch_size) {
    return _global_queue.try_pop_batch(batch_size);
  }

  // 判断全局任务队列是否为空
  bool is_global_queue_empty() { return _global_queue.empty(); }

  // 将任务添加到全局任务队列的末尾
  void push_back_task_to_global(std::function<void()> task) {
    _global_queue.push_back(std::move(task));
  }
  // 将多个任务添加到全局任务队列的末尾
  void push_back_batch_task_to_global(
      std::vector<std::function<void()>> tasks) {
    _global_queue.push_back_batch(tasks);
  }
  // 获取全局任务队列
  GlobalQueue& get_global_queue() { return _global_queue; }

  // 增加窃取任务的 worker 数量
  void increment_steal_worker_count() {
    _steal_worker_count.fetch_add(1, std::memory_order::release);
  }
  // 减少窃取任务的 worker 数量
  void decrement_steal_worker_count() {
    _steal_worker_count.fetch_sub(1, std::memory_order::release);
  }

  // 判断是否可以窃取任务
  bool can_steal_task() const {
    return _steal_worker_count.load(std::memory_order::acquire) <
           (_workers.size() / 2);
  }

 private:
  std::vector<Worker*> _workers{};                  // 所有注册的 worker
  GlobalQueue _global_queue{};                      // 全局任务队列
  std::atomic<std::size_t> _steal_worker_count{0};  // 窃取任务的 worker 数量
  std::latch _stop_latch;  // 等待所有 Worker 线程完成任务
};
}  // namespace fastexec::detail

#endif
