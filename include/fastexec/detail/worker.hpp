#ifndef __FASTSTDEXEC_DETAIL_WORKER_HPP
#define __FASTSTDEXEC_DETAIL_WORKER_HPP

#include <chrono>
#include <thread>
#include <vector>

#include "queue.hpp"
#include "shared.hpp"
namespace fastexec::detail {
class Worker;
// 线程局部存储，当前worker指针
static inline thread_local Worker* t_worker{nullptr};
class Worker {
  friend class Shared;

 public:
  Worker(Shared* shared, std::size_t worker_id)
      : _shared(shared), _worker_id(worker_id) {
    // 将自己注册到共享类中
    _shared->register_worker(worker_id, this);
    t_worker = this;
  }

  ~Worker() {
    t_worker = nullptr;
    t_shared = nullptr;
    _shared->_stop_latch.arrive_and_wait();
  }

 public:
  // worker运行函数
  void run() {
    while (true) {
      // 循环退出条件是：线程池停止且本地队列和全局队列都为空
      std::optional<std::function<void()>> task;
      // 从队列获取任务
      task = std::move(get_next_task());
      if (task.has_value()) {
        (*task)();
        continue;
      }
      //  从其他worker的队列窃取任务
      task = std::move(task_steal());
      if (task.has_value()) {
        (*task)();
        continue;
      }
      std::this_thread::sleep_for(std::chrono::microseconds(100));
      // std::this_thread::sleep_for(std::chrono::milliseconds(100));//用于valgrind检查
      _shutdown = _shared->get_global_queue().closed();
      if (quit_condition(_shutdown)) {
        break;
      }
    }
  }

 public:
  // 检查本地队列是否为空
  bool is_local_queue_empty() { return _local_queue.empty(); }

  // 获取本地队列总大小
  std::size_t get_local_queue_size() { return _local_queue.size(); }

  // 向本地队列推送任务，处理溢出
  bool push_back_task_to_local(std::function<void()> task,
                               GlobalQueue& global_queue) {
    _local_queue.push_back(std::move(task), global_queue);
    return true;
  }

  // 向本地队列推送批量任务，是否溢出通过返回值判断
  bool push_back_batch_task_to_local(std::vector<std::function<void()>> tasks) {
    _local_queue.push_back_batch(tasks);
    return true;
  }
  // 检查worker是否有任务
  bool is_worker_has_task() { return !_local_queue.empty(); }

  // 获取worker id
  std::size_t get_worker_id() const { return _worker_id; }

 private:
  // 从本地队列获取任务，先从高优先级队列获取，再从普通优先级队列获取
  std::optional<std::function<void()>> get_next_local_task() {
    if (!_local_queue.empty()) {
      return _local_queue.try_pop();
    } else {
      return std::nullopt;
    }
  }

  // worker获取下一个任务，策略是本地队列优先,本地没有任务时从全局队列拿,返回空
  std::optional<std::function<void()>> get_next_task() {
    std::optional<std::function<void()>> result{std::nullopt};

    // 先从本地取
    result = std::move(get_next_local_task());
    if (result.has_value()) {
      return result;
    }
    // 如果全局队列为空，返回空
    if (_shared->is_global_queue_empty()) {
      return std::nullopt;
    }

    // 获取到本地队列剩余大小的一半和容量一半的较小的那个
    auto num =
        std::min(_local_queue.remain_size(), _local_queue.capacity() / 2);
    if (num == 0) {
      return std::nullopt;
    }
    // 从全局队列获取num个任务
    auto tasks = _shared->get_batch_global_tasks(num);
    if (tasks.has_value() && !tasks.value().empty()) {
      auto& task_vec = tasks.value();
      // 从全局队列获取的任务中拿到最后一个任务
      auto task = std::move(task_vec.back());
      // 从全局队列获取的任务中移除最后一个任务
      task_vec.pop_back();
      // 如果全局队列中还有任务，把它们放到本地队列中
      if (!task_vec.empty()) {
        _local_queue.push_back_batch(task_vec);
      }
      return task;
    } else {
      return std::nullopt;
    }
  }

  // 任务窃取逻辑，取本地队列中剩余任务最多的worker
  std::optional<std::function<void()>> task_steal() {
    // 先判断能不能窃取
    if (!_shared->can_steal_task()) {
      return std::nullopt;
    }
    // 增加窃取worker计数
    _shared->increment_steal_worker_count();
    // 设置正在窃取任务标志
    _is_stealing.store(true, std::memory_order::release);
    auto workers = _shared->get_workers();
    std::size_t num = 0, idx = 0;
    for (auto& worker : workers) {
      // 不窃取自己的任务
      if (worker == t_worker) continue;
      if (worker->get_local_queue_size() > num &&
          !worker->_is_stealing.load(std::memory_order::acquire)) {
        num = worker->get_local_queue_size();
        idx = worker->get_worker_id();
      }
    }
    // 如果找到窃取目标，窃取任务
    if (num > 0) {
      auto res =
          workers[idx]->_local_queue.be_stolen_by(t_worker->_local_queue);
      _shared->decrement_steal_worker_count();
      _is_stealing.store(false, std::memory_order::release);
      return res;
    } else {
      // 如果没有找到窃取目标，从全局队列中获取任务
      _shared->decrement_steal_worker_count();
      _is_stealing.store(false, std::memory_order::release);
      return _shared->get_next_global_task();
    }
  }

  bool quit_condition(bool shutdown) {
    if (shutdown && _local_queue.empty() &&
        _shared->get_global_queue().empty()) {
      return true;
    } else {
      return false;
    }
  }

 private:
  std::size_t _worker_id{};               // worker id
  LocalQueue<> _local_queue{};            // 普通优先级队列
  Shared* _shared{};                      // 共享类指针
  std::atomic<bool> _is_stealing{false};  // 是否正在窃取任务
  bool _shutdown{false};                  // 是否关闭
};
}  // namespace fastexec::detail

#endif