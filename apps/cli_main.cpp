// columnar_cli <host> <port> ["SQL"]
//
// With a query argument, runs it once and exits (handy for scripting/
// benchmarking). Without one, reads a REPL: one query per line, blank line
// or Ctrl-D to exit.

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include "columnar/wire_protocol.hpp"

using namespace columnar;

namespace {

bool socket_read(int fd, void* buf, std::size_t len) {
  auto* p = static_cast<std::uint8_t*>(buf);
  std::size_t got = 0;
  while (got < len) {
    const ssize_t n = ::read(fd, p + got, len - got);
    if (n == 0) return false;
    if (n < 0) {
      if (errno == EINTR) continue;
      throw std::runtime_error(std::string("read failed: ") + std::strerror(errno));
    }
    got += static_cast<std::size_t>(n);
  }
  return true;
}

bool socket_write(int fd, const void* buf, std::size_t len) {
  const auto* p = static_cast<const std::uint8_t*>(buf);
  std::size_t sent = 0;
  while (sent < len) {
    const ssize_t n = ::write(fd, p + sent, len - sent);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    sent += static_cast<std::size_t>(n);
  }
  return true;
}

int connect_to(const std::string& host, int port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) throw std::runtime_error("socket() failed");

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<std::uint16_t>(port));
  if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
    hostent* he = ::gethostbyname(host.c_str());
    if (!he) throw std::runtime_error("cannot resolve host: " + host);
    std::memcpy(&addr.sin_addr, he->h_addr_list[0], sizeof(addr.sin_addr));
  }
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    throw std::runtime_error("connect() failed");
  }
  return fd;
}

void print_cell(const ExecColumn& col, std::size_t row) {
  if (!col.validity[row]) {
    std::printf("NULL");
    return;
  }
  switch (col.type) {
    case ExecType::kInt64: std::printf("%lld", static_cast<long long>(col.ints()[row])); break;
    case ExecType::kDouble: std::printf("%.4f", col.doubles()[row]); break;
    case ExecType::kBool: std::printf("%s", col.bools()[row] ? "true" : "false"); break;
    case ExecType::kText: std::printf("%s", col.texts()[row].c_str()); break;
  }
}

bool run_query(int fd, const std::string& sql) {
  auto read_fn = [fd](void* buf, std::size_t len) { return socket_read(fd, buf, len); };
  auto write_fn = [fd](const void* buf, std::size_t len) { return socket_write(fd, buf, len); };

  Frame request{MessageType::kQueryRequest, encode_query_request(sql)};
  if (!write_frame(write_fn, request)) {
    std::fprintf(stderr, "connection closed while sending query\n");
    return false;
  }

  Frame response;
  if (!read_frame(read_fn, response)) {
    std::fprintf(stderr, "connection closed by server\n");
    return false;
  }

  if (response.type == MessageType::kQueryError) {
    std::fprintf(stderr, "ERROR: %s\n", decode_query_error(response.payload).c_str());
    return true;
  }

  WireResultSet result = decode_query_result(response.payload);
  for (std::size_t i = 0; i < result.column_names.size(); ++i) {
    if (i) std::printf(" | ");
    std::printf("%s", result.column_names[i].c_str());
  }
  std::printf("\n");
  for (std::size_t r = 0; r < result.row_count(); ++r) {
    for (std::size_t c = 0; c < result.columns.size(); ++c) {
      if (c) std::printf(" | ");
      print_cell(result.columns[c], r);
    }
    std::printf("\n");
  }
  std::printf("(%zu row%s)\n", result.row_count(), result.row_count() == 1 ? "" : "s");
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: %s <host> <port> [\"SQL query\"]\n", argv[0]);
    return 1;
  }
  const std::string host = argv[1];
  const int port = std::atoi(argv[2]);

  int fd;
  try {
    fd = connect_to(host, port);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "%s\n", e.what());
    return 1;
  }

  if (argc >= 4) {
    const bool ok = run_query(fd, argv[3]);
    ::close(fd);
    return ok ? 0 : 1;
  }

  std::printf("columnar_cli connected to %s:%d. Enter SQL, blank line to quit.\n", host.c_str(),
              port);
  std::string line;
  while (true) {
    std::printf("> ");
    std::fflush(stdout);
    if (!std::getline(std::cin, line) || line.empty()) break;
    if (!run_query(fd, line)) break;
  }
  ::close(fd);
  return 0;
}
