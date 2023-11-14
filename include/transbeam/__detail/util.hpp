#pragma once

#include <array>
#include <concepts>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <vector>

namespace transbeam::__detail::util {

template<typename T>
class lazy_init {
public:
    lazy_init() = default;

    auto read() -> T* { return reinterpret_cast<T*>(mem_.data()); }
    template<typename... Args>
        requires(std::constructible_from<T, Args...>)
    void write(Args&&... args)
    {
        std::construct_at(read(), std::forward<Args>(args)...);
    }

private:
    alignas(T) std::array<unsigned char, sizeof(T)> mem_;
};

constexpr auto diff(auto a, auto b) { return a > b ? a - b : b - a; }

class wait_group {
public:
    void blocking_wait()
    {
        std::unique_lock l{m_};
        cond_.wait(l);
    }
    void notify_one() { cond_.notify_one(); }

private:
    std::condition_variable cond_;
    std::mutex m_;
};

} // namespace transbeam::__detail::util