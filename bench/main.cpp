// aether_bench: measure the three headline numbers for the reference and the fast engine.
//
//   aether_bench [--count N] [--instruments N] [--seed N] [--runs N]
//
// For each engine:
//   1. warm-up pass over the whole stream (result discarded)
//   2. throughput: N timed passes with no per-command clock; median orders/sec.
//      Heap allocations are counted inside the timed loop only.
//   3. latency: one pass with steady_clock around every command, into a fixed histogram.
// Commands are generated up front from a seeded RNG, so the generator is never timed.
#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "alloc_counter.hpp"
#include "histogram.hpp"

#include "aether/book/order_book.hpp"
#include "aether/engine/engine.hpp"
#include "aether/reference/ref_order_book.hpp"
#include "aether/sim/load_gen.hpp"

namespace {

using Clock = std::chrono::steady_clock;

// Cheapest possible sink: the engine still has to build every event, but nothing is stored.
struct CountingSink {
    std::uint64_t events   = 0;
    std::uint64_t checksum = 0;
    void emit(const aether::Event& e) noexcept {
        ++events;
        checksum += static_cast<std::uint64_t>(e.qty) + e.order_id;
    }
};

struct Result {
    const char*   name            = "";
    double        orders_per_sec  = 0;
    double        allocs_per_cmd  = 0;
    std::uint64_t allocs          = 0; // heap allocations inside one timed pass
    std::uint64_t events          = 0;
    std::uint64_t checksum        = 0;
    std::uint64_t p50 = 0, p99 = 0, p999 = 0, max = 0;
};

template <class Book>
Result measure(const char* name, const std::vector<aether::BookConfig>& instruments,
               const std::vector<aether::Command>& commands, int runs) {
    Result result{};
    result.name = name;

    { // warm-up: fault in pages, warm caches and the branch predictor
        aether::Engine<Book> engine(instruments);
        CountingSink         sink;
        for (const aether::Command& c : commands) engine.process(c, sink);
    }

    std::vector<double> rates;
    for (int run = 0; run < runs; ++run) {
        aether::Engine<Book> engine(instruments); // all up-front allocation happens here
        CountingSink         sink;
        const std::uint64_t  allocs_before = aether::bench::allocation_count();
        const auto           start         = Clock::now();
        for (const aether::Command& c : commands) engine.process(c, sink);
        const std::chrono::duration<double> elapsed = Clock::now() - start;
        const std::uint64_t allocs = aether::bench::allocation_count() - allocs_before;

        rates.push_back(static_cast<double>(commands.size()) / elapsed.count());
        result.allocs         = allocs;
        result.allocs_per_cmd = static_cast<double>(allocs) / static_cast<double>(commands.size());
        result.events         = sink.events;
        result.checksum       = sink.checksum;
    }
    std::sort(rates.begin(), rates.end());
    result.orders_per_sec = rates[rates.size() / 2];

    { // latency pass
        auto                 histogram = std::make_unique<aether::bench::Histogram>();
        aether::Engine<Book> engine(instruments);
        CountingSink         sink;
        for (const aether::Command& c : commands) {
            const auto t0 = Clock::now();
            engine.process(c, sink);
            const auto t1 = Clock::now();
            histogram->record(static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
        }
        result.p50  = histogram->percentile(50.0);
        result.p99  = histogram->percentile(99.0);
        result.p999 = histogram->percentile(99.9);
        result.max  = histogram->max();
    }
    return result;
}

// What two back-to-back clock reads cost; every latency sample includes about this much.
std::uint64_t clock_overhead_ns() {
    aether::bench::Histogram h;
    for (int i = 0; i < 1'000'000; ++i) {
        const auto t0 = Clock::now();
        const auto t1 = Clock::now();
        h.record(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
    }
    return h.percentile(50.0);
}

// Smallest step the clock can actually report. Latency samples are multiples of this.
std::uint64_t clock_tick_ns() {
    std::uint64_t smallest = ~std::uint64_t{0};
    for (int i = 0; i < 1'000'000; ++i) {
        const auto t0 = Clock::now();
        const auto t1 = Clock::now();
        const auto gap = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        if (gap != 0 && gap < smallest) smallest = gap;
    }
    return smallest;
}

void print(const Result& r) {
    std::printf("| %-22s | %10.2f M | %6" PRIu64 " | %6" PRIu64 " | %7" PRIu64 " | %9" PRIu64
                " | %12.3f |\n",
                r.name, r.orders_per_sec / 1e6, r.p50, r.p99, r.p999, r.max, r.allocs_per_cmd);
}

} // namespace

int main(int argc, char** argv) {
    aether::sim::LoadConfig load{};
    load.count = 5'000'000;
    int runs   = 5;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (std::strcmp(argv[i], "--count") == 0) load.count = std::strtoull(argv[i + 1], nullptr, 10);
        if (std::strcmp(argv[i], "--instruments") == 0)
            load.instruments = static_cast<std::uint32_t>(std::strtoul(argv[i + 1], nullptr, 10));
        if (std::strcmp(argv[i], "--seed") == 0) load.seed = std::strtoull(argv[i + 1], nullptr, 10);
        if (std::strcmp(argv[i], "--runs") == 0) runs = std::atoi(argv[i + 1]);
    }
    if (load.count == 0 || load.instruments == 0 || runs <= 0) {
        std::fprintf(stderr, "--count, --instruments and --runs must be positive\n");
        return 2;
    }

    const aether::BookConfig              book = aether::sim::book_config_for(load);
    const std::vector<aether::BookConfig> instruments(load.instruments, book);
    const std::vector<aether::Command>    commands = aether::sim::generate(load, book);

    std::printf("commands: %zu   instruments: %u   seed: %" PRIu64 "   runs: %d\n", commands.size(),
                load.instruments, load.seed, runs);
    std::printf("compiler: %s\n", __VERSION__);
    std::printf("steady_clock: median gap between two reads %" PRIu64 " ns, smallest non-zero gap %" PRIu64
                " ns (latencies are quantised to this)\n\n",
                clock_overhead_ns(), clock_tick_ns());

    const Result reference = measure<aether::RefOrderBook>("reference (std::map)", instruments, commands, runs);
    const Result fast      = measure<aether::OrderBook>("fast (pool + array)", instruments, commands, runs);

    std::printf("| engine                 | orders/sec   | p50 ns | p99 ns | p99.9 ns | max ns    | allocs/order |\n");
    std::printf("|------------------------|--------------|--------|--------|----------|-----------|--------------|\n");
    print(reference);
    print(fast);

    std::printf("\nevents per run: %" PRIu64 "\n", fast.events);
    std::printf("heap allocations in one timed pass: reference %" PRIu64 ", fast %" PRIu64 "\n",
                reference.allocs, fast.allocs);
    if (reference.events != fast.events || reference.checksum != fast.checksum) {
        std::printf("ERROR: the two engines produced different output\n");
        return 1;
    }
    std::printf("speed-up: %.1fx throughput\n", fast.orders_per_sec / reference.orders_per_sec);
    return 0;
}
