#pragma once

#include "transbeam/__detail/backoff.hpp"
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
    template<typename T, typename Backend>
    class sender;

    template<typename T, typename Backend>
    class receiver;
    template<typename T>
    class bounded_ringbuf;

    template<typename T>
    class linked_list;
} // namespace __detail
template<typename T>
using sync_sender = __detail::sender<T, __detail::bounded_ringbuf<T>>;

template<typename T>
using sync_receiver = __detail::receiver<T, __detail::bounded_ringbuf<T>>;

template<typename T>
using sender = __detail::sender<T, __detail::linked_list<T>>;

template<typename T>
using receiver = __detail::receiver<T, __detail::linked_list<T>>;
template<typename T>
auto bounded(std::size_t capacity)
    -> std::pair<sync_sender<T>, sync_receiver<T>>;

template<typename T>
auto unbounded() -> std::pair<sender<T>, receiver<T>>;

namespace __detail {

    template<typename B>
    struct shared_data;

    /// block in the list hold a chunk of items
    constexpr auto chunk_size = 32;
    /// maximum items in a chunk
    constexpr auto chunk_capacity = chunk_size - 1;
    constexpr auto meta_bits = 1;
    /// meta bit for index
    /// - on read it means its not the last block
    /// - on write it doesn't mean anything right now
    constexpr auto meta_bit = 1;

    class block_state {
    public:
        /// wait until the write flag is set in the state
        void ensure_write_flag()
        {
            while (true) {
                const auto st = state.load(std::memory_order::acquire);
                if ((st & write_bit) == 0) {
                    state.wait(st, std::memory_order_acquire);
                }
                else {
                    break;
                }
            }
        }
        void mark_written()
        {
            state.fetch_or(write_bit, std::memory_order::release);
            state.notify_all();
        }
        /// if the read bit is set then this slot has been read from in full
        auto read_bit_set() const -> bool
        {
            return (state.load(std::memory_order::acquire) & read_bit) != 0;
        }
        /// set the destroy bit and check the read bit. this synchronises with `mark_read_and_check_destroy`
        auto mark_destroy_and_check_read() -> bool
        {
            return (state.fetch_or(destroy_bit, std::memory_order_acq_rel) &
                    read_bit) != 0;
        }
        /// set the read bit and check the destroy bit. this synchronises with `mark_destroy_and_check_read`
        auto mark_read_and_check_destroy() -> bool
        {
            return (state.fetch_or(read_bit, std::memory_order_acq_rel) &
                    destroy_bit) != 0;
        }

    private:
        std::atomic_uint8_t state{0};

        constexpr static auto write_bit = 0b1;
        constexpr static auto read_bit = 0b10;
        constexpr static auto destroy_bit = 0b100;
    };

    /// this one is based on the crossbeam `list` flavour
    template<typename T>
    class linked_list {
    public:
        using size_type = std::size_t;
        using value_type = T;

    private:
        struct entry {
            ::transbeam::__detail::util::lazy_init<T> cell;
            block_state state;
        };
        struct block {
            /// next block in the linked list
            std::atomic<block*> next{nullptr};
            /// entries in this chunk
            entry entries[chunk_capacity]{};

            auto wait_for_next() -> block*
            {
                while (true) {
                    auto n = next.load(std::memory_order::acquire);
                    if (n != nullptr) {
                        return n;
                    }
                    else {
                        next.wait(n, std::memory_order::acquire);
                    }
                }
            }
        };
        struct index_type {
            std::atomic<size_type> idx{0};
            std::atomic<block*> bptr{nullptr};
        };

        template<typename E>
        friend struct shared_data;

        linked_list() = default;

    public:
        linked_list(linked_list&&) = delete;
        linked_list(const linked_list&) = delete;

        ~linked_list()
        {
            while (pop().has_value()) {
            }
            const auto b = read_.bptr.load();
            if (b != nullptr) {
                // we still have the last block left
                delete b;
            }
        }

