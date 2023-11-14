#pragma once

#include <atomic>
#include <bit>
#include <cassert>
#include <concepts>
#include <deque>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>

#include <transbeam/__detail/util.hpp>
#include <type_traits>

namespace transbeam::mpmc {
namespace __detail {
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

        struct slot {
            std::atomic<size_type> stamp;
            ::transbeam::__detail::util::lazy_init<T> entry;
        };

    public:
        constexpr explicit bounded_ringbuf(size_type capacity)
            : buf_(new slot[capacity]),
              capacity_{capacity},
              mark_bit_{std::bit_ceil(capacity + 1)},
              one_lap_{mark_bit_ * 2}
        {
            if (capacity == std::numeric_limits<size_type>::max() ||
                capacity == 0) {
                throw std::logic_error{
                    "invalid capacity, too big or too small"};
            }
            for (size_type n = 0; n != capacity_; ++n) {
                buf_[n].stamp = n;
            }
        }
        constexpr ~bounded_ringbuf()
        {
            // it is _not_ safe for multiple threads to still have a handle on us at this point
            // so don't bother handling that case
            const auto wd = write_.load();
            const auto rd = read_.load();
            const auto len = size();
            for (size_type n = 0; n != len; ++n) {
                const auto idx = [this, n, rd, wd] {
                    if (rd + n < capacity_) {
                        return rd + n;
                    }
                    else {
                        return capacity_ - rd + n;
                    }
                }();
                std::destroy_at(buf_[idx].entry.read());
            }
        }

        constexpr auto size() const -> size_type
        {
            while (true) {
                const auto wd = write_.load();
                const auto rd = read_.load();
                if (write_.load() == wd) {
                    const auto widx = wd & (mark_bit_ - 1);
                    const auto ridx = rd & (mark_bit_ - 1);
                    return widx > ridx               ? widx - ridx
                           : ridx > widx             ? capacity_ - widx + ridx
                           : (wd & ~mark_bit_) == rd ? 0
                                                     : capacity_;
                }
            }
        }
        constexpr auto max_size() const { return capacity_; }
        constexpr auto empty() const { return size() == 0; }

