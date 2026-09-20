// Generates a synthetic analytical dataset -- an "orders" fact table and a
// small "customers" dimension table -- and writes it in three forms from
// ONE in-memory generation pass, so the engine and SQLite comparison in
// bench_vs_sqlite.cpp are guaranteed to run over byte-identical data:
//
//   data/orders.cef, data/customers.cef   -- this engine's on-disk format
//   data/bench.db                          -- a SQLite database
//
// Synthetic and generated entirely offline by code in this repo, per the
// project's zero-credentials/local-only constraint -- no downloaded
// dataset (TPC-H's own generator is a separate C program with its own
// build; a hand-rolled generator matching TPC-H's *shape* -- a fact table
// with a low-cardinality status/region and a join to a dimension table --
// gets the same analytical-workload character without adding a build
// dependency).
//
// Usage: generate_data <num_orders> <num_customers> <output_dir>

#include <sqlite3.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <vector>

#include "columnar/column_chunk.hpp"
#include "columnar/file_format.hpp"

using namespace columnar;

namespace {

constexpr const char* kStatuses[] = {"PAID", "PENDING", "CANCELLED", "REFUNDED"};
constexpr const char* kRegions[] = {"US", "EU", "APAC", "LATAM", "MEA"};

struct OrdersData {
  std::vector<std::int64_t> order_id;
  std::vector<std::int64_t> customer_id;
  std::vector<double> amount;
  std::vector<std::int64_t> quantity;
  std::vector<std::string> status;
  std::vector<std::string> region;
};

struct CustomersData {
  std::vector<std::int64_t> id;
  std::vector<std::string> name;
  std::vector<std::string> region;
};

OrdersData generate_orders(std::int64_t n, std::int64_t num_customers) {
  OrdersData d;
  d.order_id.reserve(n);
  d.customer_id.reserve(n);
  d.amount.reserve(n);
  d.quantity.reserve(n);
  d.status.reserve(n);
  d.region.reserve(n);

  std::mt19937_64 rng(42);
  std::uniform_int_distribution<std::int64_t> customer_dist(0, num_customers - 1);
  std::uniform_real_distribution<double> amount_dist(5.0, 2000.0);
  std::uniform_int_distribution<int> qty_dist(1, 20);
  std::uniform_int_distribution<int> status_dist(0, 3);
  std::uniform_int_distribution<int> region_dist(0, 4);

  for (std::int64_t i = 0; i < n; ++i) {
    d.order_id.push_back(i);
    d.customer_id.push_back(customer_dist(rng));
    d.amount.push_back(std::round(amount_dist(rng) * 100.0) / 100.0);
    d.quantity.push_back(qty_dist(rng));
    d.status.push_back(kStatuses[status_dist(rng)]);
    d.region.push_back(kRegions[region_dist(rng)]);
  }
  return d;
}

CustomersData generate_customers(std::int64_t n) {
  CustomersData d;
  d.id.reserve(n);
  d.name.reserve(n);
  d.region.reserve(n);
  std::mt19937_64 rng(99);
  std::uniform_int_distribution<int> region_dist(0, 4);
  for (std::int64_t i = 0; i < n; ++i) {
    d.id.push_back(i);
    d.name.push_back("customer_" + std::to_string(i));
    d.region.push_back(kRegions[region_dist(rng)]);
  }
  return d;
}

std::unique_ptr<bool[]> all_valid(std::size_t n) {
  auto mask = std::make_unique<bool[]>(n);
  std::fill_n(mask.get(), n, true);
  return mask;
}

void write_orders_cef(const OrdersData& d, const std::filesystem::path& path) {
  Table table;
  Column order_id_col{{"order_id", TypeId::kInt64}, {}};
  Column customer_id_col{{"customer_id", TypeId::kInt64}, {}};
  Column amount_col{{"amount", TypeId::kDouble}, {}};
  Column quantity_col{{"quantity", TypeId::kInt64}, {}};
  Column status_col{{"status", TypeId::kVarchar}, {}};
  Column region_col{{"region", TypeId::kVarchar}, {}};

  const std::size_t n = d.order_id.size();
  for (std::size_t start = 0; start < n; start += kVectorSize) {
    const std::size_t end = std::min(start + kVectorSize, n);
    const std::size_t len = end - start;
    auto valid = all_valid(len);
    std::span<const bool> validity(valid.get(), len);

    std::vector<std::int64_t> oid(d.order_id.begin() + static_cast<long>(start), d.order_id.begin() + static_cast<long>(end));
    std::vector<std::int64_t> cid(d.customer_id.begin() + static_cast<long>(start), d.customer_id.begin() + static_cast<long>(end));
    std::vector<double> amt(d.amount.begin() + static_cast<long>(start), d.amount.begin() + static_cast<long>(end));
    std::vector<std::int64_t> qty(d.quantity.begin() + static_cast<long>(start), d.quantity.begin() + static_cast<long>(end));
    std::vector<std::string_view> status_views(d.status.begin() + static_cast<long>(start), d.status.begin() + static_cast<long>(end));
    std::vector<std::string_view> region_views(d.region.begin() + static_cast<long>(start), d.region.begin() + static_cast<long>(end));

    order_id_col.chunks.push_back(ColumnChunkVariant(PlainColumnChunk<std::int64_t>(oid, validity)));
    customer_id_col.chunks.push_back(ColumnChunkVariant(PlainColumnChunk<std::int64_t>(cid, validity)));
    amount_col.chunks.push_back(ColumnChunkVariant(PlainColumnChunk<double>(amt, validity)));
    quantity_col.chunks.push_back(ColumnChunkVariant(PlainColumnChunk<std::int64_t>(qty, validity)));
    status_col.chunks.push_back(ColumnChunkVariant(DictionaryColumnChunk(status_views, validity)));
    region_col.chunks.push_back(ColumnChunkVariant(DictionaryColumnChunk(region_views, validity)));
  }

  table.columns.push_back(std::move(order_id_col));
  table.columns.push_back(std::move(customer_id_col));
  table.columns.push_back(std::move(amount_col));
  table.columns.push_back(std::move(quantity_col));
  table.columns.push_back(std::move(status_col));
  table.columns.push_back(std::move(region_col));
  write_table(path, table);
}

void write_customers_cef(const CustomersData& d, const std::filesystem::path& path) {
  Table table;
  Column id_col{{"id", TypeId::kInt64}, {}};
  Column name_col{{"name", TypeId::kVarchar}, {}};
  Column region_col{{"region", TypeId::kVarchar}, {}};

  const std::size_t n = d.id.size();
  for (std::size_t start = 0; start < n; start += kVectorSize) {
    const std::size_t end = std::min(start + kVectorSize, n);
    const std::size_t len = end - start;
    auto valid = all_valid(len);
    std::span<const bool> validity(valid.get(), len);

    std::vector<std::int64_t> ids(d.id.begin() + static_cast<long>(start), d.id.begin() + static_cast<long>(end));
    std::vector<std::string_view> names(d.name.begin() + static_cast<long>(start), d.name.begin() + static_cast<long>(end));
    std::vector<std::string_view> regions(d.region.begin() + static_cast<long>(start), d.region.begin() + static_cast<long>(end));

    id_col.chunks.push_back(ColumnChunkVariant(PlainColumnChunk<std::int64_t>(ids, validity)));
    name_col.chunks.push_back(ColumnChunkVariant(VarcharColumnChunk(names, validity)));
    region_col.chunks.push_back(ColumnChunkVariant(DictionaryColumnChunk(regions, validity)));
  }
  table.columns.push_back(std::move(id_col));
  table.columns.push_back(std::move(name_col));
  table.columns.push_back(std::move(region_col));
  write_table(path, table);
}

void exec_or_die(sqlite3* db, const char* sql) {
  char* err = nullptr;
  if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
    std::fprintf(stderr, "sqlite error: %s\nsql: %s\n", err, sql);
    sqlite3_free(err);
    std::exit(1);
  }
}

