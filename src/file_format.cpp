#include "columnar/file_format.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
#include <fstream>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace columnar {
namespace {

constexpr std::uint32_t kMagic = 0x31464543;  // ASCII "CEF1", little-endian
constexpr std::uint8_t kFormatVersion = 1;
constexpr std::uint8_t kLittleEndianMarker = 1;
constexpr std::size_t kChunkAlignment = 64;

std::size_t round_up(std::size_t n, std::size_t multiple) {
  return ((n + multiple - 1) / multiple) * multiple;
}

// ---------------------------------------------------------- byte buffer --

void put_u8(std::vector<std::uint8_t>& buf, std::uint8_t v) { buf.push_back(v); }

template <typename T>
void put_raw(std::vector<std::uint8_t>& buf, T v) {
  static_assert(std::is_trivially_copyable_v<T>);
  const auto* bytes = reinterpret_cast<const std::uint8_t*>(&v);
  buf.insert(buf.end(), bytes, bytes + sizeof(T));
}

void put_bytes(std::vector<std::uint8_t>& buf, const void* data, std::size_t len) {
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  buf.insert(buf.end(), bytes, bytes + len);
}

void put_string(std::vector<std::uint8_t>& buf, std::string_view s) {
  put_raw<std::uint32_t>(buf, static_cast<std::uint32_t>(s.size()));
  put_bytes(buf, s.data(), s.size());
}

void pad_to_alignment(std::vector<std::uint8_t>& buf, std::size_t alignment) {
  const std::size_t target = round_up(buf.size(), alignment);
  buf.resize(target, std::uint8_t{0});
}

class ByteReader {
 public:
  ByteReader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

  std::uint8_t u8() { return read<std::uint8_t>(); }
  std::uint32_t u32() { return read<std::uint32_t>(); }
  std::uint64_t u64() { return read<std::uint64_t>(); }

  std::string_view bytes(std::size_t len) {
    check(len);
    std::string_view v(reinterpret_cast<const char*>(data_ + pos_), len);
    pos_ += len;
    return v;
  }

  std::string string() {
    const std::uint32_t len = u32();
    return std::string(bytes(len));
  }

 private:
  template <typename T>
  T read() {
    check(sizeof(T));
    T value;
    std::memcpy(&value, data_ + pos_, sizeof(T));
    pos_ += sizeof(T);
    return value;
  }

  void check(std::size_t len) const {
    if (pos_ + len > size_) {
      throw std::runtime_error("columnar file format: truncated footer");
    }
  }

  const std::uint8_t* data_;
  std::size_t size_;
  std::size_t pos_ = 0;
};

// ------------------------------------------------------------- mmap RAII --

class MappedFile {
 public:
  explicit MappedFile(const std::filesystem::path& path) {
    fd_ = ::open(path.c_str(), O_RDONLY);
    if (fd_ < 0) {
      throw std::runtime_error("columnar: cannot open " + path.string());
    }
    struct stat st{};
    if (::fstat(fd_, &st) != 0) {
      ::close(fd_);
      throw std::runtime_error("columnar: fstat failed for " + path.string());
    }
    size_ = static_cast<std::size_t>(st.st_size);
    if (size_ == 0) {
      ::close(fd_);
      throw std::runtime_error("columnar: empty file " + path.string());
    }
    data_ = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
    if (data_ == MAP_FAILED) {
      ::close(fd_);
      throw std::runtime_error("columnar: mmap failed for " + path.string());
    }
  }

  MappedFile(const MappedFile&) = delete;
  MappedFile& operator=(const MappedFile&) = delete;

  ~MappedFile() {
    if (data_ != nullptr && data_ != MAP_FAILED) {
      ::munmap(data_, size_);
    }
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }

  const std::uint8_t* data() const noexcept {
    return static_cast<const std::uint8_t*>(data_);
  }
  std::size_t size() const noexcept { return size_; }

