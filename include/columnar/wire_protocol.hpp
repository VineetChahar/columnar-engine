#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "columnar/exec_batch.hpp"

namespace columnar {

// See PROTOCOL.md for the full wire format. Summary:
//
//   [uint32 LE frame_length]  -- bytes that follow, i.e. len(version..payload)
//   [uint8  version]          -- always kProtocolVersion for now
//   [uint8  message_type]     -- MessageType below
//   [payload, frame_length - 2 bytes]
//
// A QueryRequest's payload is just the UTF-8 SQL text. A QueryResultOk's
// payload is the whole result set, column-major, in one frame (no
// streaming of partial results -- see PROTOCOL.md for why that's a scoped
// simplification, not a protocol limitation). A QueryError's payload is a
// UTF-8 message.
constexpr std::uint8_t kProtocolVersion = 1;

enum class MessageType : std::uint8_t {
  kQueryRequest = 1,
  kQueryResultOk = 2,
  kQueryError = 3,
};

struct Frame {
  MessageType type;
  std::vector<std::uint8_t> payload;
};

// A whole query result, materialized (all batches concatenated) for wire
// transfer -- see ExecBatch for the in-memory shape this mirrors.
struct WireResultSet {
  std::vector<std::string> column_names;
  std::vector<ExecColumn> columns;
  std::size_t row_count() const { return columns.empty() ? 0 : columns[0].size(); }
};

std::vector<std::uint8_t> encode_query_request(const std::string& sql);
std::vector<std::uint8_t> encode_query_result(const WireResultSet& result);
std::vector<std::uint8_t> encode_query_error(const std::string& message);

std::string decode_query_request(const std::vector<std::uint8_t>& payload);
WireResultSet decode_query_result(const std::vector<std::uint8_t>& payload);
std::string decode_query_error(const std::vector<std::uint8_t>& payload);

// Blocking, unbuffered socket I/O for one frame. `read_fn`/`write_fn` do a
// short read/write of exactly the requested number of bytes (looping
// internally over partial reads/writes) -- passed as callbacks rather than
// hard-coding ::read/::write so this is unit-testable against an in-memory
// buffer instead of a real socket.
using ReadFn = std::function<bool(void* buf, std::size_t len)>;
using WriteFn = std::function<bool(const void* buf, std::size_t len)>;

bool write_frame(const WriteFn& write_fn, const Frame& frame);
// Returns false on clean EOF before any bytes of a new frame were read;
// throws std::runtime_error on a short/corrupt frame.
bool read_frame(const ReadFn& read_fn, Frame& out);

}  // namespace columnar
