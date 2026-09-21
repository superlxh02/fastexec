#include "fastexec/exec.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <thread>

#define CHECK(expr)                                                            \
  do {                                                                         \
    if (!(expr)) {                                                             \
      std::fprintf(stderr, "CHECK failed: %s at %s:%d\n", #expr, __FILE__,     \
                   __LINE__);                                                  \
      std::abort();                                                            \
    }                                                                          \
  } while (false)

struct ThrowOnMove {
  ThrowOnMove() = default;
  ThrowOnMove(const ThrowOnMove &) = delete;
  ThrowOnMove(ThrowOnMove &&) { throw std::runtime_error("result move"); }
};

int main() {
  {
    fastexec::promise<int> p;
    auto f = p.get_future();
    p.set_value(42);
    CHECK(f.get() == 42 && !f.valid());
  }
  {
    fastexec::future<int> f;
    {
      fastexec::promise<int> p;
      f = p.get_future();
    }
    try {
      f.get();
      CHECK(false);
    } catch (const fastexec::future_error &) {
    }
  }
  {
    fastexec::packaged_task<std::unique_ptr<int>()> task(
        [] { return std::make_unique<int>(7); });
    auto f = task.get_future();
    task();
    CHECK(*f.get() == 7);
  }
  {
    fastexec::packaged_task<void()> task([] { throw std::runtime_error("x"); });
    auto f = task.get_future();
    task();
    try {
      f.get();
      CHECK(false);
    } catch (const std::runtime_error &) {
    }
  }
  {
    int v = 3;
    fastexec::promise<int &> p;
    auto f = p.get_future();
    p.set_value(v);
    CHECK(&f.get() == &v);
  }
  {
    // 子任务不可窃取，必定仍由提交它的 worker 执行。
    auto outer = fastexec::spawn([] {
      auto owner = std::this_thread::get_id();
      auto child = fastexec::spawn_with_options(
          false, [] { return std::this_thread::get_id(); });
      return std::pair{owner, std::move(child)};
    });
    auto result = outer.get();
    CHECK(result.first == result.second.get());
  }
  {
    auto f = fastexec::spawn([]() -> int { throw std::runtime_error("task"); });
    try {
      f.get();
      CHECK(false);
    } catch (const std::runtime_error &) {
    }
  }
  {
    std::atomic<int> completed{0};
    fastexec::block_on([&] {
      for (int i = 0; i < 10000; ++i)
        fastexec::spawn(
            [&] { completed.fetch_add(1, std::memory_order_relaxed); });
    });
    CHECK(completed.load(std::memory_order_relaxed) == 10000);
  }
  {
    // 只排入一个本地根任务也不能让 worker 在自己的 block_on 上死锁。
    auto outer =
        fastexec::spawn([] { return fastexec::block_on([] { return 7; }); });
    CHECK(outer.get() == 7);
    auto value = fastexec::block_on([] { return std::make_unique<int>(5); });
    CHECK(*value == 5);
    int referenced = 9;
    int &same = fastexec::block_on([&]() -> int & { return referenced; });
    CHECK(&same == &referenced);
  }
  {
    // worker 等待自己提交的 future，也必须主动执行该子任务。
    auto outer = fastexec::spawn([] {
      auto child = fastexec::spawn_with_options(false, [] { return 11; });
      return child.get();
    });
    CHECK(outer.get() == 11);
  }
  {
    // 内层任务组失败后，TLS 必须恢复为外层组，外层仍需等待随后派生的任务。
    std::atomic<int> completed{0};
    fastexec::block_on([&] {
      try {
        fastexec::block_on(
            [] { fastexec::spawn([] { throw std::runtime_error("inner"); }); });
        CHECK(false);
      } catch (const std::runtime_error &) {
      }
      fastexec::spawn([&] { completed.fetch_add(1); });
    });
    CHECK(completed.load() == 1);
  }
  {
    try {
      fastexec::block_on([]() -> int { throw std::runtime_error("root"); });
      CHECK(false);
    } catch (const std::runtime_error &e) {
      CHECK(std::string_view(e.what()) == "root");
    }
    std::atomic<int> completed{0};
    try {
      fastexec::block_on([&] {
        fastexec::spawn([] { throw std::runtime_error("child"); });
        fastexec::spawn([&] { completed.fetch_add(1); });
      });
      CHECK(false);
    } catch (const std::runtime_error &e) {
      CHECK(std::string_view(e.what()) == "child");
    }
    CHECK(completed.load() == 1);
    try {
      fastexec::block_on([] { fastexec::spawn([] { return ThrowOnMove{}; }); });
      CHECK(false);
    } catch (const std::runtime_error &e) {
      CHECK(std::string_view(e.what()) == "result move");
    }
  }
  {
    // close 拒绝外部新任务，但已有 worker 必须能继续派生并完成子任务。
    std::atomic<bool> started{false}, release{false};
    std::atomic<int> completed{0};
    auto root = fastexec::spawn([&] {
      started.store(true, std::memory_order_release);
      started.notify_one();
      while (!release.load(std::memory_order_acquire))
        release.wait(false, std::memory_order_acquire);
      for (int i = 0; i < 1000; ++i)
        fastexec::spawn(
            [&] { completed.fetch_add(1, std::memory_order_relaxed); });
    });
    while (!started.load(std::memory_order_acquire))
      started.wait(false, std::memory_order_acquire);
    fastexec::__inner::_fastexec_inner_thread_pool.close();
    release.store(true, std::memory_order_release);
    release.notify_one();
    root.get();
    fastexec::__inner::_fastexec_inner_thread_pool.wait_for_all();
    CHECK(completed.load(std::memory_order_relaxed) == 1000);
    try {
      fastexec::block_on([] {});
      CHECK(false);
    } catch (const std::runtime_error &) {
    }
    CHECK(fastexec::detail::t_current_task_group == nullptr);
  }
}