        template<typename... Args>
            requires(std::constructible_from<T, Args...>)
        auto try_emplace(Args&&... args) -> bool
        {
            ::transbeam::__detail::backoff spinner;
            auto wd = write_.idx.load(std::memory_order::acquire);
            auto target = write_.bptr.load(std::memory_order::acquire);
            // this may or may not be used
            std::unique_ptr<block> next_block{nullptr};

            while (true) {
                const auto offset = (wd >> meta_bits) % chunk_size;
                if (offset ==
                    chunk_capacity) { // we've maxed out this block, wait for the next one to be set up for us
                    spinner.thread_yield();
                    wd = write_.idx.load(std::memory_order::acquire);
                    target = write_.bptr.load(std::memory_order::acquire);
                }
                else {
                    if ((offset + 1 == chunk_capacity) &&
                        next_block == nullptr) {
                        // we are going to have to allocate the next block anyway, lets do it now
                        // so we take up minimal spin time
                        next_block = std::make_unique<block>();
                    }
                    if (target == nullptr) {
                        // there is no block allocated yet => we are the first message
                        const auto new_block = new block;
                        if (write_.bptr.compare_exchange_strong(
                                target, new_block, std::memory_order::release,
                                std::memory_order_relaxed)) {
                            // we got to write the block, lets use it
                            read_.bptr.store(new_block,
                                             std::memory_order::release);
                            target = new_block;
                        }
                        else {
                            // someone beat us it, i.e. two emplace happen at once on an empty list
                            // instead of wasting this allocation, lets use it for the next block
                            next_block = std::unique_ptr<block>{new_block};
                            // then load the new state from whoever beat us
                            wd = write_.idx.load(std::memory_order::acquire);
                            target =
                                write_.bptr.load(std::memory_order::acquire);
                            continue;
                        }
                    }

                    const auto next_wd = wd + (1 << meta_bits);
                    // we need seq_cst and aquire here to ensure we are ordered strictly with the block load
                    if (write_.idx.compare_exchange_weak(
                            wd, next_wd, std::memory_order::seq_cst,
                            std::memory_order::acquire)) {
                        if (offset + 1 == chunk_capacity) {
                            // this is the last thing we can push into this block, lets set up the next one
                            assert(next_block != nullptr);
                            const auto next = next_block.release();
                            write_.bptr.store(next, std::memory_order::release);
                            write_.idx.fetch_add(1 << meta_bits,
                                                 std::memory_order::release);
                            // point the old block we are writing to to the new block we just created
                            target->next.store(next,
                                               std::memory_order::release);
                            target->next.notify_all();
                        }
                        // now we can finally actually write the new entry
                        entry& ent = target->entries[offset];
                        ent.cell.write(std::forward<Args>(args)...);
                        // mark the fact we've written to this entry
                        ent.state.mark_written();
                        return true;
                    }
                    else {
                        target = write_.bptr.load(std::memory_order::acquire);
                        spinner.cpu_yield();
                    }
                }
            }
        }
        auto size() const -> size_type
        {
            while (true) {
                auto rd = read_.idx.load();
                auto wd = write_.idx.load();

                if (read_.idx.load() == rd) {
                    // consistent indexes, ok cool

                    // remove the metadata
                    rd &= ~((1 << meta_bits) - 1);
                    wd &= ~((1 << meta_bits) - 1);

                    auto fixup = [](auto c) {
                        if (((c >> meta_bits) & chunk_capacity) ==
                            chunk_capacity) {
                            return c + (1 << meta_bits);
                        }
                        else {
                            return c;
                        }
                    };
                    rd = fixup(rd);
                    wd = fixup(wd);

                    const auto lap = (rd >> meta_bits) / chunk_size;
                    rd -= (lap * chunk_size) << meta_bits;
                    wd -= (lap * chunk_size) << meta_bits;

                    rd >>= meta_bits;
                    wd >>= meta_bits;

                    return wd - rd - wd / chunk_size;
                }
            }
        }
        auto pop() -> std::optional<T>
        {
            ::transbeam::__detail::backoff spinner;
            auto rd = read_.idx.load(std::memory_order::acquire);
            auto rblock = read_.bptr.load(std::memory_order::acquire);
            while (true) {
                const auto chunk_idx = (rd >> meta_bits) % chunk_size;
                if (chunk_idx == chunk_capacity) {
                    spinner.thread_yield();
                    // we're at the end of this chunk
                    rd = read_.idx.load(std::memory_order::acquire);
                    rblock = read_.bptr.load(std::memory_order::acquire);
                    continue;
                }

                auto next_rd = rd + (1 << meta_bits);

                // if the meta bit is 0 on read then we are the last block
                if ((next_rd & meta_bit) == 0) {
                    // we need this fence to ensure our read load doesn't end up before this write load
                    std::atomic_thread_fence(std::memory_order_seq_cst);
                    const auto wd = write_.idx.load(std::memory_order::relaxed);
                    const auto rd_raw_off = rd >> meta_bits;
                    const auto wd_raw_off = wd >> meta_bits;

                    if (rd_raw_off == wd_raw_off) {
                        // we are the last block and write = read so we are empty
                        return std::nullopt;
                    }

                    if ((rd_raw_off / chunk_size) !=
                        (wd_raw_off / chunk_size)) {
                        // turns out we actually arn't the last block, better fix that
                        next_rd |= meta_bit;
                    }
                }

                if (rblock == nullptr) {
                    // the only way this is null but write is ahead (as checked earlier) is if the next block
                    // is currently being created, so just wait for it
                    spinner.thread_yield();

                    rd = read_.idx.load(std::memory_order::acquire);
                    rblock = read_.bptr.load(std::memory_order::acquire);
                    continue;
                }
                if (read_.idx.compare_exchange_weak(
                        rd, next_rd, std::memory_order::seq_cst,
                        std::memory_order::acquire)) {
                    if (chunk_idx + 1 == chunk_capacity) {
                        // we are reading the last of this block, move the read position on to the next one
                        const auto next = rblock->wait_for_next();
                        auto next_idx =
                            (next_rd & ~meta_bit) + (1 << meta_bits);
                        if (next->next.load(std::memory_order::relaxed) !=
                            nullptr) {
                            // there is a block after our one so make sure we set that
                            next_idx |= meta_bit;
                        }

                        read_.bptr.store(next, std::memory_order::release);
                        read_.idx.store(next_idx, std::memory_order::release);
                    }
                    // now we can actually read it out
                    entry& e = rblock->entries[chunk_idx];
                    // we need to make sure the write on this entry has fully finished so we don't race with it
                    e.state.ensure_write_flag();
                    auto item = T{static_cast<T&&>(*e.cell.read())};
                    std::destroy_at(e.cell.read());

                    if (chunk_idx + 1 == chunk_capacity) {
                        // we are the last in this block, lets try destroying everything
                        destroy_block(rblock, 0);
                    }
                    else if (e.state.mark_read_and_check_destroy()) {
                        // we've been marked for destruction, carry on the work of destroying the block
                        destroy_block(rblock, chunk_idx + 1);
                    }
                    return item;
                }
                else {
                    rblock = read_.bptr.load(std::memory_order::acquire);
                    spinner.cpu_yield();
                }
            }
        }

