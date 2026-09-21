#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <future>
#include <vector>
#include "fastexec/future.hpp"

// 编译器、优化级别、迭代次数完全相同；每轮都读取结果防止消除工作。
// 这里测量共享状态的创建、完成与读取，不包含线程池排队和线程创建。
static constexpr int iterations = 200000;
volatile std::uint64_t checksum = 0;

template <class F>
double measure(F&& fn) {
  std::vector<double> samples;
  for (int round = 0; round < 7; ++round) {
    const auto begin = std::chrono::steady_clock::now();
    std::uint64_t sum = 0;
    for (int i = 0; i < iterations; ++i) sum += fn(i);
    const auto end = std::chrono::steady_clock::now();
    checksum = checksum + sum;
    if (round) samples.push_back(
        std::chrono::duration<double, std::nano>(end - begin).count() / iterations);
  }
  std::sort(samples.begin(), samples.end());
  return (samples[2] + samples[3]) / 2;
}

int main() {
  const auto custom_promise = measure([](int i) {
    fastexec::promise<int> p;
    auto f = p.get_future();
    p.set_value(i);
    return f.get();
  });
  const auto std_promise = measure([](int i) {
    std::promise<int> p;
    auto f = p.get_future();
    p.set_value(i);
    return f.get();
  });
  const auto custom_task = measure([](int i) {
    fastexec::packaged_task<int()> task([i] { return i; });
    auto f = task.get_future();
    task();
    return f.get();
  });
  const auto std_task = measure([](int i) {
    std::packaged_task<int()> task([i] { return i; });
    auto f = task.get_future();
    task();
    return f.get();
  });
  std::printf("operation                  fastexec(ns)      std(ns)    speedup\n");
  std::printf("promise + future        %12.1f %12.1f %9.2fx\n",
              custom_promise, std_promise, std_promise / custom_promise);
  std::printf("packaged_task + future  %12.1f %12.1f %9.2fx\n",
              custom_task, std_task, std_task / custom_task);
  std::printf("checksum=%llu\n", static_cast<unsigned long long>(checksum));
}
