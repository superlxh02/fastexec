# fastexec

**fastexec** 是一个基于现代C++的高性能异步任务执行库。它内部提供了一个高性能线程池的实现，并基于该线程池对外提供了高性能的异步操作接口。

## 项目特点
- **现代 C++**: 基于现代C++标准构建 (C++20)。
- **worker-thread模式线程池**: 基于RAII和单例模式实现worker-thread模式的线程池，复用资源并最小化开销。
- **工作窃取 (Work Stealing)**: 实现了工作窃取算法，在工作线程间自动平衡负载，防止某些线程空闲而其他线程过载。
- **任务组**: 支持将多个异步任务组织成一个任务组，确保在所有任务完成后才继续执行后续代码。

## 项目环境
- 编译器要求 ： 支持C++23的编译器
- 基于fastlog - C++23


## 项目用到的现代C++特性
- `C++11`:
  - **多线程支持**: `std::thread`, `std::mutex`, `std::lock_guard`, `std::atomic`, `std::future`, `std::packaged_task`, `thread_local`
  - **智能指针**: `std::shared_ptr`, `std::make_shared`
  - **函数对象**: `std::function`
  - **元编程**: `std::tuple`, `std::make_tuple`, 变参模板 (`Variadic Templates`), `std::move`, `std::forward`
- `C++14`:
  - **Lambda 捕获表达式**: `[func = std::move(f)]` (初始化捕获)
- `C++17`:
  - **编译期控制**: `if constexpr`
  - **类型特性**: `std::invoke_result_t`, `std::is_void_v`
  - **实用工具**: `std::optional`, `std::monostate`, `[[nodiscard]]`
  - **语法特性**: 内联变量 (`inline variables`), 嵌套命名空间定义 (`namespace A::B`)
- `C++20`:
  - **并发同步**: `std::latch`, `std::atomic::wait`, `std::atomic::notify_all`
  - **线程增强**: `std::jthread` (自动汇合线程)
  - **容器视图**: `std::span`


## 项目基本 API 使用
###  异步任务 (`spawn`)

使用 `fastexec::spawn` 创建一个异步任务。它返回一个 `std::future`，可用于获取执行结果。

```cpp
#include "fastexec/exec.hpp"
#include "fastlog/fastlog.hpp"

int main() {
    // 提交一个异步任务
    int a = 10, b = 20;
    auto future = fastexec::spawn([a, b]() {
        return a + b;
    });

    // 在此期间可以做其他事情...

    // 获取结果
    int result = future.get();
    fastlog::console.info("Result: {}", result);

    return 0;
}
```

### 等待多个任务 (`wait`)

使用 `fastexec::wait` 可以同时阻塞等待多个 `std::future`，并将它们的结果打包成 `std::tuple` 返回。

```cpp
auto f1 = fastexec::spawn([]() { return 1; });
auto f2 = fastexec::spawn([]() { return 2.0; });

// 阻塞等待所有任务完成，并获取结果
auto [r1, r2] = fastexec::wait(std::move(f1), std::move(f2));
fastlog::console.info("Results: {}, {}", r1, r2);
```

### 同步任务 (`block_on`)

使用 `fastexec::block_on` 提交一个任务并阻塞当前线程，直到该任务**及其所有子任务**全部完成。这通常用于程序的入口点或需要等待一组异步操作完成的场景。

```cpp
#include "fastexec/exec.hpp"
#include "fastlog/fastlog.hpp"
#include <thread>
#include <chrono>

int main() {
    fastexec::block_on([]() {
        fastlog::console.info("Parent task start");
        
        // 派生子任务
        fastexec::spawn([]() {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            fastlog::console.info("Child task finished");
        });

        fastlog::console.info("Parent task finished");
    });
    // block_on 会在此处等待，直到 Parent 任务和 Child 任务都完成
  
    return 0;
}
```



## 核心组件

`fastexec` 的架构基于 **Worker-Thread 模型** 并结合了 **工作窃取** 机制：

1. **线程池 (`thread_pool`)**:
   - 全局单例，任务提交入口
   - 根据硬件并发度初始化 `Worker` 数量。
