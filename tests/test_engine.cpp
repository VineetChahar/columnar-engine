// End-to-end tests for the SQL front end + vectorized executor + optimizer
// (Phases 2-4): lex -> parse -> bind -> optimize -> build_physical_plan ->
// pull batches. Uses a tiny in-memory Table built directly (not through the
// on-disk format -- that's covered by test_file_format.cpp already).

#include <algorithm>
#include <array>
#include <memory>

#include <catch2/catch_test_macros.hpp>

#include "columnar/catalog.hpp"
#include "columnar/logical_plan.hpp"
#include "columnar/operators.hpp"
#include "columnar/optimizer.hpp"
#include "columnar/parser.hpp"

using namespace columnar;

namespace {

ColumnChunkVariant make_int64_chunk(std::vector<std::int64_t> values) {
  std::vector<bool> valid_flags(values.size(), true);
  auto valid = std::make_unique<bool[]>(values.size());
  std::fill_n(valid.get(), values.size(), true);
  return ColumnChunkVariant(
      PlainColumnChunk<std::int64_t>(values, std::span<const bool>(valid.get(), values.size())));
}

ColumnChunkVariant make_double_chunk(std::vector<double> values) {
  auto valid = std::make_unique<bool[]>(values.size());
  std::fill_n(valid.get(), values.size(), true);
  return ColumnChunkVariant(
      PlainColumnChunk<double>(values, std::span<const bool>(valid.get(), values.size())));
}

ColumnChunkVariant make_dict_chunk(std::vector<std::string> values) {
  std::vector<std::string_view> views(values.begin(), values.end());
  auto valid = std::make_unique<bool[]>(values.size());
  std::fill_n(valid.get(), values.size(), true);
  return ColumnChunkVariant(DictionaryColumnChunk(views, std::span<const bool>(valid.get(), values.size())));
}

struct TestFixture {
  Catalog catalog;
  Database db;

  TestFixture() {
    catalog.add_table({"orders",
                        {{"id", TypeId::kInt64},
                         {"amount", TypeId::kDouble},
                         {"status", TypeId::kVarchar}}});
    catalog.add_table({"customers", {{"id", TypeId::kInt64}, {"name", TypeId::kVarchar}}});

    Table orders;
    Column id_col;
    id_col.schema = {"id", TypeId::kInt64};
    id_col.chunks.push_back(make_int64_chunk({1, 2, 3, 4, 5}));
    id_col.chunks.push_back(make_int64_chunk({6, 7, 8, 9, 10}));
    id_col.chunks.push_back(make_int64_chunk({11, 12, 13}));

    Column amount_col;
    amount_col.schema = {"amount", TypeId::kDouble};
    amount_col.chunks.push_back(make_double_chunk({10.0, 20.0, 30.0, 40.0, 50.0}));
    amount_col.chunks.push_back(make_double_chunk({200.0, 210.0, 220.0, 230.0, 240.0}));
    amount_col.chunks.push_back(make_double_chunk({5.0, 6.0, 7.0}));

    Column status_col;
    status_col.schema = {"status", TypeId::kVarchar};
    status_col.chunks.push_back(make_dict_chunk({"PAID", "PAID", "PENDING", "PAID", "PAID"}));
    status_col.chunks.push_back(
        make_dict_chunk({"PENDING", "PENDING", "PENDING", "PENDING", "PENDING"}));
    status_col.chunks.push_back(make_dict_chunk({"PAID", "PAID", "PAID"}));

    orders.columns.push_back(std::move(id_col));
    orders.columns.push_back(std::move(amount_col));
    orders.columns.push_back(std::move(status_col));
    db.emplace("orders", std::move(orders));

    Table customers;
    Column cid_col;
    cid_col.schema = {"id", TypeId::kInt64};
    cid_col.chunks.push_back(make_int64_chunk({1, 2, 3}));
    Column name_col;
    name_col.schema = {"name", TypeId::kVarchar};
    name_col.chunks.push_back(make_dict_chunk({"Alice", "Bob", "Carol"}));
    customers.columns.push_back(std::move(cid_col));
    customers.columns.push_back(std::move(name_col));
    db.emplace("customers", std::move(customers));
  }
};

// Runs a query end to end and returns all output rows as a vector of
// stringified cells, so tests can assert on results without caring about
// batch boundaries.
std::vector<std::vector<std::string>> run(const std::string& sql, const Catalog& catalog,
                                           const Database& db, ScanStats* stats = nullptr) {
  AstQuery ast = parse_query(sql);
  LogicalPlanPtr plan = bind_query(ast, catalog);
  plan = optimize(std::move(plan));
  ExprArena arena;
  OperatorPtr root = build_physical_plan(*plan, db, stats, arena);

  std::vector<std::vector<std::string>> rows;
  while (std::optional<ExecBatch> batch = root->next()) {
    for (std::size_t r = 0; r < batch->row_count(); ++r) {
      std::vector<std::string> row;
      for (const ExecColumn& col : batch->columns) {
        if (!col.validity[r]) {
          row.push_back("NULL");
          continue;
        }
        switch (col.type) {
          case ExecType::kInt64: row.push_back(std::to_string(col.ints()[r])); break;
          case ExecType::kDouble: row.push_back(std::to_string(col.doubles()[r])); break;
          case ExecType::kBool: row.push_back(col.bools()[r] ? "true" : "false"); break;
          case ExecType::kText: row.push_back(col.texts()[r]); break;
        }
      }
      rows.push_back(std::move(row));
    }
  }
  return rows;
}

}  // namespace

