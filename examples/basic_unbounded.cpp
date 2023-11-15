#include <iostream>
#include <stop_token>
#include <thread>
#include <transbeam/mpmc.hpp>

using namespace transbeam;

int main()
{
    auto [tx, rx] = mpmc::unbounded<int>();
    std::jthread t{[tx](std::stop_token tkn) mutable {
        std::size_t n = 0;
        while (!tkn.stop_requested()) {
            tx.send(n);
            ++n;
        }
    }};
    for (std::size_t n = 0; n != 10; ++n) {
        std::cout << rx.recv() << '\n';
    }
    t.request_stop();
    t.join();
    std::cout << "joined, lets see whats left\n";
    while (true) {
        auto r = rx.try_recv();
        if (!r.has_value()) {
            break;
        }
        std::cout << *r << '\n';
    }
    std::cout << "thats all folks";
}
