# fastexec

`fastexec` 是一个 C++20 线程池，提供任务提交、工作窃取、自定义 `future/promise/packaged_task`，以及等待整棵子任务树完成的 `block_on`。

## 快速开始

```cpp
#include "fastexec/exec.hpp"
#include <iostream>

int main() {
  auto value = fastexec::block_on([] {
    auto child = fastexec::spawn([] { return 21; });
    return child.get() * 2;
  });
  std::cout << value << '\n';  // 42
  fastexec::close_and_join();
}
```

核心库是头文件实现，使用时将 `include/` 加入头文件搜索路径，并以 C++20 编译。仓库里的 fastlog 示例单独需要 C++23。

## 常用接口

| 接口 | 作用 |
| --- | --- |
| `spawn(f, args...)` | 提交任务并返回 `fastexec::future<T>` |
| `spawn_with_options(false, f, args...)` | 提交不可窃取任务 |
| `wait(std::move(f1), ...)` | 获取多个 future 的结果，返回 tuple |
| `block_on(f, args...)` | 等待根任务及其在任务组内派生的子任务，返回根任务结果并传播异常 |
| `close_and_join()` | 关闭外部提交入口并等待 worker 退出；线程池不能重新启动 |

## 构建与测试

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
./build/future_bench
```

## 文档

- [架构与源码分析](docs/架构与源码分析.md)：线程并发模型的演进、分层架构与模块所有权、本地无锁队列与三阶段窃取协议、worker 工作循环与唤醒、提交路径、结果通道、任务组跟踪、线程池生命周期。
- [示例程序](examples/example.cpp) 与 [正确性测试](tests/future_test.cpp)。
- [future 对照基准](benchmarks/future_bench.cpp)：比较本项目与标准库的结果通道微基准；不包含线程池调度开销。
