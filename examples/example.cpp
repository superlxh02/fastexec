#include "fastexec/exec.hpp"

// 基础异步接口
void base_demo() {
  auto f1 = fastexec::spawn([]() { fastlog::console.info("hello world"); });
  f1.wait();
  auto f2 = fastexec::spawn([](int a) { return a; }, 1);
  auto res = f2.get();
  fastlog::console.info("get result {}", res);
}

// 多个不同返回值的并行任务
void parallel_submit_demo() {
  auto f1 = fastexec::spawn([]() { return 1; });
  auto f2 = fastexec::spawn([]() { return 2.0; });
  auto f3 = fastexec::spawn([]() { return std::string{"hello world"}; });
  auto f4 = fastexec::spawn([]() { fastlog::console.info("void task"); });
  auto f5 =
      fastexec::spawn([]() { return std::vector<int>{100, 200, 300, 400}; });

  auto [r1, r2, r3, r4, r5] =
      fastexec::wait(std::move(f1), std::move(f2), std::move(f3), std::move(f4),
                     std::move(f5));
  fastlog::console.info("wait result: {}, {}, {}, {:}", r1, r2, r3, r5);
}

// 模拟带阻塞并行任务
void demo1_task() {
  fastexec::spawn([]() { fastlog::console.info("demo1_task first ..."); });
  fastexec::spawn([]() {
    fastlog::console.info("demo1_task second ...");
    std::this_thread::sleep_for(std::chrono::seconds(1));
  });
  fastexec::spawn([]() {
    fastlog::console.info("demo1_task third ...");
    std::this_thread::sleep_for(std::chrono::seconds(2));
  });
}

// 嵌套异步任务
void demo2_task() {
  for (int i = 0; i < 5; i++) {
    fastexec::spawn([i]() {
      fastlog::console.info("demo2_task first ... {}", i);
      std::this_thread::sleep_for(std::chrono::seconds(1));
      fastexec::spawn([i]() {
        fastlog::console.info("demo2_task second ... {}", i);
        std::this_thread::sleep_for(std::chrono::seconds(2));
        fastexec::spawn([i]() {
          fastlog::console.info("demo2_task third ... {}", i);
          fastexec::spawn(
              [i]() { fastlog::console.info("demo2_task fourth ..."); });
        });
      });
    });
  }
}

int main() {
  fastlog::set_consolelog_level(fastlog::LogLevel::Info);
  fastlog::console.info("base demo ................................");
  base_demo();
  fastlog::console.info("parallel_submit_demo ...........................");
  parallel_submit_demo();
  fastlog::console.info("demo1_task start...................................");
  fastexec::block_on(std::move(demo1_task));
  fastlog::console.info("demo1_task finish...................................");
  fastlog::console.info( "demo2_task  start....................................");
  fastexec::block_on(std::move(demo2_task));
  fastlog::console.info( "demo2_task  finish....................................");
}