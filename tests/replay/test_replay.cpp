// Determinism tests: journal round trip, replay hash, golden hash and threaded == single.
#include <gtest/gtest.h>

#include <cstdio>
#include <string>
#include <vector>

#include "aether/book/order_book.hpp"
#include "aether/journal/event_log.hpp"
#include "aether/journal/hash.hpp"
#include "aether/reference/ref_order_book.hpp"
#include "aether/runtime/pipeline.hpp"
#include "aether/sim/load_gen.hpp"

using namespace aether;

namespace {

// Pinned output of make_stream(20261120, 200'000, 4). See the GoldenHash test.
constexpr std::uint64_t GOLDEN_EVENTS = 362'903;
constexpr std::uint64_t GOLDEN_HASH   = 6383623603604726376ULL;

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

// Three threads and two rings must not change a single output event.
TEST(Replay, ThreadedPipelineMatchesSingleThreaded) {
    const Stream stream   = make_stream(7, 100'000, 3);
    const auto   single   = run_single_threaded<OrderBook>(stream.instruments, stream.commands);
    const auto   threaded = run_pipeline<OrderBook>(stream.instruments, stream.commands);
    EXPECT_EQ(threaded.hash, single.hash);
    EXPECT_EQ(threaded.events, single.events);

    // A tiny ring forces constant full/empty hand-offs between the threads.
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

// Golden hash: the output of a fixed, seeded stream is pinned. If any change to the engine,
// the books or the generator alters behaviour, this fails. Update the constants only when
// the behaviour change is intended.
TEST(Replay, GoldenHash) {
    const Stream stream = make_stream(20261120, 200'000, 4);
    const auto   result = run_single_threaded<OrderBook>(stream.instruments, stream.commands);
    EXPECT_EQ(result.events, GOLDEN_EVENTS);
    EXPECT_EQ(result.hash, GOLDEN_HASH);
}

} // namespace
