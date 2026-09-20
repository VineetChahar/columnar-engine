// Runs the Q8 join+group-by query in a tight loop for a fixed duration, so
// an external sampler (macOS `sample`, or `perf record` on Linux) has a
// long-running, steady-state target to attach to.
//
// Usage: profile_join <data_dir> <seconds>

#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

#include "columnar/catalog.hpp"
#include "columnar/file_format.hpp"
#include "columnar/logical_plan.hpp"
#include "columnar/operators.hpp"
#include "columnar/optimizer.hpp"
#include "columnar/parser.hpp"

using namespace columnar;

int main(int argc, char** argv) {
  const std::filesystem::path data_dir = argc > 1 ? argv[1] : "data";
  const double seconds = argc > 2 ? std::atof(argv[2]) : 8.0;

  Database db;
  db.emplace("orders", read_table(data_dir / "orders.cef"));
  db.emplace("customers", read_table(data_dir / "customers.cef"));
  Catalog catalog;
  for (const auto& [name, table] : db) {
    CatalogTable ct;
    ct.name = name;
    for (const Column& col : table.columns) ct.columns.push_back({col.schema.name, col.schema.type});
    catalog.add_table(std::move(ct));
  }

  const std::string sql =
      "SELECT c.region, SUM(o.amount) AS total FROM orders o JOIN customers c ON o.customer_id = "
      "c.id GROUP BY c.region ORDER BY total DESC";

  std::printf("pid=%d looping Q8 for %.1fs...\n", getpid(), seconds);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
  std::size_t iterations = 0;
  volatile std::size_t sink = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    AstQuery ast = parse_query(sql);
    LogicalPlanPtr plan = bind_query(ast, catalog);
    plan = optimize(std::move(plan));
    ScanStats stats;
    ExprArena arena;
    OperatorPtr root = build_physical_plan(*plan, db, &stats, arena);
    std::size_t rows = 0;
    while (std::optional<ExecBatch> batch = root->next()) rows += batch->row_count();
    sink = rows;
    ++iterations;
  }
  std::printf("done: %zu iterations (last result had %zu rows)\n", iterations, sink);
  return 0;
}
