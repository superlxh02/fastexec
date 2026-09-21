#ifndef FASTEXEC_DETAIL_WORKER_HPP
#define FASTEXEC_DETAIL_WORKER_HPP

#include <algorithm>
#include <cstdint>
#include <deque>
#include <optional>
#include <thread>
#include <vector>

#include "fastexec/future.hpp"
#include "queue.hpp"
#include "shared.hpp"
#include "taskgroup.hpp"
namespace fastexec::detail {
class Worker;
inline thread_local Worker *t_worker{nullptr};

class Worker {
  friend class Shared;

public:
  Worker(Shared *shared, std::size_t worker_id)
      : _shared(shared), _worker_id(worker_id),
        _random_state(0x9e3779b97f4a7c15ULL ^
                      (worker_id + 1) * 0xbf58476d1ce4e5b9ULL) {
    _shared->register_worker(static_cast<int>(worker_id), this);
    t_worker = this;
    fastexec::detail::future_help_context = this;
    fastexec::detail::future_help = [](void *worker,
                                       const std::atomic<bool> &ready) {
      static_cast<Worker *>(worker)->help_until_ready(ready);
    };
  }
  ~Worker() {
    t_worker = nullptr;
    fastexec::detail::future_help = nullptr;
    fastexec::detail::future_help_context = nullptr;
    _shared->_stop_latch.arrive_and_wait();
  }

  // 快路径只查自己的队列；空闲时依次查全局队列和随机起点的其他 worker。
  // 工作版本号在查队列前获取，atomic::wait 只在版本号未改变时睡眠，
  // 既不反复执行 100us 定时唤醒，也不会漏掉检查期间提交的任务。
  void run() {
    for (;;) {
      const auto epoch = _shared->work_epoch();
      if (run_one())
        continue;
      if (_shared->get_global_queue().closed() && _shared->all_done())
        break;
      _shared->wait_for_work(epoch);
    }
  }

  // worker 在等待自己的子任务时继续执行队列中的工作，避免占满线程池后互等。
  // 版本号在扫描队列前读取；任务组归零也会发出工作通知，因此不会漏唤醒。
  void help_until(TaskGroup &group) {
    while (group.count() != 0) {
      const auto epoch = _shared->work_epoch();
      if (run_one())
        continue;
      if (group.count() == 0)
        break;
      _shared->wait_for_work(epoch);
    }
  }

  void help_until_ready(const std::atomic<bool> &ready) {
    while (!ready.load(std::memory_order_acquire)) {
      const auto epoch = _shared->work_epoch();
      if (run_one())
        continue;
      if (ready.load(std::memory_order_acquire))
        break;
      _shared->wait_for_work(epoch);
    }
  }

  bool run_one() {
    auto task = get_next_task();
    if (!task)
      task = task_steal();
    if (!task)
      return false;
    (*task)();
    return true;
  }

  // 单生产者约束：只能由所属 worker 调用。不可窃取任务进入私有队列。
  void push_back_task_to_local(Task task, GlobalQueue &global_queue) {
    if (task.stealable)
      _local_queue.push_back(std::move(task.run), global_queue);
    else
      _private_queue.push_back(std::move(task));
    _shared->signal_work();
  }
  std::size_t get_worker_id() const { return _worker_id; }

private:
  std::optional<Task> get_next_task() {
    if (!_private_queue.empty()) {
      auto task = std::move(_private_queue.front());
      _private_queue.pop_front();
      return task;
    }
    if (auto local = _local_queue.try_pop())
      return Task{std::move(*local), true};

    // 批量从全局队列搬运，摊薄全局锁的成本。不可窃取任务在这里绑定到当前
    // worker。
    const auto batch = _local_queue.capacity() / 2;
    auto tasks = _shared->get_batch_global_tasks(batch);
    if (!tasks)
      return std::nullopt;
    auto current = std::move(tasks->back());
    tasks->pop_back();
    for (auto &task : *tasks) {
      if (task.stealable)
        _local_queue.push_back(std::move(task.run),
                               _shared->get_global_queue());
      else
        _private_queue.push_back(std::move(task));
    }
    // 一批可窃取任务刚进入本地队列，唤醒休眠中的窃取者。
    if (!tasks->empty())
      _shared->signal_work(true);
    return current;
  }

  std::uint64_t next_random() {
    // 每个 worker 独立的 xorshift64 状态，热路径无全局随机数锁。
    auto x = _random_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return _random_state = x;
  }

  std::optional<Task> task_steal() {
    auto workers = _shared->get_workers();
    if (workers.size() < 2 || !_shared->can_steal_task())
      return std::nullopt;
    _shared->increment_steal_worker_count();
    // 随机起点、顺序轮询：平均分散热点，且一次失败后仍能检查所有目标。
    const auto start = next_random() % workers.size();
    for (std::size_t step = 0; step < workers.size(); ++step) {
      auto *victim = workers[(start + step) % workers.size()];
      if (victim == this)
        continue;
      if (auto stolen = victim->_local_queue.be_stolen_by(_local_queue)) {
        _shared->decrement_steal_worker_count();
        return Task{std::move(*stolen), true};
      }
    }
    _shared->decrement_steal_worker_count();
    return std::nullopt;
  }

  std::size_t _worker_id;
  LocalQueue<> _local_queue;
  std::deque<Task> _private_queue;
  Shared *_shared;
  std::uint64_t _random_state;
};
} // namespace fastexec::detail
#endif