    private:
        void destroy_block(block* b, size_type start)
        {
            for (size_type n = start; n != chunk_capacity - 1; ++n) {
                entry& e = b->entries[n];
                if (!e.state.read_bit_set()) {
                    // a thread is still using this, set the destroy bit
                    if (!e.state.mark_destroy_and_check_read()) {
                        // the thread is _still_ using it so it will see the destroy bit and
                        // continue destroying this block for us
                        return;
                    }
                }
            }
            // we're home clear, we can remove the whole block
            delete b;
        }
        index_type write_;
        index_type read_;
    };
    constexpr auto slot_written_bit = 1 << 0;
    constexpr auto slot_lap_bit = 1 << 1;

    constexpr auto take_top_two_bits(std::size_t s) -> uint8_t
    {
        return static_cast<uint8_t>(s >> (8 * sizeof(std::size_t) - 2));
    }

    constexpr std::size_t mark_bit_{static_cast<std::size_t>(1)
                                    << (8 * sizeof(std::size_t) - 2)};
    constexpr std::size_t one_lap_{mark_bit_ << 1};
    constexpr std::size_t signet_lap_shift = (8 * sizeof(std::size_t) - 2);

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
            std::atomic_uint8_t stamp;
            ::transbeam::__detail::util::lazy_init<T> entry;
        };

    public:
        constexpr explicit bounded_ringbuf(size_type capacity)
            : buf_(new slot[capacity]), capacity_{capacity}
        {
            if (capacity == std::numeric_limits<size_type>::max() ||
                capacity == 0) {
                throw std::logic_error{
                    "invalid capacity, too big or too small"};
            }
        }
        bounded_ringbuf(bounded_ringbuf&&) = delete;
        bounded_ringbuf(const bounded_ringbuf&) = delete;

        constexpr ~bounded_ringbuf()
        {
            // it is _not_ safe for multiple threads to still have a handle on us at this point
            // so don't bother handling that case
            const auto rd = read_.load();
            const auto ridx = rd & (mark_bit_ - 1);
            const auto len = size();
            for (size_type n = 0; n != len; ++n) {
                const auto idx = (ridx + n) % capacity_;
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
                    return widx > ridx   ? widx - ridx
                           : ridx > widx ? capacity_ - widx + ridx
                           : (wd & ~mark_bit_) == (rd & ~mark_bit_) ? 0
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
            ::transbeam::__detail::backoff spinner;
            auto wd = write_.load(std::memory_order_relaxed);

            while (true) {
                const auto idx = wd & (mark_bit_ - 1);
                const auto lap = wd & one_lap_;
                slot& s = buf_[idx];
                const auto stamp = s.stamp.load(std::memory_order_acquire);

                if (((stamp & slot_written_bit) == 0) &&
                    (stamp & slot_lap_bit) == (lap >> signet_lap_shift)) {
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
                        s.stamp.store(take_top_two_bits(next_wd) |
                                          slot_written_bit,
                                      std::memory_order_release);
                        return true;
                    }
                    else {
                        spinner.cpu_yield();
                    }
                }
                else if ((stamp & slot_written_bit) ==
                         (take_top_two_bits(wd - one_lap_) |
                          slot_written_bit)) {
                    // we've come back on ourselves, can't overwrite this
                    std::atomic_thread_fence(std::memory_order_seq_cst);
                    const auto rd = read_.load(std::memory_order_relaxed);
                    if (((rd + one_lap_) & ~mark_bit_) == wd) {
                        // if head is behind by a lap its full
                        return false;
                    }
                    spinner.cpu_yield();
                    wd = write_.load(std::memory_order_relaxed);
                }
                else {
                    // gotta wait for the stamp so yield the thread
                    spinner.thread_yield();
                    wd = write_.load(std::memory_order_relaxed);
                }
            }
            return true;
        }
        auto pop() -> std::optional<T>
        {
            ::transbeam::__detail::backoff spinner;
            auto rd = read_.load(std::memory_order_relaxed);

            while (true) {
                const auto idx = rd & (mark_bit_ - 1);
                const auto lap = rd & ~(one_lap_ - 1);

                slot& s = buf_[idx];
                const auto stamp = s.stamp.load(std::memory_order_acquire);

                // if the write stamp is ahead by 1 we are allowed to read this
                if ((stamp & slot_written_bit) != 0) {
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
                        s.stamp.store(lap > 0 ? 0 : slot_lap_bit,
                                      std::memory_order_release);
                        return ent;
                    }
                    else {
                        spinner.cpu_yield();
                    }
                }
                else if ((stamp & slot_written_bit) == 0) {
                    std::atomic_thread_fence(std::memory_order_seq_cst);
                    const auto wd = write_.load(std::memory_order_relaxed);
                    if ((wd & ~mark_bit_) == (rd & ~mark_bit_)) {
                        // if on same lap and index then we are full
                        return std::nullopt;
                    }
                    spinner.cpu_yield();
                    rd = read_.load(std::memory_order_relaxed);
                }
                else {
                    spinner.thread_yield();
                    rd = read_.load(std::memory_order_relaxed);
                }
            }
        }

    private:
        std::unique_ptr<slot[]> buf_;
        size_type capacity_;
        /// index of the next free location to write to
        std::atomic<size_type> write_{0};
        /// the index of the first element
        std::atomic<size_type> read_{mark_bit_};
    };
    template<typename Backend>
    struct shared_data {
        Backend buf;
        ::transbeam::__detail::util::wait_group writers;
        ::transbeam::__detail::util::wait_group readers;
        template<typename... Args>
        shared_data(Args&&... args) : buf{std::forward<Args>(args)...}
        {
        }
    };

    template<typename T, typename Backend>
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
        auto size() const -> std::size_t { return shared_->buf.size(); }

        auto subscribe() const -> sender<T, Backend>
        {
            return sender<T, Backend>{shared_};
        }

    private:
        receiver(std::shared_ptr<__detail::shared_data<Backend>> shared)
            : shared_{std::move(shared)}
        {
        }

        template<typename E>
        friend auto ::transbeam::mpmc::bounded(std::size_t capacity)
            -> std::pair<::transbeam::mpmc::sync_sender<E>,
                         ::transbeam::mpmc::sync_receiver<E>>;

        template<typename E>
        friend auto ::transbeam::mpmc::unbounded()
            -> std::pair<::transbeam::mpmc::sender<E>,
                         ::transbeam::mpmc::receiver<E>>;
        friend class sender<T, Backend>;

        std::shared_ptr<__detail::shared_data<Backend>> shared_;
    };

    template<typename T, typename Backend>
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
        auto subscribe() const -> receiver<T, Backend>
        {
            return receiver<T, Backend>{shared_};
        }

        auto size() const -> std::size_t { return shared_->buf.size(); }

    private:
        sender(std::shared_ptr<__detail::shared_data<Backend>> shared)
            : shared_{std::move(shared)}
        {
        }
        template<typename E>
        friend auto ::transbeam::mpmc::bounded(std::size_t capacity)
            -> std::pair<::transbeam::mpmc::sync_sender<E>,
                         ::transbeam::mpmc::sync_receiver<E>>;

        template<typename E>
        friend auto ::transbeam::mpmc::unbounded()
            -> std::pair<::transbeam::mpmc::sender<E>,
                         ::transbeam::mpmc::receiver<E>>;
        friend class receiver<T, Backend>;

        std::shared_ptr<__detail::shared_data<Backend>> shared_;
    };

} // namespace __detail

