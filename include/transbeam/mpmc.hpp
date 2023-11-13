#pragma once

#include <atomic>
#include <concepts>
#include <deque>
#include <memory>
#include <optional>
#include <stdexcept>

#include <transbeam/__detail/util.hpp>

namespace transbeam::mpmc { namespace __detail {
    // fixme: more writers than capacity... do we race on the lap? I think we do
    /// fixed size lock-free ring buffer
    template<typename T>
    class bounded_ringbuf {
        using vptr_t = std::atomic<T*>;

        using alloc_t = std::allocator<T>;

    public:
        using size_type = std::size_t;
        using value_type = T;

        constexpr explicit bounded_ringbuf(size_type capacity)
            : alloc_{}, buf_(alloc_.allocate(capacity)), capacity_{capacity}
        {
        }
        constexpr ~bounded_ringbuf()
        {
            // it is _not_ safe for multiple threads to still have a handle on us at this point
            // so don't bother handling that case
            while (pop().has_value()) {
            }
            alloc_.deallocate(buf_, capacity_);
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
        auto try_emplace(Args&&... args) -> bool
        {
            const auto mwd = [this]() -> std::optional<size_type> {
                while (true) {
                    // first we try and reserve a slot to write to
                    auto reservation = write_.load(std::memory_order_relaxed);
                    const auto rd = read_.load(std::memory_order_acquire);
                    const auto wanted = (reservation + 1) % capacity_;
                    if (wanted == rd) { // no free slots to write to
                        return std::nullopt;
                    }
                    if (write_.compare_exchange_weak(
                            reservation, wanted, std::memory_order_release,
                            std::memory_order_relaxed)) {
                        return wanted;
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
            return true;
        }
        auto pop() -> std::optional<T>
        {
            while (true) {
                auto rd = read_.load(std::memory_order_relaxed);
                const auto last = end_.load(std::memory_order_acquire);
                if (rd == last) { // nothing left to read
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
        std::atomic<size_type> read_{0};
    };

}} // namespace transbeam::mpmc::__detail
