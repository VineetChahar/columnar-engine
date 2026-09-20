#include "columnar/wire_protocol.hpp"

#include <cstring>
#include <stdexcept>

namespace columnar {

namespace {

void put_u8(std::vector<std::uint8_t>& buf, std::uint8_t v) { buf.push_back(v); }

void put_u32(std::vector<std::uint8_t>& buf, std::uint32_t v) {
  const auto* bytes = reinterpret_cast<const std::uint8_t*>(&v);
  buf.insert(buf.end(), bytes, bytes + sizeof(v));
}
void put_u64(std::vector<std::uint8_t>& buf, std::uint64_t v) {
  const auto* bytes = reinterpret_cast<const std::uint8_t*>(&v);
  buf.insert(buf.end(), bytes, bytes + sizeof(v));
}
void put_f64(std::vector<std::uint8_t>& buf, double v) {
  const auto* bytes = reinterpret_cast<const std::uint8_t*>(&v);
  buf.insert(buf.end(), bytes, bytes + sizeof(v));
}
void put_bytes(std::vector<std::uint8_t>& buf, const void* data, std::size_t len) {
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  buf.insert(buf.end(), bytes, bytes + len);
}
void put_string(std::vector<std::uint8_t>& buf, const std::string& s) {
  put_u32(buf, static_cast<std::uint32_t>(s.size()));
  put_bytes(buf, s.data(), s.size());
}

class Reader {
 public:
  Reader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

  std::uint8_t u8() { return read<std::uint8_t>(); }
  std::uint32_t u32() { return read<std::uint32_t>(); }
  std::uint64_t u64() { return read<std::uint64_t>(); }
  double f64() { return read<double>(); }

  std::string string() {
    const std::uint32_t len = u32();
    check(len);
    std::string s(reinterpret_cast<const char*>(data_ + pos_), len);
    pos_ += len;
    return s;
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
    if (pos_ + len > size_) throw std::runtime_error("wire protocol: truncated payload");
  }

  const std::uint8_t* data_;
  std::size_t size_;
  std::size_t pos_ = 0;
};

}  // namespace

std::vector<std::uint8_t> encode_query_request(const std::string& sql) {
  std::vector<std::uint8_t> payload;
  put_bytes(payload, sql.data(), sql.size());
  return payload;
}

std::string decode_query_request(const std::vector<std::uint8_t>& payload) {
  return std::string(reinterpret_cast<const char*>(payload.data()), payload.size());
}

std::vector<std::uint8_t> encode_query_error(const std::string& message) {
  std::vector<std::uint8_t> payload;
  put_bytes(payload, message.data(), message.size());
  return payload;
}

std::string decode_query_error(const std::vector<std::uint8_t>& payload) {
  return std::string(reinterpret_cast<const char*>(payload.data()), payload.size());
}

std::vector<std::uint8_t> encode_query_result(const WireResultSet& result) {
  std::vector<std::uint8_t> payload;
  put_u32(payload, static_cast<std::uint32_t>(result.column_names.size()));
  for (std::size_t i = 0; i < result.column_names.size(); ++i) {
    put_string(payload, result.column_names[i]);
    put_u8(payload, static_cast<std::uint8_t>(result.columns[i].type));
  }
  put_u64(payload, static_cast<std::uint64_t>(result.row_count()));

  for (const ExecColumn& col : result.columns) {
    for (std::size_t r = 0; r < col.size(); ++r) {
      put_u8(payload, col.validity[r]);
      if (!col.validity[r]) continue;
      switch (col.type) {
        case ExecType::kInt64: put_u64(payload, static_cast<std::uint64_t>(col.ints()[r])); break;
        case ExecType::kDouble: put_f64(payload, col.doubles()[r]); break;
        case ExecType::kBool: put_u8(payload, col.bools()[r]); break;
        case ExecType::kText: put_string(payload, col.texts()[r]); break;
      }
    }
  }
  return payload;
}

WireResultSet decode_query_result(const std::vector<std::uint8_t>& payload) {
  Reader r(payload.data(), payload.size());
  WireResultSet result;
  const std::uint32_t column_count = r.u32();
  std::vector<ExecType> types(column_count);
  for (std::uint32_t i = 0; i < column_count; ++i) {
    result.column_names.push_back(r.string());
    types[i] = static_cast<ExecType>(r.u8());
  }
  const std::uint64_t row_count = r.u64();

  for (std::uint32_t i = 0; i < column_count; ++i) {
    ExecColumn col = ExecColumn::make(types[i], row_count);
    for (std::uint64_t row = 0; row < row_count; ++row) {
      col.validity[row] = r.u8();
      if (!col.validity[row]) continue;
      switch (types[i]) {
        case ExecType::kInt64: col.ints()[row] = static_cast<std::int64_t>(r.u64()); break;
        case ExecType::kDouble: col.doubles()[row] = r.f64(); break;
        case ExecType::kBool: col.bools()[row] = r.u8(); break;
        case ExecType::kText: col.texts()[row] = r.string(); break;
      }
    }
    result.columns.push_back(std::move(col));
  }
  return result;
}

bool write_frame(const WriteFn& write_fn, const Frame& frame) {
  const std::uint32_t frame_length = static_cast<std::uint32_t>(2 + frame.payload.size());
  std::vector<std::uint8_t> header;
  put_u32(header, frame_length);
  put_u8(header, kProtocolVersion);
  put_u8(header, static_cast<std::uint8_t>(frame.type));
  if (!write_fn(header.data(), header.size())) return false;
  if (!frame.payload.empty() && !write_fn(frame.payload.data(), frame.payload.size())) {
    return false;
  }
  return true;
}

bool read_frame(const ReadFn& read_fn, Frame& out) {
  std::uint8_t length_bytes[4];
  if (!read_fn(length_bytes, 4)) return false;  // clean EOF between frames
  std::uint32_t frame_length;
  std::memcpy(&frame_length, length_bytes, 4);
  if (frame_length < 2) throw std::runtime_error("wire protocol: frame too short");

  std::vector<std::uint8_t> rest(frame_length);
  if (!read_fn(rest.data(), rest.size())) {
    throw std::runtime_error("wire protocol: connection closed mid-frame");
  }
  const std::uint8_t version = rest[0];
  if (version != kProtocolVersion) {
    throw std::runtime_error("wire protocol: unsupported version " + std::to_string(version));
  }
  out.type = static_cast<MessageType>(rest[1]);
  out.payload.assign(rest.begin() + 2, rest.end());
  return true;
}

}  // namespace columnar
