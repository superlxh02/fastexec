#ifndef __FASTSTDEXEC_DETAIL_QUEUE_HPP
#define __FASTSTDEXEC_DETAIL_QUEUE_HPP

#include <atomic>
#include <cassert>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

#include "fastlog/fastlog.hpp"
#include "util.hpp"
namespace fastexec::detail {
// 非阻塞全局队列：基于互斥锁，但是不基于条件变量
class GlobalQueue : util::noncopyable {
 public:
  explicit GlobalQueue() = default;

  ~GlobalQueue() {
    if (!closed()) close();
  }

 public:
  [[nodiscard]]
  bool closed() const {
    return _closed;
  }

  void close() { _closed.store(true); }

  [[nodiscard]]
  std::size_t size() const {
    auto lock = get_lock();
    return _queue.size();
  }

  [[nodiscard]]
  bool empty() const {
    auto lock = get_lock();
    return _queue.empty();
  }

  void push_back(std::function<void()> task) {
    if (closed()) throw std::runtime_error{"queue is closed"};
    auto lock = get_lock();
    _queue.push_back(std::move(task));
  }

  void push_back_batch(std::span<std::function<void()>> tasks) {
    if (closed()) throw std::runtime_error{"queue is closed"};
    auto lock = get_lock();
    _queue.insert(_queue.end(), tasks.begin(), tasks.end());
  }

  auto try_pop() -> std::optional<std::function<void()>> {
    auto lock = get_lock();
    if (_queue.empty()) return std::nullopt;
    auto task = std::move(_queue.front());
    _queue.pop_front();
    return task;
  }
  // 尝试批量弹出任务
  auto try_pop_batch(std::size_t size)
      -> std::optional<std::vector<std::function<void()>>> {
    auto lock = get_lock();
    if (_queue.empty()) return std::nullopt;
    std::size_t n = std::min(_queue.size(), size);
    if (n == 0) return std::nullopt;
    std::vector<std::function<void()>> tasks;
    tasks.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
      tasks.push_back(std::move(_queue.front()));
      _queue.pop_front();
    }
    return tasks;
  }

 private:
  // 获取互斥锁
  auto get_lock() const -> std::lock_guard<std::mutex> {
    return std::lock_guard<std::mutex>{_mutex};
  }

 private:
  mutable std::mutex _mutex{};                 // 互斥锁，用于保护队列
  std::deque<std::function<void()>> _queue{};  // 任务队列
  std::atomic<bool> _closed{false};            // 队列是否关闭
};

// 本地队列 ，基于array,无锁，支持窃取操作
// 注意，该队列是单生产者多消费者队列
// 所以将头指针拆成两部分，加上通过cas更新，来保证线程安全
// 正常单线程操作情况：头指针两部分相等。
// 有其他线程操作队列的时候(窃取该队列)：两部分不相等
template <std::size_t CAPACITY = 256>
class LocalQueue {
  static_assert((CAPACITY & (CAPACITY - 1)) == 0,
                "CAPACITY must be power of 2");
  static_assert(CAPACITY > 0, "CAPACITY must be greater than 0");

 public:
  LocalQueue() = default;
  ~LocalQueue() = default;

 public:
  [[nodiscard]]
  std::size_t capacity() const {
    return CAPACITY;
  }
  // 返回队列中剩余可用空间的数量
  [[nodiscard]]
  std::size_t remain_size() {
    auto tail = _tail.load(std::memory_order::acquire);
    auto head = _head.load(std::memory_order::acquire);
    auto [steal, local_head] = unpack(head);
    return CAPACITY - static_cast<std::uint64_t>(tail - steal);
  }

  // 返回队列中当前任务数量
  [[nodiscard]]
  std::size_t size() {
    auto tail = _tail.load(std::memory_order::acquire);
    auto head = _head.load(std::memory_order::acquire);
    auto [steal, local_head] = unpack(head);
    return static_cast<std::size_t>(tail - local_head);
  }

