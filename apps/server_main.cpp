// columnar_server <port> <table1.cef> [<table2.cef> ...]
//
// Loads each .cef file (Phase 1's on-disk format) via read_table(), infers
// each table's name from its filename (stem, no extension), and builds the
// Catalog directly from the schema already stored in the file -- no
// separate config needed, since write_table() persisted column names and
// types. Then serves the wire protocol in PROTOCOL.md over TCP.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <thread>

#include "columnar/catalog.hpp"
#include "columnar/file_format.hpp"
#include "columnar/logical_plan.hpp"
#include "columnar/operators.hpp"
#include "columnar/optimizer.hpp"
#include "columnar/parser.hpp"
#include "columnar/thread_pool.hpp"
#include "columnar/wire_protocol.hpp"

using namespace columnar;

namespace {

std::string table_name_from_path(const std::filesystem::path& path) { return path.stem().string(); }

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
      return false;  // peer likely gone; let the caller close the connection
    }
    sent += static_cast<std::size_t>(n);
  }
  return true;
}

void handle_connection(int fd, const Catalog& catalog, const Database& db) {
  auto read_fn = [fd](void* buf, std::size_t len) { return socket_read(fd, buf, len); };
  auto write_fn = [fd](const void* buf, std::size_t len) { return socket_write(fd, buf, len); };

  try {
    Frame frame;
    while (read_frame(read_fn, frame)) {
      if (frame.type != MessageType::kQueryRequest) continue;  // ignore unexpected message types

      Frame response;
      try {
        const std::string sql = decode_query_request(frame.payload);
        AstQuery ast = parse_query(sql);
        LogicalPlanPtr plan = bind_query(ast, catalog);
        plan = optimize(std::move(plan));
        ScanStats stats;
        ExprArena arena;
        OperatorPtr root = build_physical_plan(*plan, db, &stats, arena);
        ExecBatch full = materialize_all(*root);

        WireResultSet result;
        result.column_names = std::move(full.column_names);
        result.columns = std::move(full.columns);
        response.type = MessageType::kQueryResultOk;
        response.payload = encode_query_result(result);
      } catch (const std::exception& e) {
        response.type = MessageType::kQueryError;
        response.payload = encode_query_error(e.what());
      }
      if (!write_frame(write_fn, response)) break;
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "connection error: %s\n", e.what());
  }
  ::close(fd);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: %s <port> <table1.cef> [<table2.cef> ...]\n", argv[0]);
    return 1;
  }
  ::signal(SIGPIPE, SIG_IGN);  // writing to a closed socket must not kill the process

  const int port = std::atoi(argv[1]);
  Catalog catalog;
  Database db;

  for (int i = 2; i < argc; ++i) {
    const std::filesystem::path path = argv[i];
    const std::string name = table_name_from_path(path);
    std::fprintf(stderr, "loading %s as table '%s'...\n", path.c_str(), name.c_str());
    Table table = read_table(path);

    CatalogTable catalog_table;
    catalog_table.name = name;
    for (const Column& col : table.columns) catalog_table.columns.push_back({col.schema.name, col.schema.type});
    catalog.add_table(std::move(catalog_table));
    db.emplace(name, std::move(table));
  }

  const int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd < 0) {
    std::perror("socket");
    return 1;
  }
  int reuse = 1;
  ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(static_cast<std::uint16_t>(port));
  if (::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    std::perror("bind");
    return 1;
  }
  if (::listen(listen_fd, 64) != 0) {
    std::perror("listen");
    return 1;
  }

  const unsigned num_threads = std::max(2u, std::thread::hardware_concurrency());
  std::fprintf(stderr, "columnar_server listening on port %d with %u worker threads\n", port,
               num_threads);
  ThreadPool pool(num_threads);

  for (;;) {
    sockaddr_in client_addr{};
    socklen_t client_len = sizeof(client_addr);
    const int client_fd = ::accept(listen_fd, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
    if (client_fd < 0) {
      if (errno == EINTR) continue;
      std::perror("accept");
      continue;
    }
    pool.submit([client_fd, &catalog, &db] { handle_connection(client_fd, catalog, db); });
  }
}
