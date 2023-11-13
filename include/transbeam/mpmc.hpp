#pragma once

#include <atomic>
#include <concepts>
#include <stdexcept>

#include <transbeam/__detail/util.hpp>

namespace transbeam::mpmc { namespace __detail {
    /// fixed size lock-free ring buffer
    template<typename T>
    class bounded_ringbuf {
        using vptr_t = std::atomic<T*>;

    public:
        using size_type = std::size_t;
        using value_type = T;

        explicit bounded_ringbuf(size_type capacity)
            : buf_{new T[capacity]}, capacity_{capacity}
        {
        }

        constexpr auto size() const
        {
            const auto w = write_.load(std::memory_order_relaxed);
            const auto r = read_.load(std::memory_order_relaxed);
            return w > r ? w - r : r - w;
        }
        constexpr auto max_size() const { return capacity_; }
        constexpr auto empty() const { return size() == 0; }

        template<typename... Args>
            requires(std::constructible_from<T, Args...>)
        auto emplace(Args&&... args) -> bool
        {
            const auto curr_write = write_.load();
            if (::transbeam::__detail::util::diff(curr_write, read_.load()) ==
                0) {
                throw std::out_of_range{
                    "tried to emplace into full ring buffer"};
            }
            const auto wd = advance_ptr();
        }
        auto pop() -> std::optional<T>
        {
            const auto rd = read_;
            if (rd == write_.load()) {
                return std::nullopt;
            }
            auto el = std::move(buf_[rd]);
            read_ = (read_ + 1) % capacity_;
            return el;
        }

    private:
        auto advance_ptr(size_type exp, std::atomic<size_type>& p)
        {
            while (!p.compare_exchange_weak(exp, (exp + 1) % capacity_)) {
                exp = p.load();
            }
            return exp;
        }

        T* buf_;
        size_type capacity_;
        std::atomic<size_type> write_{0};
        std::atomic<size_type> busy_write_{0};
        std::atomic<size_type> read_{0};
    };

}} // namespace transbeam::mpmc::__detail