template<typename T>
auto bounded(std::size_t capacity)
    -> std::pair<sync_sender<T>, sync_receiver<T>>
{
    static_assert(std::move_constructible<T>,
                  "channels require move constructible types");
    auto shared =
        std::make_shared<__detail::shared_data<__detail::bounded_ringbuf<T>>>(
            capacity);
    return std::pair{sync_sender<T>{shared},
                     sync_receiver<T>{std::move(shared)}};
}

template<typename T>
auto unbounded() -> std::pair<sender<T>, receiver<T>>
{
    static_assert(std::move_constructible<T>,
                  "channels require move constructible types");
    auto shared =
        std::make_shared<__detail::shared_data<__detail::linked_list<T>>>();
    return std::pair{sender<T>{shared}, receiver<T>{std::move(shared)}};
}

static_assert(std::is_copy_constructible_v<sync_receiver<int>>);
static_assert(std::is_move_constructible_v<sync_receiver<int>>);
static_assert(std::is_copy_assignable_v<sync_receiver<int>>);
static_assert(std::is_move_assignable_v<sync_receiver<int>>);

static_assert(std::is_copy_constructible_v<sync_sender<int>>);
static_assert(std::is_move_constructible_v<sync_sender<int>>);
static_assert(std::is_copy_assignable_v<sync_sender<int>>);
static_assert(std::is_move_assignable_v<sync_sender<int>>);
} // namespace transbeam::mpmc
