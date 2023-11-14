#pragma once

#include <array>
#include <concepts>
#include <deque>

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

} // namespace transbeam::__detail::util