void write_sqlite_db(const OrdersData& orders, const CustomersData& customers,
                      const std::filesystem::path& path) {
  std::filesystem::remove(path);
  sqlite3* db = nullptr;
  if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) {
    std::fprintf(stderr, "cannot open %s\n", path.c_str());
    std::exit(1);
  }

  exec_or_die(db, "PRAGMA synchronous = OFF;");
  exec_or_die(db, "PRAGMA journal_mode = MEMORY;");
  exec_or_die(db,
              "CREATE TABLE customers (id INTEGER PRIMARY KEY, name TEXT, region TEXT);");
  exec_or_die(db,
              "CREATE TABLE orders (order_id INTEGER PRIMARY KEY, customer_id INTEGER, "
              "amount REAL, quantity INTEGER, status TEXT, region TEXT);");
  // A realistic OLTP-style setup indexes the foreign key -- this is what
  // makes SQLite competitive (or better) on the point-lookup/join-probe
  // query in the benchmark set. Giving SQLite this and NOT giving the
  // columnar engine an equivalent index is the honest, apples-to-apples
  // comparison: it's what each system's idiomatic setup actually looks
  // like, not a rigged one.
  exec_or_die(db, "CREATE INDEX idx_orders_customer_id ON orders(customer_id);");

  exec_or_die(db, "BEGIN TRANSACTION;");
  sqlite3_stmt* stmt = nullptr;
  sqlite3_prepare_v2(db, "INSERT INTO customers VALUES (?, ?, ?);", -1, &stmt, nullptr);
  for (std::size_t i = 0; i < customers.id.size(); ++i) {
    sqlite3_bind_int64(stmt, 1, customers.id[i]);
    sqlite3_bind_text(stmt, 2, customers.name[i].c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, customers.region[i].c_str(), -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_reset(stmt);
  }
  sqlite3_finalize(stmt);

  sqlite3_prepare_v2(db, "INSERT INTO orders VALUES (?, ?, ?, ?, ?, ?);", -1, &stmt, nullptr);
  for (std::size_t i = 0; i < orders.order_id.size(); ++i) {
    sqlite3_bind_int64(stmt, 1, orders.order_id[i]);
    sqlite3_bind_int64(stmt, 2, orders.customer_id[i]);
    sqlite3_bind_double(stmt, 3, orders.amount[i]);
    sqlite3_bind_int64(stmt, 4, orders.quantity[i]);
    sqlite3_bind_text(stmt, 5, orders.status[i].c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 6, orders.region[i].c_str(), -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_reset(stmt);
  }
  sqlite3_finalize(stmt);
  exec_or_die(db, "COMMIT;");
  exec_or_die(db, "ANALYZE;");
  sqlite3_close(db);
}

}  // namespace

int main(int argc, char** argv) {
  const std::int64_t num_orders = argc > 1 ? std::atoll(argv[1]) : 2000000;
  const std::int64_t num_customers = argc > 2 ? std::atoll(argv[2]) : 2000;
  const std::filesystem::path out_dir = argc > 3 ? argv[3] : "data";
  std::filesystem::create_directories(out_dir);

  std::printf("generating %lld orders / %lld customers...\n", static_cast<long long>(num_orders),
              static_cast<long long>(num_customers));
  OrdersData orders = generate_orders(num_orders, num_customers);
  CustomersData customers = generate_customers(num_customers);

  std::printf("writing %s...\n", (out_dir / "orders.cef").c_str());
  write_orders_cef(orders, out_dir / "orders.cef");
  std::printf("writing %s...\n", (out_dir / "customers.cef").c_str());
  write_customers_cef(customers, out_dir / "customers.cef");
  std::printf("writing %s...\n", (out_dir / "bench.db").c_str());
  write_sqlite_db(orders, customers, out_dir / "bench.db");

  std::printf("done.\n");
  return 0;
}
