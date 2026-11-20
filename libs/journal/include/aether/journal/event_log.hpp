#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include "aether/core/config.hpp"
#include "aether/core/events.hpp"

// The event log stores the engine's INPUTS only: the instrument configuration, then every
// sequenced command. Outputs are never stored; they are recomputed on replay.
//
// File layout (native byte order):
//   LogHeader
//   InstrumentRecord x header.instrument_count
//   LogRecord        x (until end of file)
namespace aether {

struct LogHeader {
    char          magic[8]         = {'A', 'E', 'T', 'H', 'L', 'O', 'G', '1'};
    std::uint32_t version          = 1;
    std::uint32_t instrument_count = 0;
};
static_assert(sizeof(LogHeader) == 16);

struct InstrumentRecord {
    std::int64_t  min_price   = 0;
    std::int64_t  max_price   = 0;
    std::uint64_t max_orders  = 0;
    std::uint64_t id_capacity = 0;
};
static_assert(sizeof(InstrumentRecord) == 32);

struct LogRecord {
    std::uint64_t seq        = 0;
    std::uint64_t ts_ns      = 0; // from the sequencer
    std::uint8_t  type       = 0; // CommandType
    std::uint8_t  side       = 0;
    std::uint8_t  order_type = 0;
    std::uint8_t  pad        = 0;
    std::uint32_t instrument = 0;
    std::int64_t  price      = 0;
    std::int64_t  qty        = 0;
    std::uint64_t order_id   = 0;
};
static_assert(sizeof(LogRecord) == 48);
static_assert(std::is_trivially_copyable_v<LogRecord>);

constexpr LogRecord to_record(const Command& c) noexcept {
    LogRecord r{};
    r.seq        = c.seq;
    r.ts_ns      = c.ts_ns;
    r.type       = static_cast<std::uint8_t>(c.type);
    r.side       = static_cast<std::uint8_t>(c.side);
    r.order_type = static_cast<std::uint8_t>(c.order_type);
    r.instrument = c.instrument;
    r.price      = c.price;
    r.qty        = c.qty;
    r.order_id   = c.order_id;
    return r;
}

constexpr Command to_command(const LogRecord& r) noexcept {
    Command c{};
    c.seq        = r.seq;
    c.ts_ns      = r.ts_ns;
    c.type       = static_cast<CommandType>(r.type);
    c.side       = static_cast<Side>(r.side);
    c.order_type = static_cast<OrderType>(r.order_type);
    c.instrument = r.instrument;
    c.price      = r.price;
    c.qty        = r.qty;
    c.order_id   = r.order_id;
    return c;
}

class LogWriter {
public:
    LogWriter(const std::string& path, std::span<const BookConfig> instruments)
        : file_(std::fopen(path.c_str(), "wb")) {
        if (file_ == nullptr) throw std::runtime_error("cannot open log for writing: " + path);
        std::setvbuf(file_, nullptr, _IOFBF, std::size_t{1} << 20); // 1 MiB: fewer write() calls
        LogHeader header{};
        header.instrument_count = static_cast<std::uint32_t>(instruments.size());
        write(&header, sizeof(header));
        for (const BookConfig& cfg : instruments) {
            const InstrumentRecord rec{cfg.min_price, cfg.max_price, cfg.max_orders, cfg.id_capacity};
            write(&rec, sizeof(rec));
        }
    }
    ~LogWriter() { close(); }
    LogWriter(const LogWriter&)            = delete;
    LogWriter& operator=(const LogWriter&) = delete;

    void append(const Command& c) {
        const LogRecord rec = to_record(c);
        write(&rec, sizeof(rec));
    }

    void close() {
        if (file_ != nullptr) {
            std::fclose(file_);
            file_ = nullptr;
        }
    }

private:
    void write(const void* data, std::size_t size) {
        if (std::fwrite(data, 1, size, file_) != size) throw std::runtime_error("log write failed");
    }

    std::FILE* file_;
};

class LogReader {
public:
    explicit LogReader(const std::string& path) : file_(std::fopen(path.c_str(), "rb")) {
        if (file_ == nullptr) throw std::runtime_error("cannot open log for reading: " + path);
        LogHeader       header{};
        const LogHeader expected{};
        if (!read(&header, sizeof(header)) ||
            std::memcmp(header.magic, expected.magic, sizeof(header.magic)) != 0 ||
            header.version != expected.version) {
            throw std::runtime_error("not an Aether log file: " + path);
        }
        instruments_.reserve(header.instrument_count);
        for (std::uint32_t i = 0; i < header.instrument_count; ++i) {
            InstrumentRecord rec{};
            if (!read(&rec, sizeof(rec))) throw std::runtime_error("truncated log header: " + path);
            instruments_.push_back(BookConfig{rec.min_price, rec.max_price,
                                              static_cast<std::size_t>(rec.max_orders),
                                              rec.id_capacity});
        }
    }
    ~LogReader() {
        if (file_ != nullptr) std::fclose(file_);
    }
    LogReader(const LogReader&)            = delete;
    LogReader& operator=(const LogReader&) = delete;

    const std::vector<BookConfig>& instruments() const noexcept { return instruments_; }

    // Reads the next command. Returns false at end of file.
    bool next(Command& out) {
        LogRecord rec{};
        if (!read(&rec, sizeof(rec))) return false;
        out = to_command(rec);
        return true;
    }

private:
    bool read(void* data, std::size_t size) { return std::fread(data, 1, size, file_) == size; }

    std::FILE*              file_;
    std::vector<BookConfig> instruments_;
};

} // namespace aether
