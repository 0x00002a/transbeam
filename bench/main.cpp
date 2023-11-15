
#include "transbeam/mpmc.hpp"
#include <benchmark/benchmark.h>
#include <thread>

constexpr auto ten_thousand = 10'000;

static void four_thread(auto tx)
{
    std::vector<std::jthread> threads;
    for (auto n = 0; n != 4; ++n) {
        threads.emplace_back([tx, i = n]() mutable {
            const auto scale = ten_thousand / 4;
            for (auto n = (i * scale); n != ((i + 1) * scale); ++n) {
                if (!tx.try_send(n)) {
                    std::terminate();
                }
            }
        });
    }
}

static void BM_unbounded_10k_entries_input_single_thread(benchmark::State& s)
{
    for (auto _ : s) {
        auto [tx, rx] = transbeam::mpmc::unbounded<int>();
        for (auto n = 0; n != ten_thousand; ++n) {
            tx.send(n);
        }
    }
}

static void BM_bounded_10k_entries_input_single_thread(benchmark::State& s)
{
    for (auto _ : s) {
        auto [tx, rx] = transbeam::mpmc::bounded<int>(ten_thousand);
        for (auto n = 0; n != ten_thousand; ++n) {
            tx.send(n);
        }
    }
}

static void BM_bounded_10k_entries_input_four_thread(benchmark::State& s)
{
    for (auto _ : s) {
        auto [tx, rx] = transbeam::mpmc::bounded<int>(ten_thousand);
        four_thread(tx);
    }
};

static void BM_unbounded_10k_entries_input_four_thread(benchmark::State& s)
{
    for (auto _ : s) {
        auto [tx, rx] = transbeam::mpmc::unbounded<int>();
        four_thread(tx);
    }
};

BENCHMARK(BM_unbounded_10k_entries_input_single_thread);
BENCHMARK(BM_bounded_10k_entries_input_single_thread);
BENCHMARK(BM_bounded_10k_entries_input_four_thread);
BENCHMARK(BM_unbounded_10k_entries_input_four_thread);

BENCHMARK_MAIN();
