#pragma once

#include <atomic>
#include <concepts>
#include <deque>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>

#include <transbeam/__detail/util.hpp>

namespace transbeam::mpmc { namespace __detail {
    // fixme: more writers than capacity... do we race on the lap? I think we do
    /// fixed size lock-free ring buffer
    template<typename T>
    class bounded_ringbuf {
    public:
        using size_type = std::size_t;
        using value_type = T;

    private:
        using vptr_t = std::atomic<T*>;

        using alloc_t = std::allocator<T>;
        constexpr static auto out_of_bounds =
            std::numeric_limits<size_type>::max();

    public:
        constexpr explicit bounded_ringbuf(size_type capacity)
            : alloc_{}, buf_(alloc_.allocate(capacity)), capacity_{capacity}
        {
            if (capacity == std::numeric_limits<size_type>::max() ||
                capacity == 0) {
                throw std::logic_error{
                    "invalid capacity, too big or too small"};
            }
        }
        constexpr ~bounded_ringbuf()
        {
            // it is _not_ safe for multiple threads to still have a handle on us at this point
            // so don't bother handling that case
            while (pop().has_value()) {
            }
            alloc_.deallocate(buf_, capacity_);
        }

        constexpr auto size() const -> size_type
        {
            const auto w = end_.load(std::memory_order_relaxed);
            const auto r = read_.load(std::memory_order_relaxed);
            if (r == out_of_bounds) {
                return 0;
            }
            if (r > w) {
                // end has lapped
                return capacity_ - r + w;
            }
            else {
                return w - r;
            }
        }
        constexpr auto max_size() const { return capacity_; }
        constexpr auto empty() const { return size() == 0; }

        template<typename... Args>
            requires(std::constructible_from<T, Args...>)
        auto try_emplace(Args&&... args) -> bool
        {
            const auto mwd = [this]() -> std::optional<size_type> {
                while (true) {
                    // first we try and reserve a slot to write to
                    auto reservation = write_.load(std::memory_order_relaxed);
                    const auto rd = read_.load(std::memory_order_acquire);
                    if (reservation == rd) { // no free slots to write to
                        return std::nullopt;
                    }
                    if (write_.compare_exchange_weak(
                            reservation, (reservation + 1) % capacity_,
                            std::memory_order_release,
                            std::memory_order_relaxed)) {
                        return reservation;
                    }
                }
            }();
            if (!mwd) {
                return false;
            }
            auto wd = *mwd;
            std::construct_at(buf_ + wd, std::forward<Args>(args)...);
            while (!end_.compare_exchange_weak(wd, (wd + 1) % capacity_,
                                               std::memory_order_release,
                                               std::memory_order_relaxed)) {
            }
            auto rd = read_.load(std::memory_order_relaxed);
            if (rd == out_of_bounds) {
                // doesn't matter if this fails since as long as its not out of bounds anymore its fine
                read_.compare_exchange_strong(rd, 0, std::memory_order::release,
                                              std::memory_order_relaxed);
            }
            return true;
        }
        auto pop() -> std::optional<T>
        {
            while (true) {
                auto rd = read_.load(std::memory_order_relaxed);
                const auto last = end_.load(std::memory_order_acquire);
                if (rd == out_of_bounds || rd == last) { // nothing left to read
                    return std::nullopt;
                }
                if (read_.compare_exchange_weak(rd, (rd + 1) % capacity_,
                                                std::memory_order_release,
                                                std::memory_order_relaxed)) {
                    auto el = std::move(buf_[rd]);
                    buf_[rd].~T();
                    return el;
                }
            }
        }

    private:
        auto advance_ptr(size_type exp, std::atomic<size_type>& p)
        {
            while (!p.compare_exchange_weak(exp, (exp + 1) % capacity_)) {
                exp = p.load();
            }
            return exp;
        }

        alloc_t alloc_;
        T* buf_;
        size_type capacity_;
        /// index of the next free location to write to
        std::atomic<size_type> write_{0};
        /// 1 past the end of the valid range
        std::atomic<size_type> end_{0};
        /// the index of the first element
        std::atomic<size_type> read_{out_of_bounds};
    };

}} // namespace transbeam::mpmc::__detail