 private:
  int fd_ = -1;
  void* data_ = nullptr;
  std::size_t size_ = 0;
};

// -------------------------------------------------------- chunk metadata --

// Decoded footer entry for one chunk -- everything needed to prune (zone
// map, row/null counts) or materialize (offset/length into the data
// section) a chunk without touching the others. Phase 4's EXPLAIN reads
// exactly this struct's fields when it reports "chunks pruned per query."
struct ChunkFooterEntry {
  Encoding encoding;
  std::uint64_t data_offset;
  std::uint32_t row_count;
  std::uint32_t null_count;
};

// ----------------------------------------------------- writer: per chunk --

void write_numeric_zone_map(std::vector<std::uint8_t>& footer, const auto& zone_map) {
  put_u8(footer, zone_map.has_values ? 1 : 0);
  put_raw(footer, zone_map.min);
  put_raw(footer, zone_map.max);
}

void write_string_zone_map(std::vector<std::uint8_t>& footer, const StringZoneMap& zm) {
  put_u8(footer, zm.has_values ? 1 : 0);
  put_string(footer, zm.min);
  put_string(footer, zm.max);
}

void write_dictionary(std::vector<std::uint8_t>& footer,
                       const std::vector<std::string>& dictionary) {
  put_raw<std::uint32_t>(footer, static_cast<std::uint32_t>(dictionary.size()));
  for (const std::string& entry : dictionary) {
    put_string(footer, entry);
  }
}

// Writes one chunk's raw data block into `file_buf` (padded to
// kChunkAlignment) and its metadata (encoding, offset, zone map, and any
// encoding-specific extras) into `footer_buf`. Dispatches on the concrete
// alternative via std::visit + `if constexpr` rather than a virtual
// serialize() method -- there are exactly seven alternatives, known at
// compile time, and this keeps the switch exhaustive-checked by the
// compiler (add an eighth alternative to the variant and this stops
// compiling until it's handled here too).
void write_chunk(std::vector<std::uint8_t>& file_buf, std::vector<std::uint8_t>& footer_buf,
                  const ColumnChunkVariant& chunk) {
  std::visit(
      [&](const auto& c) {
        using ChunkT = std::decay_t<decltype(c)>;

        pad_to_alignment(file_buf, kChunkAlignment);
        const std::uint64_t data_offset = file_buf.size();

        put_u8(footer_buf, static_cast<std::uint8_t>(ChunkT::kEncoding));
        put_raw<std::uint64_t>(footer_buf, data_offset);
        put_raw<std::uint32_t>(footer_buf, static_cast<std::uint32_t>(c.row_count()));
        put_raw<std::uint32_t>(footer_buf, c.zone_map().null_count);

        if constexpr (std::is_same_v<ChunkT, PlainColumnChunk<std::int32_t>> ||
                      std::is_same_v<ChunkT, PlainColumnChunk<std::int64_t>> ||
                      std::is_same_v<ChunkT, PlainColumnChunk<double>>) {
          write_numeric_zone_map(footer_buf, c.zone_map());
          put_bytes(file_buf, c.validity().data(), c.validity().byte_size());
          put_bytes(file_buf, c.values().data(),
                    c.values().size() * sizeof(c.values()[0]));
        } else if constexpr (std::is_same_v<ChunkT, BoolColumnChunk>) {
          write_numeric_zone_map(footer_buf, c.zone_map());
          put_bytes(file_buf, c.validity().data(), c.validity().byte_size());
          put_bytes(file_buf, c.data_bitmap().data(), c.data_bitmap().byte_size());
        } else if constexpr (std::is_same_v<ChunkT, VarcharColumnChunk>) {
          write_string_zone_map(footer_buf, c.zone_map());
          put_raw<std::uint64_t>(footer_buf, c.data_bytes());
          put_bytes(file_buf, c.validity().data(), c.validity().byte_size());
          put_bytes(file_buf, c.offsets().data(), c.offsets().size() * sizeof(std::uint32_t));
          put_bytes(file_buf, c.raw_data(), c.data_bytes());
        } else if constexpr (std::is_same_v<ChunkT, DictionaryColumnChunk>) {
          write_string_zone_map(footer_buf, c.zone_map());
          write_dictionary(footer_buf, c.dictionary());
          put_bytes(file_buf, c.validity().data(), c.validity().byte_size());
          put_bytes(file_buf, c.codes().data(), c.codes().size() * sizeof(std::uint32_t));
        } else if constexpr (std::is_same_v<ChunkT, DictionaryRleColumnChunk>) {
          write_string_zone_map(footer_buf, c.zone_map());
          write_dictionary(footer_buf, c.dictionary());
          put_raw<std::uint32_t>(footer_buf, static_cast<std::uint32_t>(c.run_count()));
          for (const RleRun& run : c.runs()) {
            put_raw<std::uint32_t>(file_buf, run.code);
            put_raw<std::uint32_t>(file_buf, run.length);
            put_u8(file_buf, run.is_null ? 1 : 0);
          }
        } else {
          static_assert(!sizeof(ChunkT*), "unhandled ColumnChunkVariant alternative");
        }
      },
      chunk);
}

// ----------------------------------------------------- reader: per chunk --

// Reads a packed bitmap directly out of the mmap'd data section at `cursor`,
// advancing `cursor` past it. (Unlike ByteReader, which walks the footer
// bytes sequentially, this walks the raw file pointer -- the data section
// has no bounds-checked reader because its layout is already fully
// determined by the footer metadata that was validated on the way in.)
Bitmap read_bitmap_at(const std::uint8_t* file, std::size_t& cursor, std::size_t num_bits) {
  Bitmap bitmap(num_bits, /*default_value=*/false);
  const std::size_t byte_len = (num_bits + 7) / 8;
  std::memcpy(bitmap.data(), file + cursor, byte_len);
  cursor += byte_len;
  return bitmap;
}

template <typename T>
ZoneMap<T> read_numeric_zone_map(ByteReader& r, std::uint32_t row_count,
                                  std::uint32_t null_count) {
  ZoneMap<T> zm;
  zm.row_count = row_count;
  zm.null_count = null_count;
  zm.has_values = r.u8() != 0;
  std::string_view min_bytes = r.bytes(sizeof(T));
  std::string_view max_bytes = r.bytes(sizeof(T));
  std::memcpy(&zm.min, min_bytes.data(), sizeof(T));
  std::memcpy(&zm.max, max_bytes.data(), sizeof(T));
  return zm;
}

StringZoneMap read_string_zone_map(ByteReader& r, std::uint32_t row_count,
                                    std::uint32_t null_count) {
  StringZoneMap zm;
  zm.row_count = row_count;
  zm.null_count = null_count;
  zm.has_values = r.u8() != 0;
  zm.min = r.string();
  zm.max = r.string();
  return zm;
}

std::vector<std::string> read_dictionary(ByteReader& r) {
  const std::uint32_t size = r.u32();
  std::vector<std::string> dictionary;
  dictionary.reserve(size);
  for (std::uint32_t i = 0; i < size; ++i) {
    dictionary.push_back(r.string());
  }
  return dictionary;
}

template <typename T>
ColumnChunkVariant read_plain_numeric_chunk(ByteReader& footer, const std::uint8_t* file,
                                             std::uint64_t data_offset,
                                             std::uint32_t row_count,
                                             std::uint32_t null_count) {
  ZoneMap<T> zone_map = read_numeric_zone_map<T>(footer, row_count, null_count);

  std::size_t cursor = data_offset;
  Bitmap validity = read_bitmap_at(file, cursor, row_count);

  AlignedBuffer<T> values(row_count);
  std::memcpy(values.data(), file + cursor, row_count * sizeof(T));

  return PlainColumnChunk<T>(typename PlainColumnChunk<T>::RawPartsTag{}, std::move(values),
                              std::move(validity), zone_map, row_count);
}

ColumnChunkVariant read_bool_chunk(ByteReader& footer, const std::uint8_t* file,
                                    std::uint64_t data_offset, std::uint32_t row_count,
                                    std::uint32_t null_count) {
  ZoneMap<bool> zone_map = read_numeric_zone_map<bool>(footer, row_count, null_count);

  std::size_t cursor = data_offset;
  Bitmap validity = read_bitmap_at(file, cursor, row_count);
  Bitmap data_bitmap = read_bitmap_at(file, cursor, row_count);

  return BoolColumnChunk(BoolColumnChunk::RawPartsTag{}, std::move(data_bitmap),
                          std::move(validity), zone_map, row_count);
}

ColumnChunkVariant read_varchar_chunk(ByteReader& footer, const std::uint8_t* file,
                                       std::uint64_t data_offset, std::uint32_t row_count,
                                       std::uint32_t null_count) {
  StringZoneMap zone_map = read_string_zone_map(footer, row_count, null_count);
  const std::uint64_t data_bytes = footer.u64();

  std::size_t cursor = data_offset;
  Bitmap validity = read_bitmap_at(file, cursor, row_count);

  AlignedBuffer<std::uint32_t> offsets(row_count + 1);
  std::memcpy(offsets.data(), file + cursor, (row_count + 1) * sizeof(std::uint32_t));
  cursor += (row_count + 1) * sizeof(std::uint32_t);

  AlignedBuffer<char> data(data_bytes);
  if (data_bytes > 0) {
    std::memcpy(data.data(), file + cursor, data_bytes);
  }

  return VarcharColumnChunk(VarcharColumnChunk::RawPartsTag{}, std::move(offsets),
                             std::move(data), std::move(validity), std::move(zone_map),
                             row_count);
}

ColumnChunkVariant read_dictionary_chunk(ByteReader& footer, const std::uint8_t* file,
                                          std::uint64_t data_offset, std::uint32_t row_count,
                                          std::uint32_t null_count) {
  StringZoneMap zone_map = read_string_zone_map(footer, row_count, null_count);
  std::vector<std::string> dictionary = read_dictionary(footer);

  std::size_t cursor = data_offset;
  Bitmap validity = read_bitmap_at(file, cursor, row_count);

  AlignedBuffer<std::uint32_t> codes(row_count);
  std::memcpy(codes.data(), file + cursor, row_count * sizeof(std::uint32_t));

  return DictionaryColumnChunk(DictionaryColumnChunk::RawPartsTag{}, std::move(dictionary),
                                std::move(codes), std::move(validity), std::move(zone_map),
                                row_count);
}

ColumnChunkVariant read_dictionary_rle_chunk(ByteReader& footer, const std::uint8_t* file,
                                              std::uint64_t data_offset,
                                              std::uint32_t row_count,
                                              std::uint32_t null_count) {
  StringZoneMap zone_map = read_string_zone_map(footer, row_count, null_count);
  std::vector<std::string> dictionary = read_dictionary(footer);
  const std::uint32_t run_count = footer.u32();

  std::vector<RleRun> runs;
  runs.reserve(run_count);
  std::size_t cursor = data_offset;
  for (std::uint32_t i = 0; i < run_count; ++i) {
    RleRun run;
    std::memcpy(&run.code, file + cursor, sizeof(std::uint32_t));
    cursor += sizeof(std::uint32_t);
    std::memcpy(&run.length, file + cursor, sizeof(std::uint32_t));
    cursor += sizeof(std::uint32_t);
    run.is_null = file[cursor] != 0;
    cursor += 1;
    runs.push_back(run);
  }

  return DictionaryRleColumnChunk(DictionaryRleColumnChunk::RawPartsTag{},
                                   std::move(dictionary), std::move(runs),
                                   std::move(zone_map), row_count);
}

}  // namespace