2. **工作者 (`Worker`)**:
   - 每个线程绑定一个 `Worker` 实例，用于处理任务。
   - 维护一个本地队列。
3. **共享资源(`Shared`)**
   - 维护一个Worker数组，存储所有Worker实例。
   - 维护一个全局队列。
   - 维护停止状态，用于通知所有Worker线程停止运行。
4. **队列**
   -全局队列：基于单独互斥锁构建，非阻塞，用于负载均衡本地任务
   -本地队列：无锁实现，基于原子变量，单生产者多消费者，支持窃取
5. **任务组 (`TaskGroup`)**
   - 维护原子计数器，用于追踪一组相关联任务的生命周期。
   - 支持结构化并发，确保 `block_on` 能等待所有派生子任务完成。

## **任务调度**

任务调度决定了任务从提交到执行的整个生命周期路径。该项目采用了**多级队列驱动**的调度策略。

- **多级任务队列**
  - **本地队列 (LocalQueue)**：每个 Worker 线程拥有独立的私有队列 。这种设计减少了线程间的锁竞争。
  - **全局队列 (GlobalQueue)**：一个线程安全的公共队列，用于存放 `LOW`（低优先级）任务、外部线程提交的任务，或者作为本地队列溢出时的缓冲池。
- **调度优先级逻辑**
  - 当 Worker 运行（`run`）时，它遵循以下获取任务的顺序：
    1. **本地队列**先尝试从 `_local_queue` 弹出。
    2. **全局任务获取**：若本地均为空，则尝试从全局队列中**批量（Batch）**拉取任务到本地（`get_next_task`
       ）。批量拉取可以减少对全局锁的频繁竞争。
    3. **任务窃取**：若全局队列也为空，进入窃取阶段。
- **非阻塞设计**：Worker 在获取不到任务时会进行短时间的 `sleep_for(50us)`，而不是使用条件变量阻塞，这在高性能场景下能有效降低上下文切换开销。

## **任务窃取**

任务窃取是解决工作量不均匀的关键技术，允许空闲线程从繁忙线程中“偷”走一部分工作。

- **核心实现机制**
  - **双指针原子操作**：`LocalQueue` 使用了一个 64 位的原子变量 `_head`，将其拆分为两个 32 位指针：`steal`（窃取指针）和
    `local_head`（本地消费指针）。
  - **并发安全**：这种设计实现了**单生产者-多消费者**模型。Worker 自己从 `local_head` 端消费，而其他窃取者从 `steal`
    端窃取。通过 CAS（Compare-And-Swap）操作保证了在窃取发生时，本地 Worker 依然可以安全地处理任务。
- **窃取策略**
  - **贪心选择**：当 Worker 需要窃取时，它会遍历所有其他 Worker，寻找本地队列中任务数最多的那个（
    `task_steal`）。
  - **分而治之**：一旦选定目标，窃取者会尝试窃取目标队列中**一半**的任务（`be_stolen_by`）。这种“一次偷一半”的策略能快速平衡两个线程间的负载。
- **窃取限制**：为了防止过度窃取导致的总线风暴，项目引入了 `_steal_worker_count`，限制同时进行窃取的 Worker 数量不超过总数的一半（
  `can_steal_task`）。

## **负载均衡**

负载均衡确保了系统中的多个 Worker 能够尽可能均匀地分担计算压力。本项目主要依赖**动态重平衡**。

- **任务分发策略**
  - **外部提交**：外部线程（非 Worker 线程）提交的任务直接进入**全局队列**。
  - **内部提交**：Worker 线程提交的任务优先进入其**本地队列**，以保持数据的局部性（Locality）。
- **动态重平衡 (Dynamic Rebalancing)**
  - **任务窃取**：这是最核心的动态平衡手段。当某个 Worker 因为处理的任务耗时较短而提前变为空闲时，它会通过窃取机制主动分担其他
    Worker 的压力。
  - **全局队列缓冲**：当 Worker 线程无本地任务且无法从其他 Worker 窃取时，会从全局队列获取任务。同时，本地队列溢出时也会将任务推送到全局队列（
    `handle_overflow`）。

## **任务组**

