#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "columnar/column_chunk.hpp"

namespace columnar {

struct ColumnSchema {
  std::string name;
  TypeId type;
};

struct Column {
  ColumnSchema schema;
  std::vector<ColumnChunkVariant> chunks;
};

struct Table {
  std::vector<Column> columns;
};

// Writes `table` to `path` in this engine's on-disk columnar format:
//
//   [chunk 0 data][pad][chunk 1 data][pad]...[chunk N data][pad]
//   [footer: schema + per-chunk metadata incl. zone maps]
//   [4-byte little-endian footer length]
//   [4-byte magic "CEF1"]
//
// A trailer layout (data first, footer last) is what Parquet/ORC also do:
// the format doesn't need to know its own total metadata size before it
// starts writing chunk bytes. Each chunk's data block starts at a 64-byte
// (cache-line) aligned file offset -- see DESIGN.md section 6 for why, and
// LEARNING.md for the gap between "aligned on disk" and "zero-copy in
// memory" that this phase does not close yet.
//
// Fixed little-endian, multi-byte integers written via raw memcpy (every
// platform this engine targets -- x86_64, ARM64 -- is little-endian
// natively). The footer's version+endianness-marker bytes exist so a reader
// can detect a mismatched build instead of silently misinterpreting bytes,
// not to support cross-endian portability.
void write_table(const std::filesystem::path& path, const Table& table);

// Memory-maps `path`, parses its footer, and reconstructs a Table. Chunk
// bytes are copied from the mapped region into normal owned
// ColumnChunkVariant objects (via each chunk type's RawPartsTag
// constructor) rather than left as views into the mapping -- see
// LEARNING.md for what a fully zero-copy reader would need to change and
// why that's out of scope for Phase 1.
Table read_table(const std::filesystem::path& path);

}  // namespace columnar
