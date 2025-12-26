#ifndef __FASTSTDEXEC_DETAIL_UTIL_HPP
#define __FASTSTDEXEC_DETAIL_UTIL_HPP

namespace fastexec::detail::util {
// 非拷贝类，用于防止类被拷贝
class noncopyable {
 public:
  noncopyable(const noncopyable &) = delete;
  noncopyable &operator=(const noncopyable &) = delete;

 protected:
  noncopyable() = default;
  ~noncopyable() noexcept = default;
};

// 单例类，用于创建全局唯一的实例
template <typename T>
class Singleton {
  // Singleton() = delete;
  // ~Singleton() = delete;

 public:
  static auto instance() -> T & {
    static T instance;
    return instance;
  }
};
}  // namespace fastexec::detail::util

#endif