TEST_CASE_METHOD(TestFixture, "WHERE filters rows and zone-map pruning skips chunks",
                  "[engine]") {
  ScanStats stats;
  auto rows = run("SELECT id FROM orders WHERE amount > 100", catalog, db, &stats);
  REQUIRE(rows.size() == 5);
  REQUIRE(stats.total_chunks == 3);
  REQUIRE(stats.pruned_chunks == 2);  // chunks [10-50] and [5-7] both fully <= 100
}

TEST_CASE_METHOD(TestFixture, "GROUP BY with COUNT/SUM aggregates", "[engine]") {
  auto rows = run("SELECT status, COUNT(*) AS n, SUM(amount) AS total FROM orders GROUP BY status "
                   "ORDER BY total DESC",
                   catalog, db);
  REQUIRE(rows.size() == 2);
  REQUIRE(rows[0][0] == "PENDING");
  REQUIRE(rows[0][1] == "6");
  REQUIRE(rows[1][0] == "PAID");
  REQUIRE(rows[1][1] == "7");
}

TEST_CASE_METHOD(TestFixture, "ORDER BY DESC with LIMIT", "[engine]") {
  auto rows = run("SELECT id, amount FROM orders WHERE status = 'PAID' ORDER BY amount DESC LIMIT 3",
                   catalog, db);
  REQUIRE(rows.size() == 3);
  REQUIRE(rows[0][0] == "5");   // amount 50
  REQUIRE(rows[1][0] == "4");   // amount 40
  REQUIRE(rows[2][0] == "2");   // amount 20
}

TEST_CASE_METHOD(TestFixture, "INNER JOIN on equality", "[engine]") {
  auto rows = run("SELECT o.id, c.name, o.amount FROM orders o JOIN customers c ON o.id = c.id",
                   catalog, db);
  REQUIRE(rows.size() == 3);
  std::vector<std::string> names;
  for (auto& r : rows) names.push_back(r[1]);
  std::sort(names.begin(), names.end());
  REQUIRE(names == std::vector<std::string>{"Alice", "Bob", "Carol"});
}

TEST_CASE_METHOD(TestFixture, "LEFT JOIN keeps unmatched left rows with NULLs", "[engine]") {
  auto rows = run(
      "SELECT o.id, c.name FROM orders o LEFT JOIN customers c ON o.id = c.id ORDER BY o.id",
      catalog, db);
  REQUIRE(rows.size() == 13);  // every order row, matched or not
  REQUIRE(rows[0][1] == "Alice");
  REQUIRE(rows[3][1] == "NULL");  // order id 4 has no matching customer
}

TEST_CASE_METHOD(TestFixture, "arithmetic expressions and aliases", "[engine]") {
  auto rows = run("SELECT amount * 2 AS doubled FROM orders WHERE id = 1", catalog, db);
  REQUIRE(rows.size() == 1);
  REQUIRE(rows[0][0] == "20.000000");
}

TEST_CASE_METHOD(TestFixture, "projection pushdown skips decoding unreferenced columns",
                  "[engine]") {
  AstQuery ast = parse_query("SELECT id FROM orders WHERE amount > 0");
  LogicalPlanPtr plan = bind_query(ast, catalog);
  plan = optimize(std::move(plan));
  // Root is Project(id) -> Filter(amount > 0) -> Scan. The Project only
  // needs `id`, but the WHERE clause also needs `amount` -- so 2 of 3
  // columns should be marked for decode, not 1 and not 3.
  const auto& project = static_cast<const LogicalProject&>(*plan);
  const auto& filter = static_cast<const LogicalFilter&>(project.input());
  const auto& scan = static_cast<const LogicalScan&>(filter.input());
  REQUIRE(scan.projected_column_indices.size() == 2);
}

TEST_CASE_METHOD(TestFixture, "lexer/parser error reports a position", "[engine]") {
  REQUIRE_THROWS_AS(parse_query("SELECT FROM orders"), QueryError);
  try {
    parse_query("SELECT FROM orders");
    FAIL("expected QueryError");
  } catch (const QueryError& e) {
    REQUIRE(e.line == 1);
    REQUIRE(e.col > 1);
  }
}

TEST_CASE_METHOD(TestFixture, "binder reports unknown column with position", "[engine]") {
  AstQuery ast = parse_query("SELECT nope FROM orders");
  REQUIRE_THROWS_AS(bind_query(ast, catalog), QueryError);
}

TEST_CASE_METHOD(TestFixture, "binder reports ambiguous/unknown table", "[engine]") {
  AstQuery ast = parse_query("SELECT * FROM no_such_table");
  REQUIRE_THROWS_AS(bind_query(ast, catalog), QueryError);
}
