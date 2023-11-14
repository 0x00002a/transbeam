
#include <array>
#include <chrono>
#include <format>
#include <optional>
#include <ostream>
#include <stop_token>
#include <thread>
#include <vector>

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <transbeam/mpmc.hpp>

auto on_threads(std::size_t n, auto f)
{
    std::vector<std::jthread> threads;
    for (auto c = 0; c != n; ++c) {
        threads.emplace_back([&f] { f(); });
    }
    return threads;
}
namespace std {

template<typename T>
auto& operator<<(std::ostream& o, std::optional<T> v)
{
    if (v) {
        return o << std::format("{}", *v);
    }
    else {
        return o << "nullopt";
    }
}

} // namespace std

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
            std::jthread t1{[&](std::stop_token stop) mutable {
                while (!stop.stop_requested()) {
                    rb.try_emplace(1);
                }
            }};

            std::jthread t2{[&](auto stop) mutable {
                while (!stop.stop_requested()) {
                    rb.try_emplace(2);
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
            int read{0};

            auto [tx, rx] = mpmc::bounded<int>(1);
            tx.send(1);
            {
                std::jthread wd{[&read, tx]() mutable { tx.send(2); }};
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
    }
}