任务组（Task Group）是实现**结构化并发**的核心机制。它解决了传统线程池“发射后不管”导致的任务生命周期难以管理的问题。

- **设计理念**
  - **隐式关联**：通过线程局部存储（TLS），`block_on` 创建的任务组会自动传播到该作用域内派生的所有子任务中。
  - **生命周期绑定**：父任务只有在所有子任务（以及子任务的子任务）全部完成后才会返回。这类似于 C++ `std::jthread` 的自动 join 行为，但在异步任务粒度上实现。

- **实现细节**
  - **原子引用计数**：`TaskGroup` 内部维护一个原子计数器。每当 `spawn` 一个新任务时，若当前存在活跃的任务组，计数器加一。
  - **上下文守卫 (Context Guard)**：任务执行时，会使用 RAII 对象临时设置当前线程的 TLS 指向所属的任务组，确保在该任务中继续 `spawn` 的孙任务也能正确加入该组。
  - **高效等待**：`block_on` 使用 `std::atomic::wait` (C++20) 在计数器上进行高效等待，避免了自旋锁带来的 CPU 浪费。
  
  这是一个关于 `TaskGroup` 生命周期的详细流程表。
**场景假设**：主线程调用 `block_on` 提交任务 **A**，任务 **A** 在执行过程中又提交了子任务 **B**。

| 步骤 | 执行位置 | Ref Count (内存) | Run Count (逻辑) | 详情说明 |
| :--- | :--- | :---: | :---: | :--- |
| **1. 创建组** | `block_on` | 1 | 0 | `make_shared` 创建对象，主线程局部变量持有。 |
| **2. 设置上下文** | `block_on` | 2 | 0 | 设置主线程 `TLS`，此时主线程被标记为该组成员。 |
| **3. 提交任务 A** | `submit` | 2 | **1** | 检测到 TLS 存在，调用 `increment()`，逻辑计数 +1。 |
| **4. 捕获引用** | `submit` | 3 | 1 | 任务 A 的 Lambda 闭包捕获 Group 指针（值传递），引用 +1。 |
| **5. 恢复上下文** | `block_on` | 2 | 1 | 主线程恢复之前的 TLS，引用 -1（不再持有该组 TLS）。 |
| **6. 主线程等待** | `block_on` | 2 | 1 | 主线程调用 `wait()` 阻塞，等待 Run Count 归零。 |
| **7. 运行任务 A** | Worker 线程 | 4 | 1 | `ContextGuard` 构造：<br>1. 成员变量拷贝 (+1)<br>2. 设置 Worker TLS (+1) |
| **8. 提交子任务 B** | A 的代码 | 4 | **2** | 任务 A 中调用 `submit`，Worker TLS 有值，逻辑计数 +1。 |
| **9. 捕获引用(B)** | A 的代码 | 5 | 2 | 任务 B 的 Lambda 闭包捕获 Group 指针，引用 +1。 |
| **10. 任务 A 结束** | Worker 线程 | 5 | **1** | `ContextGuard` 析构，调用 `decrement()`，逻辑计数 -1。 |
| **11. 清理 A** | Worker 线程 | 2 | 1 | 1. 恢复 Worker TLS (-1)<br>2. Guard 销毁 (-1)<br>3. 任务 A 闭包销毁 (-1)<br>剩余：Main局部变量 + 任务 B 闭包。 |
| **12. 运行任务 B** | Worker 线程 | 4 | 1 | `ContextGuard` 构造（同步骤 7），引用再次增加。 |
| **13. 任务 B 结束** | Worker 线程 | 4 | **0** | `ContextGuard` 析构，调用 `decrement()`。<br>**归零！** 通知主线程唤醒。 |
| **14. 清理 B** | Worker 线程 | 1 | 0 | 同步骤 11 清理过程。<br>剩余：只有 Main 局部变量持有。 |
| **15. 等待返回** | `block_on` | 1 | 0 | 主线程 `wait()` 收到通知并返回。 |
| **16. 销毁组** | `block_on` 结束 | **0** | 0 | 局部变量 `group` 离开作用域，引用归零，**内存释放**。 |
