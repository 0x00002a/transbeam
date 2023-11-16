
#include <array>
#include <chrono>
#include <concepts>
#include <latch>
#include <optional>
#include <ostream>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <vector>

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <transbeam/mpmc.hpp>

auto on_threads(std::size_t n, auto f)
{
    std::vector<std::jthread> threads;
    for (std::size_t c = 0; c != n; ++c) {
        threads.emplace_back([&f] { f(); });
    }
    return threads;
}
namespace std {

template<typename T>
auto& operator<<(std::ostream& o, std::optional<T> v)
{
    if (v) {
        return o << *v;
    }
    else {
        return o << "nullopt";
    }
}

} // namespace std

struct ctor_tester {
    int* dtor_calls;
    int* move_calls;
    int* copy_calls;

    ctor_tester(int* dtor, int* mov, int* cpy)
        : dtor_calls{dtor}, move_calls{mov}, copy_calls{cpy}
    {
    }
    ~ctor_tester()
    {
        if (dtor_calls != nullptr) {
            (*dtor_calls)++;
        }
    }
    ctor_tester(const ctor_tester& o)
        : ctor_tester{o.dtor_calls, o.move_calls, o.copy_calls}
    {
        if (copy_calls) {
            (*copy_calls)++;
        }
    }

    ctor_tester(ctor_tester&& o)
        : ctor_tester{o.dtor_calls, o.move_calls, o.copy_calls}
    {
        if (move_calls) {
            (*move_calls)++;
        }
    }
};
void dtors_called_inplace_test(auto tx, auto rx)
{
    int dtor_calls{0};
    int mov{0};
    int cpy{0};
    tx.send(&dtor_calls, &mov, &cpy);
    CHECK(dtor_calls == 0);
    CHECK(mov == 0);
    CHECK(cpy == 0);
    int prev_dtors;
    {
        auto m = rx.recv();
        CHECK(dtor_calls == mov);
        prev_dtors = dtor_calls;
        CHECK(cpy == 0);
    }
    CHECK(dtor_calls == prev_dtors + 1);
    CHECK(cpy == 0);
}

struct immovable {
    immovable(immovable&&) = delete;
    immovable(const immovable&) = default;
    immovable() = default;
};
static_assert(!std::move_constructible<immovable>);