void write_table(const std::filesystem::path& path, const Table& table) {
  std::vector<std::uint8_t> file_buf;
  std::vector<std::uint8_t> footer_buf;

  put_u8(footer_buf, kFormatVersion);
  put_u8(footer_buf, kLittleEndianMarker);
  put_raw<std::uint32_t>(footer_buf, static_cast<std::uint32_t>(table.columns.size()));

  for (const Column& column : table.columns) {
    put_string(footer_buf, column.schema.name);
    put_u8(footer_buf, static_cast<std::uint8_t>(column.schema.type));
    put_raw<std::uint32_t>(footer_buf, static_cast<std::uint32_t>(column.chunks.size()));
    for (const ColumnChunkVariant& chunk : column.chunks) {
      write_chunk(file_buf, footer_buf, chunk);
    }
  }

  std::vector<std::uint8_t> out;
  out.reserve(file_buf.size() + footer_buf.size() + 8);
  out.insert(out.end(), file_buf.begin(), file_buf.end());
  out.insert(out.end(), footer_buf.begin(), footer_buf.end());
  put_raw<std::uint32_t>(out, static_cast<std::uint32_t>(footer_buf.size()));
  put_raw<std::uint32_t>(out, kMagic);

  std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
  if (!ofs) {
    throw std::runtime_error("columnar: cannot open " + path.string() + " for writing");
  }
  ofs.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
  if (!ofs) {
    throw std::runtime_error("columnar: write failed for " + path.string());
  }
}

