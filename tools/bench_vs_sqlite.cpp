// Runs the same 8 analytical SQL queries, unmodified, against this engine
// and against SQLite over byte-identical data (see generate_data.cpp), and
// reports wall-clock time for the full "hand it SQL text, get a result
// back" round trip on both sides -- parse+bind+optimize+execute for this
// engine, prepare+step+finalize for SQLite, no cached prepared statements
// on either side, since that's what a real client actually pays per query.
//
// Usage: bench_vs_sqlite <data_dir> [iterations]

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "columnar/catalog.hpp"
#include "columnar/file_format.hpp"
#include "columnar/logical_plan.hpp"
#include "columnar/operators.hpp"
#include "columnar/optimizer.hpp"
#include "columnar/parser.hpp"

using namespace columnar;
using Clock = std::chrono::steady_clock;

namespace {

struct QuerySpec {
  std::string name;
  std::string sql;
};

const std::vector<QuerySpec> kQueries = {
    {"Q1 point-filter count", "SELECT COUNT(*) AS n FROM orders WHERE status = 'PAID'"},
    {"Q2 group-by aggregate",
     "SELECT region, SUM(amount) AS total, COUNT(*) AS n FROM orders GROUP BY region ORDER BY "
     "total DESC"},
    {"Q3 range-filter avg", "SELECT AVG(amount) AS avg_amount FROM orders WHERE amount > 500"},
    {"Q4 group-by avg", "SELECT status, AVG(quantity) AS avg_qty FROM orders GROUP BY status"},
    {"Q5 point lookup (indexed on SQLite)", "SELECT order_id, amount FROM orders WHERE customer_id = 42"},
    {"Q6 two-column group-by",
     "SELECT region, status, COUNT(*) AS n FROM orders GROUP BY region, status ORDER BY region, "
     "status"},
    {"Q7 min/max with filter", "SELECT MIN(amount) AS lo, MAX(amount) AS hi FROM orders WHERE region = 'EU'"},
    {"Q8 join + group-by",
     "SELECT c.region, SUM(o.amount) AS total FROM orders o JOIN customers c ON o.customer_id = "
     "c.id GROUP BY c.region ORDER BY total DESC"},
};

Catalog build_catalog(const Database& db) {
  Catalog catalog;
  for (const auto& [name, table] : db) {
    CatalogTable ct;
    ct.name = name;
    for (const Column& col : table.columns) ct.columns.push_back({col.schema.name, col.schema.type});
    catalog.add_table(std::move(ct));
  }
  return catalog;
}

double run_engine_query(const std::string& sql, const Catalog& catalog, const Database& db,
                         std::size_t* out_row_count) {
  const auto t0 = Clock::now();
  AstQuery ast = parse_query(sql);
  LogicalPlanPtr plan = bind_query(ast, catalog);
  plan = optimize(std::move(plan));
  ScanStats stats;
  ExprArena arena;
  OperatorPtr root = build_physical_plan(*plan, db, &stats, arena);
  std::size_t rows = 0;
  while (std::optional<ExecBatch> batch = root->next()) rows += batch->row_count();
  const auto t1 = Clock::now();
  *out_row_count = rows;
  return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

double run_sqlite_query(sqlite3* db, const std::string& sql, std::size_t* out_row_count) {
  const auto t0 = Clock::now();
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
    std::fprintf(stderr, "sqlite prepare failed: %s\nsql: %s\n", sqlite3_errmsg(db), sql.c_str());
    std::exit(1);
  }
  std::size_t rows = 0;
  while (sqlite3_step(stmt) == SQLITE_ROW) ++rows;
  sqlite3_finalize(stmt);
  const auto t1 = Clock::now();
  *out_row_count = rows;
  return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

}  // namespace

int main(int argc, char** argv) {
  const std::filesystem::path data_dir = argc > 1 ? argv[1] : "data";
  const int iterations = argc > 2 ? std::atoi(argv[2]) : 7;

  std::printf("loading %s ...\n", data_dir.c_str());
  Database db;
  db.emplace("orders", read_table(data_dir / "orders.cef"));
  db.emplace("customers", read_table(data_dir / "customers.cef"));
  Catalog catalog = build_catalog(db);

  sqlite3* sqlite_db = nullptr;
  if (sqlite3_open_v2((data_dir / "bench.db").c_str(), &sqlite_db, SQLITE_OPEN_READONLY, nullptr) !=
      SQLITE_OK) {
    std::fprintf(stderr, "cannot open bench.db\n");
    return 1;
  }

  std::printf("\n%-38s %12s %12s %10s %8s\n", "query", "engine(ms)", "sqlite(ms)", "speedup",
              "rows");
  std::printf("%s\n", std::string(38 + 12 + 12 + 10 + 8 + 4, '-').c_str());

  std::vector<std::string> csv_lines;
  csv_lines.push_back("query,engine_ms,sqlite_ms,speedup,engine_rows,sqlite_rows");

  for (const QuerySpec& q : kQueries) {
    std::vector<double> engine_times, sqlite_times;
    std::size_t engine_rows = 0, sqlite_rows = 0;

    // One warmup iteration each (page cache, malloc arenas, disk cache),
    // then `iterations` measured runs; report the minimum -- the standard
    // way to suppress OS scheduling noise without pretending variance
    // doesn't exist (see README.md for the full per-iteration numbers).
    run_engine_query(q.sql, catalog, db, &engine_rows);
    run_sqlite_query(sqlite_db, q.sql, &sqlite_rows);

    for (int i = 0; i < iterations; ++i) {
      engine_times.push_back(run_engine_query(q.sql, catalog, db, &engine_rows));
      sqlite_times.push_back(run_sqlite_query(sqlite_db, q.sql, &sqlite_rows));
    }

    const double engine_ms = *std::min_element(engine_times.begin(), engine_times.end());
    const double sqlite_ms = *std::min_element(sqlite_times.begin(), sqlite_times.end());
    const double speedup = sqlite_ms / engine_ms;

    std::printf("%-38s %12.3f %12.3f %9.2fx %8zu\n", q.name.c_str(), engine_ms, sqlite_ms, speedup,
                engine_rows);
    if (engine_rows != sqlite_rows) {
      std::printf("  ** WARNING: row count mismatch (engine=%zu, sqlite=%zu) **\n", engine_rows,
                  sqlite_rows);
    }
    csv_lines.push_back(q.name + "," + std::to_string(engine_ms) + "," + std::to_string(sqlite_ms) +
                         "," + std::to_string(speedup) + "," + std::to_string(engine_rows) + "," +
                         std::to_string(sqlite_rows));
  }

  std::FILE* csv = std::fopen((data_dir / "bench_results.csv").c_str(), "w");
  if (csv) {
    for (const std::string& line : csv_lines) std::fprintf(csv, "%s\n", line.c_str());
    std::fclose(csv);
    std::printf("\nwrote %s\n", (data_dir / "bench_results.csv").c_str());
  }

  sqlite3_close(sqlite_db);
  return 0;
}
