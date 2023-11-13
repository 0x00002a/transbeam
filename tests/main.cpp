
#include <array>
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
    }
}
