#include <cassert>
#include <iostream>
#include <thread>
#include <transbeam/mpmc.hpp>

using namespace transbeam;

int main()
{
    auto [tx, rx] = mpmc::bounded<int>(10);
    std::jthread t{[tx]() mutable {
        for (std::size_t n = 0; n != 10; ++n) {
            tx.send(n);
        }
    }};
    for (std::size_t n = 0; n != 10; ++n) {
        std::cout << rx.recv() << '\n';
    }
}
