#include <gtest/gtest.h>

#include <cstdio>
#include <string>
#include <vector>

#include "aether/book/order_book.hpp"
#include "aether/journal/event_log.hpp"
#include "aether/journal/hash.hpp"
#include "aether/reference/ref_order_book.hpp"
#include "aether/runtime/pipeline.hpp"
#include "aether/runtime/sharded.hpp"
#include "aether/sim/load_gen.hpp"

using namespace aether;

namespace {
constexpr std::uint64_t GOLDEN_EVENTS = 357'564;
constexpr std::uint64_t GOLDEN_HASH   = 8394587788805723800ULL;

struct Stream {
    std::vector<BookConfig> instruments;
    std::vector<Command>    commands;
};

Stream make_stream(std::uint64_t seed, std::size_t count, std::uint32_t instrument_count) {
    sim::LoadConfig load{};
    load.seed        = seed;
    load.count       = count;
    load.instruments = instrument_count;
    load.target_live = 2'000;
    const BookConfig book = sim::book_config_for(load);
    return {std::vector<BookConfig>(instrument_count, book), sim::generate(load, book)};
}

std::string temp_log(const char* name) { return ::testing::TempDir() + name; }

TEST(Journal, RecordRoundTripKeepsEveryField) {
    Command c{};
    c.seq        = 123456789;
    c.ts_ns      = 987654321;
    c.order_id   = 42;
    c.price      = 10'050;
    c.qty        = 77;
    c.instrument = 3;
    c.type       = CommandType::Modify;
    c.side       = Side::Sell;
    c.order_type = OrderType::IOC;
    EXPECT_EQ(to_command(to_record(c)), c);
}

TEST(Journal, WriteThenReadGivesBackSameConfigAndCommands) {
    const Stream      stream = make_stream(11, 20'000, 3);
    const std::string path   = temp_log("aether_roundtrip.log");
    {
        LogWriter writer(path, stream.instruments);
        for (const Command& c : stream.commands) writer.append(c);
    }
    LogReader            reader(path);
    std::vector<Command> read_back;
    Command              c{};
    while (reader.next(c)) read_back.push_back(c);
    std::remove(path.c_str());

    EXPECT_EQ(reader.instruments(), stream.instruments);
    EXPECT_EQ(read_back, stream.commands);
}

TEST(Journal, RejectsFilesThatAreNotLogs) {
    const std::string path = temp_log("aether_garbage.log");
    std::FILE*        f    = std::fopen(path.c_str(), "wb");
    ASSERT_NE(f, nullptr);
    std::fputs("this is definitely not an aether log file", f);
    std::fclose(f);
    EXPECT_THROW(LogReader reader(path), std::runtime_error);
    std::remove(path.c_str());
    EXPECT_THROW(LogReader reader(temp_log("aether_missing.log")), std::runtime_error);
}

TEST(Replay, ReplayFromLogReproducesLiveHash) {
    const Stream         stream = make_stream(7, 100'000, 3);
    const PipelineResult live   = run_single_threaded<OrderBook>(stream.instruments, stream.commands);

    const std::string path = temp_log("aether_replay.log");
    {
        LogWriter writer(path, stream.instruments);
        for (const Command& c : stream.commands) writer.append(c);
    }
    LogReader            reader(path);
    std::vector<Command> commands;
    Command              c{};
    while (reader.next(c)) commands.push_back(c);
    std::remove(path.c_str());

    const PipelineResult replayed = run_single_threaded<OrderBook>(reader.instruments(), commands);
    EXPECT_EQ(replayed.hash, live.hash);
    EXPECT_EQ(replayed.events, live.events);
}

TEST(Replay, RunningTwiceGivesTheSameHash) {
    const Stream stream = make_stream(8, 50'000, 2);
    const auto   first  = run_single_threaded<OrderBook>(stream.instruments, stream.commands);
    const auto   second = run_single_threaded<OrderBook>(stream.instruments, stream.commands);
    EXPECT_EQ(first.hash, second.hash);
}

TEST(Replay, ReferenceAndFastEngineHashTheSame) {
    const Stream stream    = make_stream(7, 100'000, 3);
    const auto   reference = run_single_threaded<RefOrderBook>(stream.instruments, stream.commands);
    const auto   fast      = run_single_threaded<OrderBook>(stream.instruments, stream.commands);
    EXPECT_EQ(fast.hash, reference.hash);
    EXPECT_EQ(fast.events, reference.events);
}

TEST(Replay, ThreadedPipelineMatchesSingleThreaded) {
    const Stream stream   = make_stream(7, 100'000, 3);
    const auto   single   = run_single_threaded<OrderBook>(stream.instruments, stream.commands);
    const auto   threaded = run_pipeline<OrderBook>(stream.instruments, stream.commands);
    EXPECT_EQ(threaded.hash, single.hash);
    EXPECT_EQ(threaded.events, single.events);

    const auto squeezed = run_pipeline<OrderBook>(stream.instruments, stream.commands, nullptr, 8);
    EXPECT_EQ(squeezed.hash, single.hash);
}

TEST(Replay, ThreadedPipelineJournalsEveryCommand) {
    const Stream      stream = make_stream(9, 30'000, 2);
    const std::string path   = temp_log("aether_pipeline.log");
    PipelineResult    live{};
    {
        LogWriter writer(path, stream.instruments);
        live = run_pipeline<OrderBook>(stream.instruments, stream.commands, &writer);
    }
    LogReader            reader(path);
    std::vector<Command> commands;
    Command              c{};
    while (reader.next(c)) commands.push_back(c);
    std::remove(path.c_str());

    EXPECT_EQ(commands, stream.commands);
    EXPECT_EQ(run_single_threaded<OrderBook>(reader.instruments(), commands).hash, live.hash);
}

TEST(Sharding, PartitionKeepsPerInstrumentOrderAndRemapsIds) {
    const Stream stream = make_stream(13, 20'000, 5);
    const auto   shards = partition_by_shard(stream.commands, 2);
    ASSERT_EQ(shards.size(), 2u);
    EXPECT_EQ(shards[0].size() + shards[1].size(), stream.commands.size());

    for (std::size_t s = 0; s < 2; ++s) {
        SeqNo last = 0;
        for (const Command& c : shards[s]) {
            EXPECT_LT(c.instrument, s == 0 ? 3u : 2u);
            EXPECT_GT(c.seq, last);
            last = c.seq;
        }
    }
    const auto configs = partition_instruments(stream.instruments, 2);
    EXPECT_EQ(configs[0].size(), 3u);
    EXPECT_EQ(configs[1].size(), 2u);
}

TEST(Sharding, EachShardMatchesItsSingleThreadedRun) {
    const Stream stream  = make_stream(14, 120'000, 6);
    const auto   streams = partition_by_shard(stream.commands, 3);
    const auto   configs = partition_instruments(stream.instruments, 3);

    const auto sharded = run_sharded<OrderBook, HashSink>(configs, streams);
    ASSERT_EQ(sharded.sinks.size(), 3u);
    std::uint64_t total_events = 0;
    for (std::size_t s = 0; s < 3; ++s) {
        const auto fast      = run_single_threaded<OrderBook>(configs[s], streams[s]);
        const auto reference = run_single_threaded<RefOrderBook>(configs[s], streams[s]);
        EXPECT_EQ(sharded.sinks[s].hash(), fast.hash) << "shard " << s;
        EXPECT_EQ(sharded.sinks[s].count, fast.events) << "shard " << s;
        EXPECT_EQ(fast.hash, reference.hash) << "shard " << s;
        total_events += sharded.sinks[s].count;
    }

    EXPECT_EQ(total_events, run_single_threaded<OrderBook>(stream.instruments, stream.commands).events);
}

TEST(Replay, GoldenHash) {
    const Stream stream = make_stream(20261120, 200'000, 4);
    const auto   result = run_single_threaded<OrderBook>(stream.instruments, stream.commands);
    EXPECT_EQ(result.events, GOLDEN_EVENTS);
    EXPECT_EQ(result.hash, GOLDEN_HASH);
}
}