TEST_SUITE("mpmc")
{
    TEST_SUITE("bounded_ringbuf")
    {
        TEST_CASE("simple case acts correctly")
        {
            transbeam::mpmc::__detail::bounded_ringbuf<int> rb{1};
            REQUIRE(rb.try_emplace(3));
            REQUIRE(rb.size() == rb.max_size());
            REQUIRE(rb.pop() == std::optional{3});
        }
        TEST_CASE("single threaded buffered case")
        {
            transbeam::mpmc::__detail::bounded_ringbuf<int> rb{2};
            CHECK(rb.try_emplace(3));
            CHECK(rb.try_emplace(5));
            CHECK(!rb.try_emplace(4));
            CHECK(rb.pop() == std::optional{3});
            CHECK(rb.try_emplace(6));
            CHECK(rb.size() == rb.max_size());
            CHECK(rb.pop() == std::optional{5});
            CHECK(rb.try_emplace(7));
            CHECK(rb.size() == rb.max_size());
            CHECK(rb.pop() == std::optional{6});
            CHECK(rb.pop() == std::optional{7});
            CHECK(rb.empty());
        }
        TEST_CASE("multi threaded unbuffered doesn't race")
        {
            transbeam::mpmc::__detail::bounded_ringbuf<int> rb{1};
            std::latch ready{3};
            std::jthread t1{[&](std::stop_token stop) mutable {
                ready.arrive_and_wait();
                while (!stop.stop_requested()) {
                    rb.try_emplace(1);
                }
            }};

            std::jthread t2{[&](auto stop) mutable {
                ready.arrive_and_wait();
                while (!stop.stop_requested()) {
                    rb.try_emplace(2);
                }
            }};

            std::jthread t3{[&](auto stop) mutable {
                ready.arrive_and_wait();
                while (!stop.stop_requested()) {
                    rb.try_emplace(3);
                }
            }};
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            REQUIRE(rb.max_size() == rb.size());
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            REQUIRE(rb.pop());
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            REQUIRE(rb.pop());
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            REQUIRE(!rb.try_emplace(6));
        }
        TEST_CASE(
            "constructing a ringbuf doesn't call constructors of elements")
        {
            static bool constructed = false;
            struct thing {
                thing() { constructed = true; }
            };
            transbeam::mpmc::__detail::bounded_ringbuf<thing> rb{30};
            REQUIRE(!constructed);
            rb.try_emplace();
            REQUIRE(constructed);
        }
        TEST_CASE("destroying a ringbuf calls destructors")
        {
            struct thing {
                bool* destroyed;
                thing(bool* destroyed) : destroyed{destroyed}
                {
                    REQUIRE(destroyed != nullptr);
                    CHECK(!*destroyed);
                }
                ~thing() { *destroyed = true; }
            };
            bool d1{false};
            bool d2{false};
            {
                transbeam::mpmc::__detail::bounded_ringbuf<thing> rb{10};
                rb.try_emplace(&d1);
                rb.try_emplace(&d2);
            }
            CHECK(d1);
            CHECK(d2);
        }
    }
    TEST_SUITE("bounded channel")
    {
        using namespace transbeam;
        TEST_CASE("sending on one thread")
        {
            auto [tx, rx] = mpmc::bounded<int>(1);
            tx.send(5);
            CHECK(!tx.try_send(3));
            REQUIRE(rx.recv() == 5);
        }
        TEST_CASE("multithreaded wakes up waiting read thread")
        {
            int read{0};

            auto [tx, rx] = mpmc::bounded<int>(1);
            {
                std::jthread rd{[&read, rx]() mutable { read = rx.recv(); }};
                CHECK(read == 0);
                tx.send(1);
            }
            CHECK(read == 1);
        }

        TEST_CASE("multithreaded wakes up waiting write thread")
        {

            auto [tx, rx] = mpmc::bounded<int>(1);
            tx.send(1);
            {
                std::jthread wd{[tx]() mutable { tx.send(2); }};
                CHECK(rx.recv() == 1);
            }
            CHECK(rx.recv() == 2);
        }
        TEST_CASE("channel can be resubscribed")
        {
            auto [tx, rx] = mpmc::bounded<int>(1);

            tx.subscribe();
            rx.subscribe();
        }

        TEST_CASE("dtors in place")
        {
            auto [tx, rx] = mpmc::bounded<ctor_tester>(1);
            dtors_called_inplace_test(tx, rx);
        }
    }
    TEST_SUITE("unbounded")
    {
        using namespace transbeam;
        TEST_CASE("simple single thread case")
        {
            auto [tx, rx] = mpmc::unbounded<int>();
            tx.send(5);
            CHECK(rx.recv() == 5);
        }
        TEST_CASE("destructors are called")
        {
            struct thing {
                bool* destroyed;
                thing(bool* destroyed) : destroyed{destroyed}
                {
                    REQUIRE(destroyed != nullptr);
                    CHECK(!*destroyed);
                }
                ~thing() { *destroyed = true; }
            };
            bool d1{false};
            bool d2{false};
            {
                auto [tx, rx] = mpmc::unbounded<thing>();
                tx.send(&d1);
                tx.send(&d2);
                CHECK(!d1);
                CHECK(!d2);
            }
            CHECK(d1);
            CHECK(d2);
        }
        TEST_CASE("try recv doesn't block if empty")
        {
            auto [tx, rx] = mpmc::unbounded<int>();
            CHECK(rx.try_recv() == std::nullopt);
            tx.send(5);
            rx.recv();
            CHECK(rx.try_recv() == std::nullopt);
        }
        TEST_CASE("all messages sent can be read")
        {
            constexpr auto nb_threads = 3;
            constexpr auto to_send = nb_threads * 100;
            auto [tx, rx] = mpmc::unbounded<int>();
            {
                std::vector<std::jthread> threads;
                for (std::size_t t = 0; t != nb_threads; ++t) {
                    threads.emplace_back([t, tx]() mutable {
                        const auto scale = to_send / nb_threads;
                        const auto start = t * scale;
                        const auto end = (t + 1) * scale;

                        for (std::size_t n = start; n != end; ++n) {
                            tx.send(n);
                        }
                    });
                }
            }
            REQUIRE(tx.size() == rx.size());
            REQUIRE(tx.size() == to_send);
            std::vector<int> got;
            got.reserve(to_send);
            for (std::size_t n = 0; n != to_send; ++n) {
                const auto m = rx.recv();
                CHECK(std::find(got.begin(), got.end(), m) == got.end());
                got.emplace_back(m);
            }

            for (std::size_t n = 0; n != to_send; ++n) {
                CHECK(std::find(got.begin(), got.end(), n) != got.end());
            }
        }
        TEST_CASE("dtors in place")
        {
            auto [tx, rx] = mpmc::unbounded<ctor_tester>();
            dtors_called_inplace_test(tx, rx);
        }
    }
}