  [[nodiscard]]
  bool empty() {
    return size() == 0u;
  }

 public:
  // 尝试批量将任务推送到队列末尾
  void push_back_batch(std::span<std::function<void()>> tasks) {
    assert(0 < tasks.size() && tasks.size() <= capacity());
    auto [steal, _] = unpack(_head.load(std::memory_order::acquire));
    auto tail = _tail.load(std::memory_order::relaxed);
    for (auto&& task : tasks) {
      std::size_t idx = tail & _mask;
      _tasks[idx] = std::move(task);
      tail += 1;
    }
    _tail.store(tail, std::memory_order::release);
  }

  // 尝试将任务推送到队列末尾,如果队列已满,则尝试将队列中的任务转移到全局队列
  void push_back(std::function<void()> task, GlobalQueue& global_queue) {
    // step 1 : 溢出处理
    //  预先定义尾指针变量
    std::uint32_t tail = 0;
    // handle_overflow使用了compare_exchange_weak,所以这里需要循环重试
    while (true) {
      auto head = _head.load(std::memory_order::acquire);
      auto [steal, local_head] = unpack(head);
      tail = _tail.load(std::memory_order::acquire);
      // 如果尾指针与窃取指针的差值小于队列容量，说明队列未满，直接跳出循环
      if (tail - steal < static_cast<std::uint32_t>(CAPACITY)) {
        break;
      } else if (steal != local_head) {
        // 队列已满,且头指针与实际头指针不同,说明有其他线程在窃取任务
        // 尝试将任务推送到全局队列
        global_queue.push_back(std::move(task));
      } else {
        // 正常调用处理溢出
        if (handle_overflow(task, local_head, tail, global_queue)) {
          return;
        }
      }
    }
    // step 2 : 将任务存储在数组中,更新头指针
    _tasks[tail & _mask] = std::move(task);
    _tail.store(tail + 1, std::memory_order::release);
  }

  // 尝试从队列头部弹出任务
  // 这个队列是多消费者队列，所以需要用cas操作来更新头指针。
  std::optional<std::function<void()>> try_pop() {
    auto cur_head = _head.load(std::memory_order::acquire);
    std::size_t index = 0;
    while (true) {
      auto [cur_steal, cur_local_head] = unpack(cur_head);
      auto tail = _tail.load(std::memory_order::acquire);
      // 如果实际头指针等于尾指针，说明队列为空
      if (cur_local_head == tail) {
        return std::nullopt;
      } else {
        // 得到下一个实际头指针
        auto next_local_head = cur_local_head + 1;
        // 得到下一个完整头指针，
        auto next_head = (cur_local_head == cur_steal)
                             ? pack(next_local_head, next_local_head)
                             : pack(cur_steal, next_local_head);

        // 尝试更新头指针,如果失败,说明有其他线程在更新头指针,需要重试
        if (_head.compare_exchange_weak(cur_head, next_head,
                                        std::memory_order::acq_rel,
                                        std::memory_order::acquire)) {
          // 成功，获得当前任务的索引,并且退出循环
          index = static_cast<std::size_t>(cur_local_head) & _mask;
          break;
        }
      }
    }
    auto task = std::move(_tasks[index]);
    _tasks[index] = nullptr;
    return task;
  }
  // 当前队列被目标队列窃取
  // 返回最后一个被窃取的任务
  std::optional<std::function<void()>> be_stolen_by(LocalQueue& dst_queue) {
    std::optional<std::function<void()>> result{std::nullopt};
    auto [dst_steal, dst_local_head] =
        unpack(dst_queue._head.load(std::memory_order::acquire));
    auto dst_tail = dst_queue._tail.load(std::memory_order::acquire);
    // 如果目标队列的已有任务大于队列容量的一半,无法进行窃取操作，直接返回空
    if (dst_tail - dst_steal > static_cast<std::uint32_t>(CAPACITY) / 2) {
      return result;
    }
    // 进行窃取并且更新头指针
    auto steal_num = be_stolen_by_impl(dst_queue, dst_tail);

    // 如果窃取数量为0,说明没有任务被窃取,直接返回空
    if (steal_num == 0) {
      return result;
    }
    // 窃取数量减1,表示拿出最后一个被窃取的任务
    steal_num = steal_num - 1;
    // 得到新的目标队列尾指针
    auto next_dst_tail = dst_tail + steal_num;
    // 得到最后一个任务的索引
    auto idx = static_cast<std::size_t>(next_dst_tail) & _mask;
    // 填充结果，最后一个被窃取的任务
    result.emplace(std::move(dst_queue._tasks[idx]));
    // 如果窃取数量仍然大于0,更新目标队列的尾指针
    if (steal_num > 0) {
      // 更新目标队列的尾指针
      dst_queue._tail.store(next_dst_tail, std::memory_order::release);
    }
    return result;
  }

