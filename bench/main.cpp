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
#include "aether/runtime/sharded.hpp"
#include "aether/sim/load_gen.hpp"

namespace {
using Clock = std::chrono::steady_clock;

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
    std::uint64_t allocs          = 0;
    std::uint64_t events          = 0;
    std::uint64_t checksum        = 0;
    std::uint64_t p50 = 0, p99 = 0, p999 = 0, max = 0;
};

// One untimed warm-up pass, then timed passes on a fresh engine (median reported), then a
// separate latency pass with the clock around every command. Allocations are counted inside
// the timed loop only; what the engine allocates at construction is excluded on purpose.
template <class Book>
Result measure(const char* name, const std::vector<aether::BookConfig>& instruments,
               const std::vector<aether::Command>& commands, int runs) {
    Result result{};
    result.name = name;

    {
        aether::Engine<Book> engine(instruments);
        CountingSink         sink;
        engine.process_batch(commands, sink);
    }

    std::vector<double> rates;
    for (int run = 0; run < runs; ++run) {
        aether::Engine<Book> engine(instruments);
        CountingSink         sink;
        const std::uint64_t  allocs_before = aether::bench::allocation_count();
        const auto           start         = Clock::now();
        engine.process_batch(commands, sink);
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

    {
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

void scaling(const aether::sim::LoadConfig& base, int max_shards, int runs, bool pin) {
    const aether::BookConfig                      book = aether::sim::book_config_for(base);
    std::vector<std::vector<aether::Command>>     all_streams;
    for (int s = 0; s < max_shards; ++s) {
        aether::sim::LoadConfig load = base;
        load.seed                    = base.seed + static_cast<std::uint64_t>(s);
        all_streams.push_back(aether::sim::generate(load, book));
    }

    std::printf("\nscaling: one engine thread per shard, %zu commands and %u instrument(s) per shard%s\n",
                base.count, base.instruments,
                pin ? (aether::pin_current_thread(0) ? ", threads pinned to CPUs" : ", pinning requested but not supported on this OS") : "");

    std::printf("| shards | until last shard | sum of shard rates | slowest shard | fastest shard | allocations |\n");
    std::printf("|--------|------------------|--------------------|---------------|---------------|-------------|\n");
    for (int n = 1; n <= max_shards; ++n) {
        const std::vector<std::vector<aether::Command>> streams(all_streams.begin(), all_streams.begin() + n);
        const std::vector<std::vector<aether::BookConfig>> instruments(
            static_cast<std::size_t>(n), std::vector<aether::BookConfig>(base.instruments, book));
        const double total = static_cast<double>(base.count) * n;

        aether::run_sharded<aether::OrderBook, CountingSink>(instruments, streams, pin);
        struct Pass {
            double until_last = 0, sum_rates = 0, slowest = 0, fastest = 0;
        };
        std::vector<Pass> passes;
        std::uint64_t     allocs = 0;
        for (int run = 0; run < runs; ++run) {
            const std::uint64_t before = aether::bench::allocation_count();
            const auto result = aether::run_sharded<aether::OrderBook, CountingSink>(instruments, streams, pin);
            const std::uint64_t with_work = aether::bench::allocation_count() - before;

            const std::vector<std::vector<aether::Command>> empty(static_cast<std::size_t>(n));
            const std::uint64_t before_empty = aether::bench::allocation_count();
            aether::run_sharded<aether::OrderBook, CountingSink>(instruments, empty, pin);
            const std::uint64_t setup_only = aether::bench::allocation_count() - before_empty;
            allocs = with_work > setup_only ? with_work - setup_only : 0;

            Pass pass;
            pass.until_last = total / result.seconds;
            pass.slowest    = 1e300;
            for (const double seconds : result.shard_seconds) {
                const double rate = static_cast<double>(base.count) / seconds;
                pass.sum_rates += rate;
                pass.slowest = std::min(pass.slowest, rate);
                pass.fastest = std::max(pass.fastest, rate);
            }
            passes.push_back(pass);
        }
        std::sort(passes.begin(), passes.end(),
                  [](const Pass& a, const Pass& b) { return a.until_last < b.until_last; });
        const Pass& median = passes[passes.size() / 2];
        std::printf("| %6d | %14.1f M | %16.1f M | %11.1f M | %11.1f M | %11" PRIu64 " |\n", n,
                    median.until_last / 1e6, median.sum_rates / 1e6, median.slowest / 1e6,
                    median.fastest / 1e6, allocs);
    }
}

void print(const Result& r) {
    std::printf("| %-22s | %10.2f M | %6" PRIu64 " | %6" PRIu64 " | %7" PRIu64 " | %9" PRIu64
                " | %12.3f |\n",
                r.name, r.orders_per_sec / 1e6, r.p50, r.p99, r.p999, r.max, r.allocs_per_cmd);
}
}

int main(int argc, char** argv) {
    aether::sim::LoadConfig load{};
    load.count = 5'000'000;
    int runs   = 5;
    int shards = 0;
    bool pin   = false;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (std::strcmp(argv[i], "--count") == 0) load.count = std::strtoull(argv[i + 1], nullptr, 10);
        if (std::strcmp(argv[i], "--instruments") == 0)
            load.instruments = static_cast<std::uint32_t>(std::strtoul(argv[i + 1], nullptr, 10));
        if (std::strcmp(argv[i], "--seed") == 0) load.seed = std::strtoull(argv[i + 1], nullptr, 10);
        if (std::strcmp(argv[i], "--runs") == 0) runs = std::atoi(argv[i + 1]);
        if (std::strcmp(argv[i], "--shards") == 0) shards = std::atoi(argv[i + 1]);
        if (std::strcmp(argv[i], "--pin") == 0) pin = std::atoi(argv[i + 1]) != 0;
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

    if (shards > 0) scaling(load, shards, runs, pin);
    return 0;
}
