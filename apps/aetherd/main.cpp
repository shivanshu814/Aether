// aetherd: run the full threaded exchange on generated order flow.
//
//   aetherd [--count N] [--instruments N] [--seed N] [--log FILE]
//
// gateway/sequencer (this thread) -> SPSC ring -> engine thread -> SPSC ring -> publisher.
// With --log, every command is journaled before the engine sees it; replay the file with
// aether_replay and the hash printed here must match.
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <vector>

#include "aether/book/order_book.hpp"
#include "aether/runtime/pipeline.hpp"
#include "aether/sim/load_gen.hpp"

int main(int argc, char** argv) {
    aether::sim::LoadConfig load{};
    load.instruments = 4;
    std::string log_path;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (std::strcmp(argv[i], "--count") == 0) load.count = std::strtoull(argv[i + 1], nullptr, 10);
        if (std::strcmp(argv[i], "--instruments") == 0)
            load.instruments = static_cast<std::uint32_t>(std::strtoul(argv[i + 1], nullptr, 10));
        if (std::strcmp(argv[i], "--seed") == 0) load.seed = std::strtoull(argv[i + 1], nullptr, 10);
        if (std::strcmp(argv[i], "--log") == 0) log_path = argv[i + 1];
    }
    if (load.count == 0 || load.instruments == 0) {
        std::fprintf(stderr, "--count and --instruments must be positive\n");
        return 2;
    }

    try {
        const aether::BookConfig              book = aether::sim::book_config_for(load);
        const std::vector<aether::BookConfig> instruments(load.instruments, book);
        const std::vector<aether::Command>    commands = aether::sim::generate(load, book);

        std::unique_ptr<aether::LogWriter> journal;
        if (!log_path.empty()) journal = std::make_unique<aether::LogWriter>(log_path, instruments);

        const auto start  = std::chrono::steady_clock::now();
        const auto result = aether::run_pipeline<aether::OrderBook>(instruments, commands, journal.get());
        const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;
        if (journal) journal->close();

        std::printf("instruments: %u\n", load.instruments);
        std::printf("commands:    %zu\n", commands.size());
        std::printf("events:      %" PRIu64 "\n", result.events);
        std::printf("hash:        0x%016" PRIx64 "\n", result.hash);
        std::printf("end-to-end:  %.2f M commands/sec through 3 threads%s\n",
                    static_cast<double>(commands.size()) / elapsed.count() / 1e6,
                    journal ? " (journaled)" : "");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