 private:
  // 处理队列溢出,将本地队列中一半的任务转移到全局队列
  bool handle_overflow(std::function<void()> task, std::uint32_t local_head,
                       std::uint32_t tail, GlobalQueue& global_queue) {
    // step1 : 更新头指针
    // 1.获取到队列容量的一半作为默认转移的数量
    auto take_len = static_cast<std::uint32_t>(CAPACITY / 2);
    assert(tail - local_head);
    // 2.打包当前头指针
    auto cur_head = pack(local_head, local_head);
    // 3.打包下一个头指针
    auto next_head = pack(local_head + take_len, local_head + take_len);
    // 4.更新头指针
    if (!_head.compare_exchange_weak(cur_head, next_head,
                                     std::memory_order::relaxed)) {
      fastlog::console.error("handle_overflow: failed to update head pointer");
      return false;
    }

    // step2 : 转移任务到一个临时vector
    // 1.将take_len数量的任务从本地队列转移到一个临时定义的vector内
    std::vector<std::function<void()>> tasks;
    for (int i = 0; i < take_len; i++) {
      std::size_t idx = static_cast<std::size_t>(local_head + i) & _mask;
      tasks.push_back(std::move(_tasks[idx]));
    }
    // 2.将触发溢出的任务添加到vector内
    tasks.push_back(task);

    // step3 : 将vector内的任务批量推送到全局队列
    global_queue.push_back_batch(tasks);
    return true;
  }

