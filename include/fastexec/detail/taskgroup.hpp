#ifndef __FASTSTDEXEC_DETAIL_TASK_GROUP_HPP
#define __FASTSTDEXEC_DETAIL_TASK_GROUP_HPP

#include <atomic>

#include "fastlog/fastlog.hpp"
namespace fastexec::detail {

/**
 *  任务组计数器
 * 用于追踪一组相关任务的完成情况。
 * 内部维护一个原子计数器，支持增加、减少和等待归零操作。
 */
struct TaskGroup {
  TaskGroup() { fastlog::console.trace("TaskGroup created"); }
  ~TaskGroup() { fastlog::console.trace("TaskGroup destroyed"); }
  // 正在运行（或排队）的任务数量
  std::atomic<size_t> running_count{0};

  // 增加计数：表示有一个新任务加入了该组
  void increment() { running_count.fetch_add(1, std::memory_order_relaxed); }

  // 减少计数：表示该组的一个任务已完成
  void decrement() {
    // fetch_sub 返回修改前的值。如果修改前是 1，说明减完后变成了 0。
    if (running_count.fetch_sub(1, std::memory_order_release) == 1) {
      // 只有当计数器归零时，才通知所有等待者
      running_count.notify_all();
    }
  }

  // 阻塞等待，直到计数器归零
  void wait() {
    size_t count = running_count.load(std::memory_order_acquire);
    while (count != 0) {
      // 使用 C++20 atomic::wait 进行高效等待
      running_count.wait(count);
      count = running_count.load(std::memory_order_acquire);
    }
  }
};

}  // namespace fastexec::detail

#endif