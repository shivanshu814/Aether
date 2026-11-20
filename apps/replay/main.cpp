#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <exception>
#include <vector>

#include "aether/book/order_book.hpp"
#include "aether/journal/event_log.hpp"
#include "aether/reference/ref_order_book.hpp"
#include "aether/runtime/pipeline.hpp"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <log file> [--reference] [--threaded]\n", argv[0]);
        return 2;
    }
    bool use_reference = false;
    bool threaded      = false;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--reference") == 0) use_reference = true;
        if (std::strcmp(argv[i], "--threaded") == 0) threaded = true;
    }

    try {
        aether::LogReader            reader(argv[1]);
        std::vector<aether::Command> commands;
        aether::Command              c{};
        while (reader.next(c)) commands.push_back(c);

        aether::PipelineResult result{};
        if (use_reference) {
            result = threaded
                         ? aether::run_pipeline<aether::RefOrderBook>(reader.instruments(), commands)
                         : aether::run_single_threaded<aether::RefOrderBook>(reader.instruments(), commands);
        } else {
            result = threaded
                         ? aether::run_pipeline<aether::OrderBook>(reader.instruments(), commands)
                         : aether::run_single_threaded<aether::OrderBook>(reader.instruments(), commands);
        }
        std::printf("instruments: %zu\n", reader.instruments().size());
        std::printf("commands:    %zu\n", commands.size());
        std::printf("events:      %" PRIu64 "\n", result.events);
        std::printf("hash:        0x%016" PRIx64 "\n", result.hash);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