  // 当前队列被目标队列窃取(头指针更新+窃取操作封装)
  // 主要逻辑 ：
  // 1.将当前队列任务的一半所为窃取任务数量，保持窃取指针不变，得到新的本地头指针(当前头指针+窃取数量)，打包成完成头指针通过cas更新
  // 2.根据窃取数量，目标队列窃取当前队列的任务
  // 3.获取到之前更新后的头指针作为当前指针，更新当前队列的窃取指针，将窃取指针等于本地头指针，合并成一个64位整数，通过cas更新
  std::uint32_t be_stolen_by_impl(LocalQueue& dst, std::uint32_t dst_tail) {
    // step1 :
    // 更新整体头指针-更新本地头指针，窃取指针不动,告诉其他线程我开始窃取任务了
    std::uint64_t cur_src_head = _head.load(std::memory_order::acquire);
    std::uint64_t next_src_head = 0;
    std::uint32_t steal_num = 0;
    while (true) {
      auto [cur_src_steal, cur_src_local_head] = unpack(cur_src_head);
      auto cur_src_tail = _tail.load(std::memory_order::acquire);
      auto cur_src_size = cur_src_tail - cur_src_local_head;
      // 如果窃取指针不等于实际头指针，说明有其他线程在窃取任务
      if (cur_src_steal != cur_src_local_head) {
        return 0;
      }
      // 1. 计算当前队列中可窃取的任务数量
      // 2. 取当前队列大小的一半作为窃取数量
      steal_num = cur_src_size / 2;
      if (steal_num == 0) {
        return 0;
      }
      // 3.更新下一个本地头指针,值为当前头指针加上窃取数量
      auto next_src_local_head = cur_src_local_head + steal_num;
      // 4.确保下一个头指针不等于当前头指针,如果相等,说明队列只有一个任务,无法窃取
      assert(cur_src_steal != next_src_local_head);
      // 5.打包下一个头指针，窃取指针不变，和下一个本地头指针合并成一个64位整数
      next_src_head = pack(cur_src_steal, next_src_local_head);
      if (_head.compare_exchange_weak(cur_src_head, next_src_head,
                                      std::memory_order::acq_rel,
                                      std::memory_order::acquire)) {
        break;
      }
    }

    // step2 : 窃取任务
    // 1.从next_src_steal开始,窃取steal_num数量的任务
    auto [next_src_steal, next_src_local_head] = unpack(next_src_head);
    for (std::uint32_t i = 0; i < steal_num; i++) {
      // 2.将窃取到的任务移动到目标队列的对应位置
      auto src_idx = static_cast<std::uint32_t>(next_src_steal + i) & _mask;
      auto dst_idx = static_cast<std::uint32_t>(dst_tail + i) & _mask;
      dst._tasks[dst_idx] = std::move(_tasks[src_idx]);
    }

    // step3 : 更新整体头指针-更新窃取指针，让窃取指针等于本地头指针
    // 1.将之前的下一个头指针更新成当前指针
    cur_src_head = next_src_head;
    // 2.更新
    while (true) {
      // 2.1 从当前头指针中提取当前窃取指针和当前本地头指针
      auto [cur_src_steal, cur_src_local_head] = unpack(cur_src_head);
      // 2.2 打包下一个头指针，让窃取指针和本地头指针一致，合并成一个64位整数
      next_src_head = pack(cur_src_local_head, cur_src_local_head);
      // 2.3 尝试更新头指针,如果成功,返回窃取数量
      if (_head.compare_exchange_weak(cur_src_head, next_src_head,
                                      std::memory_order::acq_rel,
                                      std::memory_order::acquire)) {
        return steal_num;
      }
    }
  }

 private:
  constexpr static inline std::size_t _mask =
      CAPACITY - 1;  // 掩码，用于取模操作
  /*
   * 功能 ：将两个32位整数合并成一个64位整数
   * 实现原理 ：
   *    static_cast<uint64_t>(steal) << 32
   * ：将第一个参数转换为64位并左移32位，放在高32位
   * static_cast<uint64_t>(local_head) ：将第二个参数转换为64位，占据低32位
   * 使用按位或操作符 | 将两部分合并
   */
  [[nodiscard]]
  static auto pack(uint32_t steal, uint32_t local_head) -> uint64_t {
    return static_cast<uint64_t>(steal) << 32 |
           static_cast<uint64_t>(local_head);
  }
  /*
   * 功能:将64位头指针拆成两部分，每个部分32位
   * 高32位代表窃取指针，低32位代表本地头指针
   * 实现原理 ：
   *   head >> 32 ：右移32位，获取高32位的值
   *   static_cast<uint32_t>(head) ：直接转换为32位整数，自动截取低32位
   */
  [[nodiscard]]
  static auto unpack(std::uint64_t head) -> std::pair<uint32_t, uint32_t> {
    return {static_cast<uint32_t>(head >> 32), static_cast<uint32_t>(head)};
  }

 private:
  std::array<std::function<void()>, CAPACITY> _tasks{};  // 固定数组存放任务
  std::atomic<std::uint64_t> _head{};  // 64位头指针，用于生产和窃取任务
  std::atomic<std::uint32_t> _tail{};  // 32位尾指针，用于消费任务
};
}  // namespace fastexec::detail
#endif