        template<typename... Args>
            requires(std::constructible_from<T, Args...>)
        auto try_emplace(Args&&... args) -> bool
        {
            auto wd = write_.load(std::memory_order_relaxed);

            while (true) {
                const auto idx = wd & (mark_bit_ - 1);
                const auto lap = wd & ~(one_lap_ - 1);
                slot& s = buf_[idx];
                const auto stamp = s.stamp.load(std::memory_order_acquire);

                if (wd == stamp) {
                    const auto next_wd = [this, idx, lap, wd] {
                        if (idx + 1 >= capacity_) {
                            // wrap around and increase lap
                            return lap + one_lap_;
                        }
                        else {
                            return wd + 1;
                        }
                    }();
                    if (write_.compare_exchange_weak(
                            wd, next_wd, std::memory_order_seq_cst,
                            std::memory_order_relaxed)) {
                        // now we can actually do the update
                        s.entry.write(std::forward<Args>(args)...);
                        s.stamp.store(wd + 1, std::memory_order_release);
                        return true;
                    }
                }
                else if (stamp + one_lap_ == wd + 1) {
                    // we've come back on ourselves, can't overwrite this
                    std::atomic_thread_fence(std::memory_order_seq_cst);
                    const auto rd = read_.load(std::memory_order_relaxed);
                    if (rd + one_lap_ == wd) {
                        // if head is behind by a lap its full
                        return false;
                    }
                    wd = write_.load(std::memory_order_relaxed);
                }
                else {
                    wd = write_.load(std::memory_order_relaxed);
                }
            }
            return true;
        }
        auto pop() -> std::optional<T>
        {
            auto rd = read_.load(std::memory_order_relaxed);

            while (true) {
                const auto idx = rd & (mark_bit_ - 1);
                const auto lap = rd & ~(one_lap_ - 1);

                slot& s = buf_[idx];
                const auto stamp = s.stamp.load(std::memory_order_acquire);

                // if the write stamp is ahead by 1 we are allowed to read this
                if (rd + 1 == stamp) {
                    const auto next_rd = [this, rd, lap, idx] {
                        if (idx + 1 >= capacity_) {
                            return lap + one_lap_;
                        }
                        else {
                            return rd + 1;
                        }
                    }();
                    if (read_.compare_exchange_weak(
                            rd, next_rd, std::memory_order::seq_cst,
                            std::memory_order_relaxed)) {
                        // now we can actually read
                        auto ent = T{static_cast<T&&>(*s.entry.read())};
                        std::destroy_at(s.entry.read());
                        s.stamp.store(rd + one_lap_, std::memory_order_release);
                        return ent;
                    }
                }
                else if (rd == stamp) {
                    std::atomic_thread_fence(std::memory_order_seq_cst);
                    const auto wd = write_.load(std::memory_order_relaxed);
                    if ((wd & ~mark_bit_) == rd) {
                        // if on same lap and index then we are full
                        return std::nullopt;
                    }
                    rd = read_.load(std::memory_order_relaxed);
                }
                else {
                    rd = read_.load(std::memory_order_relaxed);
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
        constexpr auto wrap_pt() { return capacity_; }

        std::unique_ptr<slot[]> buf_;
        size_type capacity_;
        size_type mark_bit_;
        size_type one_lap_;
        /// index of the next free location to write to
        std::atomic<size_type> write_{0};
        /// 1 past the end of the valid range
        std::atomic<size_type> end_{0};
        /// the index of the first element
        std::atomic<size_type> read_{0};
    };
    template<typename T>
    struct shared_data {
        bounded_ringbuf<T> buf;
        ::transbeam::__detail::util::wait_group writers;
        ::transbeam::__detail::util::wait_group readers;
        shared_data(std::size_t cap) : buf{cap} {}
    };

} // namespace __detail
template<typename T>
class sender;

template<typename T>
class receiver {
public:
    auto try_recv() -> std::optional<T>
    {
        auto r = shared_->buf.pop();
        if (r.has_value()) {
            shared_->writers.notify_one();
        }
        return r;
    }
    auto recv() -> T
    {
        while (true) {
            auto r = this->try_recv();
            if (!r) {
                shared_->readers.blocking_wait();
            }
            else {
                return std::move(*r);
            }
        }
    }

private:
    receiver(std::shared_ptr<__detail::shared_data<T>> shared)
        : shared_{std::move(shared)}
    {
    }
    template<typename E>
    friend auto bounded(std::size_t capacity)
        -> std::pair<sender<E>, receiver<E>>;

    std::shared_ptr<__detail::shared_data<T>> shared_;
};

template<typename T>
class sender {
public:
    template<typename... Args>
        requires(std::constructible_from<T, Args...>)
    auto try_send(Args&&... args) -> bool
    {
        if (shared_->buf.try_emplace(std::forward<Args>(args)...)) {
            shared_->readers.notify_one();
            return true;
        }
        else {
            return false;
        }
    }

    template<typename... Args>
        requires(std::constructible_from<T, Args...>)
    void send(Args&&... args)
    {
        while (!this->try_send(std::forward<Args>(args)...)) {
            shared_->writers.blocking_wait();
        }
    }

private:
    sender(std::shared_ptr<__detail::shared_data<T>> shared)
        : shared_{std::move(shared)}
    {
    }
    template<typename E>
    friend auto bounded(std::size_t capacity)
        -> std::pair<sender<E>, receiver<E>>;

    std::shared_ptr<__detail::shared_data<T>> shared_;
};

template<typename T>
auto bounded(std::size_t capacity) -> std::pair<sender<T>, receiver<T>>
{
    auto shared = std::make_shared<__detail::shared_data<T>>(capacity);
    return std::pair{sender{shared}, receiver{std::move(shared)}};
}

static_assert(std::is_copy_constructible_v<receiver<int>>);
static_assert(std::is_move_constructible_v<receiver<int>>);
static_assert(std::is_copy_assignable_v<receiver<int>>);
static_assert(std::is_move_assignable_v<receiver<int>>);

static_assert(std::is_copy_constructible_v<sender<int>>);
static_assert(std::is_move_constructible_v<sender<int>>);
static_assert(std::is_copy_assignable_v<sender<int>>);
static_assert(std::is_move_assignable_v<sender<int>>);
} // namespace transbeam::mpmc