Table read_table(const std::filesystem::path& path) {
  // Every chunk type below is reconstructed by memcpy-ing bytes out of the
  // mapping into its own owned AlignedBuffer/Bitmap/vector (RawPartsTag
  // constructors), so nothing in the returned Table points back into this
  // mapping -- it's safe for `mapped` to unmap when this function returns.
  MappedFile mapped(path);
  if (mapped.size() < 8) {
    throw std::runtime_error("columnar: file too small to be valid: " + path.string());
  }

  const std::uint8_t* file = mapped.data();
  const std::size_t file_size = mapped.size();

  std::uint32_t magic;
  std::memcpy(&magic, file + file_size - 4, sizeof(magic));
  if (magic != kMagic) {
    throw std::runtime_error("columnar: bad magic in " + path.string());
  }
  std::uint32_t footer_length;
  std::memcpy(&footer_length, file + file_size - 8, sizeof(footer_length));
  if (footer_length + 8 > file_size) {
    throw std::runtime_error("columnar: corrupt footer length in " + path.string());
  }

  const std::size_t footer_offset = file_size - 8 - footer_length;
  ByteReader footer(file + footer_offset, footer_length);

  const std::uint8_t version = footer.u8();
  const std::uint8_t endianness = footer.u8();
  if (version != kFormatVersion) {
    throw std::runtime_error("columnar: unsupported format version in " + path.string());
  }
  if (endianness != kLittleEndianMarker) {
    throw std::runtime_error(
        "columnar: file was written on a big-endian host; this build only "
        "reads little-endian columnar files: " +
        path.string());
  }

  Table table;
  const std::uint32_t column_count = footer.u32();
  table.columns.reserve(column_count);

  for (std::uint32_t ci = 0; ci < column_count; ++ci) {
    Column column;
    column.schema.name = footer.string();
    column.schema.type = static_cast<TypeId>(footer.u8());
    const std::uint32_t chunk_count = footer.u32();
    column.chunks.reserve(chunk_count);

    for (std::uint32_t chi = 0; chi < chunk_count; ++chi) {
      const Encoding chunk_encoding = static_cast<Encoding>(footer.u8());
      const std::uint64_t data_offset = footer.u64();
      const std::uint32_t row_count = footer.u32();
      const std::uint32_t null_count = footer.u32();

      switch (column.schema.type) {
        case TypeId::kInt32:
          column.chunks.push_back(read_plain_numeric_chunk<std::int32_t>(
              footer, file, data_offset, row_count, null_count));
          break;
        case TypeId::kInt64:
          column.chunks.push_back(read_plain_numeric_chunk<std::int64_t>(
              footer, file, data_offset, row_count, null_count));
          break;
        case TypeId::kDouble:
          column.chunks.push_back(read_plain_numeric_chunk<double>(
              footer, file, data_offset, row_count, null_count));
          break;
        case TypeId::kBool:
          column.chunks.push_back(
              read_bool_chunk(footer, file, data_offset, row_count, null_count));
          break;
        case TypeId::kVarchar:
          switch (chunk_encoding) {
            case Encoding::kPlain:
              column.chunks.push_back(
                  read_varchar_chunk(footer, file, data_offset, row_count, null_count));
              break;
            case Encoding::kDictionary:
              column.chunks.push_back(read_dictionary_chunk(footer, file, data_offset,
                                                              row_count, null_count));
              break;
            case Encoding::kDictionaryRle:
              column.chunks.push_back(read_dictionary_rle_chunk(
                  footer, file, data_offset, row_count, null_count));
              break;
          }
          break;
      }
    }
    table.columns.push_back(std::move(column));
  }

  return table;
}

}  // namespace columnar
