
#include <array>
#include <format>
#include <optional>
#include <ostream>
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
            CHECK(rb.pop() == std::optional{5});
            CHECK(rb.empty());
        }
    }
}
