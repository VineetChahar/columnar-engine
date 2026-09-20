// Benchmarks for each Phase 3 operator in isolation, plus the tuple-at-a-
// time vs. vectorized comparison DESIGN.md section 5 promises: the same
// filter predicate, one row at a time vs. one batch at a time, so the win
// is a measured number instead of an assertion.

#include <memory>
#include <random>
#include <vector>

#include <benchmark/benchmark.h>

#include "columnar/column_chunk.hpp"
#include "columnar/expression.hpp"
#include "columnar/file_format.hpp"
#include "columnar/operators.hpp"

using namespace columnar;

namespace {

constexpr std::size_t kRowsPerChunk = kVectorSize;

std::unique_ptr<bool[]> all_valid(std::size_t n) {
  auto mask = std::make_unique<bool[]>(n);
  std::fill_n(mask.get(), n, true);
  return mask;
}

// A single-column (int64 "amount") table of `num_chunks` chunks of
// kVectorSize rows each, values uniformly spread over [0, 1000) so a
// `WHERE amount > threshold` predicate has a controllable, known
// selectivity.
Table make_amount_table(std::size_t num_chunks) {
  Table table;
  Column col;
  col.schema = {"amount", TypeId::kInt64};
  std::mt19937 rng(7);
  std::uniform_int_distribution<int> dist(0, 999);
  auto valid = all_valid(kRowsPerChunk);
  std::span<const bool> validity(valid.get(), kRowsPerChunk);

  for (std::size_t c = 0; c < num_chunks; ++c) {
    std::vector<std::int64_t> values(kRowsPerChunk);
    for (auto& v : values) v = dist(rng);
    col.chunks.push_back(ColumnChunkVariant(PlainColumnChunk<std::int64_t>(values, validity)));
  }
  table.columns.push_back(std::move(col));
  return table;
}

ExecBatch decode_one_chunk(const Table& table, std::size_t chunk_index) {
  ExecBatch batch;
  batch.column_names = {"amount"};
  batch.columns.push_back(decode_chunk(table.columns[0].chunks[chunk_index]));
  return batch;
}

// ---- Reference #1: classic Volcano, tuple-at-a-time, virtual dispatch ----
//
// One next_row() virtual call per ROW, through a Scan->Filter iterator
// chain -- exactly the model DESIGN.md section 5 names as what
// vectorization has to beat: per-row dispatch overhead and no batch
// amortization. This is the fair comparison point for the real
// ScanOperator->FilterOperator pipeline below (BM_FilterVectorizedPipeline):
// same two-stage plan, same predicate, same data, same total rows.
class RowIterator {
 public:
  virtual ~RowIterator() = default;
  virtual bool next_row(std::int64_t& value) = 0;
};

class TupleScanIterator : public RowIterator {
 public:
  explicit TupleScanIterator(const Table& table) : table_(table) {}
  bool next_row(std::int64_t& value) override {
    const auto& chunks = table_.columns[0].chunks;
    while (chunk_index_ < chunks.size()) {
      const auto& chunk = std::get<PlainColumnChunk<std::int64_t>>(chunks[chunk_index_]);
      if (row_index_ >= chunk.row_count()) {
        ++chunk_index_;
        row_index_ = 0;
        continue;
      }
      const bool valid = !chunk.is_null(row_index_);
      value = chunk.value(row_index_);
      ++row_index_;
      if (valid) return true;
    }
    return false;
  }

 private:
  const Table& table_;
  std::size_t chunk_index_ = 0;
  std::size_t row_index_ = 0;
};

class TupleFilterIterator : public RowIterator {
 public:
  TupleFilterIterator(std::unique_ptr<RowIterator> input, std::int64_t threshold)
      : input_(std::move(input)), threshold_(threshold) {}
  bool next_row(std::int64_t& value) override {
    std::int64_t v;
    while (input_->next_row(v)) {
      if (v > threshold_) {
        value = v;
        return true;
      }
    }
    return false;
  }

 private:
  std::unique_ptr<RowIterator> input_;
  std::int64_t threshold_;
};

// ---- Reference #2: a raw scalar loop, no dispatch, no batching at all ----
//
// Not the Volcano comparison -- this is a *different*, also-interesting
// data point: what a hand-written loop over already-decoded arrays costs
// with zero abstraction whatsoever. Compared against
// BM_FilterExpressionEvaluator below, it's what first revealed that this
// project's naive Expression::evaluate() was actually *slower* than this
// loop for a cheap predicate, because ColumnRefExpr/LiteralExpr::evaluate()
// each materialize a fresh, fully-copied ExecColumn -- see LEARNING.md.
std::size_t filter_raw_scalar_loop(const ExecColumn& col, std::int64_t threshold) {
  std::size_t count = 0;
  for (std::size_t i = 0; i < col.size(); ++i) {
    if (col.validity[i] && col.ints()[i] > threshold) {
      ++count;
    }
  }
  return count;
}

std::size_t filter_expression_evaluator(const ExecBatch& batch, const Expression& predicate) {
  ExecColumn mask = predicate.evaluate(batch, nullptr);
  std::size_t count = 0;
  for (std::size_t i = 0; i < mask.size(); ++i) {
    count += (mask.validity[i] && mask.bools()[i]) ? 1 : 0;
  }
  return count;
}

}  // namespace

// ------------------------------------------------------------- Scan --

static void BM_Scan(benchmark::State& state) {
  const std::size_t num_chunks = static_cast<std::size_t>(state.range(0));
  Table table = make_amount_table(num_chunks);
  for (auto _ : state) {
    ScanStats stats;
    ScanOperator scan(table, {0}, {"amount"}, {}, {}, &stats);
    std::size_t rows = 0;
    while (std::optional<ExecBatch> batch = scan.next()) rows += batch->row_count();
    benchmark::DoNotOptimize(rows);
  }
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * num_chunks * kRowsPerChunk));
}
BENCHMARK(BM_Scan)->Arg(16)->Arg(256);

// --------------------------- Volcano tuple-at-a-time vs vectorized pipeline --
//
// Same logical query (`SELECT amount FROM t WHERE amount > 500`), same
// data, same total row count -- one built as a virtual-dispatch-per-row
// Scan->Filter iterator chain, the other as the project's real
// ScanOperator->FilterOperator batch pipeline. This is the number
// DESIGN.md section 5 asks for.

static void BM_FilterVolcanoTupleAtATime(benchmark::State& state) {
  const std::size_t num_chunks = static_cast<std::size_t>(state.range(0));
  Table table = make_amount_table(num_chunks);
  for (auto _ : state) {
    std::unique_ptr<RowIterator> scan = std::make_unique<TupleScanIterator>(table);
    TupleFilterIterator filter(std::move(scan), 500);
    std::size_t count = 0;
    std::int64_t v;
    while (filter.next_row(v)) ++count;
    benchmark::DoNotOptimize(count);
  }
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * num_chunks * kRowsPerChunk));
}
BENCHMARK(BM_FilterVolcanoTupleAtATime)->Arg(64);

static void BM_FilterVectorizedPipeline(benchmark::State& state) {
  const std::size_t num_chunks = static_cast<std::size_t>(state.range(0));
  Table table = make_amount_table(num_chunks);
  auto predicate = std::make_unique<BinaryExpr>(
      BinaryOp::kGt, std::make_unique<ColumnRefExpr>("amount", 0, ExecType::kInt64),
      LiteralExpr::make_int(500), ExecType::kBool);
  for (auto _ : state) {
    ScanStats stats;
    auto scan = std::make_unique<ScanOperator>(table, std::vector<std::size_t>{0},
                                                std::vector<std::string>{"amount"},
                                                std::vector<ScanPredicate>{}, std::vector<bool>{},
                                                &stats);
    FilterOperator filter(std::move(scan), *predicate);
    std::size_t count = 0;
    while (std::optional<ExecBatch> batch = filter.next()) count += batch->row_count();
    benchmark::DoNotOptimize(count);
  }
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * num_chunks * kRowsPerChunk));
}
BENCHMARK(BM_FilterVectorizedPipeline)->Arg(64);

// ------------------------- raw scalar loop vs. this project's expression evaluator --

static void BM_FilterRawScalarLoop(benchmark::State& state) {
  Table table = make_amount_table(1);
  ExecBatch batch = decode_one_chunk(table, 0);
  for (auto _ : state) {
    std::size_t count = filter_raw_scalar_loop(batch.columns[0], 500);
    benchmark::DoNotOptimize(count);
  }
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * kRowsPerChunk));
}
BENCHMARK(BM_FilterRawScalarLoop);

static void BM_FilterExpressionEvaluator(benchmark::State& state) {
  Table table = make_amount_table(1);
  ExecBatch batch = decode_one_chunk(table, 0);
  auto predicate = std::make_unique<BinaryExpr>(
      BinaryOp::kGt, std::make_unique<ColumnRefExpr>("amount", 0, ExecType::kInt64),
      LiteralExpr::make_int(500), ExecType::kBool);
  for (auto _ : state) {
    std::size_t count = filter_expression_evaluator(batch, *predicate);
    benchmark::DoNotOptimize(count);
  }
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * kRowsPerChunk));
}
BENCHMARK(BM_FilterExpressionEvaluator);

// --------------------------------------------------------- ProjectOperator --

static void BM_ProjectOperator(benchmark::State& state) {
  const std::size_t num_chunks = static_cast<std::size_t>(state.range(0));
  Table table = make_amount_table(num_chunks);
  std::vector<ExpressionPtr> exprs;
  exprs.push_back(std::make_unique<BinaryExpr>(
      BinaryOp::kMul, std::make_unique<ColumnRefExpr>("amount", 0, ExecType::kInt64),
      LiteralExpr::make_int(2), ExecType::kInt64));
  for (auto _ : state) {
    ScanStats stats;
    auto scan = std::make_unique<ScanOperator>(table, std::vector<std::size_t>{0},
                                                std::vector<std::string>{"amount"},
                                                std::vector<ScanPredicate>{}, std::vector<bool>{},
                                                &stats);
    ProjectOperator project(std::move(scan), exprs, {"doubled"});
    std::size_t rows = 0;
    while (std::optional<ExecBatch> batch = project.next()) rows += batch->row_count();
    benchmark::DoNotOptimize(rows);
  }
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * num_chunks * kRowsPerChunk));
}
BENCHMARK(BM_ProjectOperator)->Arg(64);

// ---------------------------------------------------- HashAggregateOperator --

Table make_grouped_table(std::size_t num_chunks, int cardinality) {
  Table table;
  Column key_col;
  key_col.schema = {"key", TypeId::kInt64};
  Column val_col;
  val_col.schema = {"val", TypeId::kInt64};
  std::mt19937 rng(11);
  std::uniform_int_distribution<int> key_dist(0, cardinality - 1);
  std::uniform_int_distribution<int> val_dist(0, 999);
  auto valid = all_valid(kRowsPerChunk);
  std::span<const bool> validity(valid.get(), kRowsPerChunk);

  for (std::size_t c = 0; c < num_chunks; ++c) {
    std::vector<std::int64_t> keys(kRowsPerChunk), vals(kRowsPerChunk);
    for (std::size_t i = 0; i < kRowsPerChunk; ++i) {
      keys[i] = key_dist(rng);
      vals[i] = val_dist(rng);
    }
    key_col.chunks.push_back(ColumnChunkVariant(PlainColumnChunk<std::int64_t>(keys, validity)));
    val_col.chunks.push_back(ColumnChunkVariant(PlainColumnChunk<std::int64_t>(vals, validity)));
  }
  table.columns.push_back(std::move(key_col));
  table.columns.push_back(std::move(val_col));
  return table;
}

static void BM_HashAggregate(benchmark::State& state) {
  const std::size_t num_chunks = static_cast<std::size_t>(state.range(0));
  Table table = make_grouped_table(num_chunks, 100);
  std::vector<ExpressionPtr> group_by;
  group_by.push_back(std::make_unique<ColumnRefExpr>("key", 0, ExecType::kInt64));
  std::vector<AggregateItem> aggs;
  AggregateItem sum_item;
  sum_item.func = AggFunc::kSum;
  sum_item.arg = std::make_unique<ColumnRefExpr>("val", 1, ExecType::kInt64);
  sum_item.output_name = "total";
  aggs.push_back(std::move(sum_item));

  for (auto _ : state) {
    ScanStats stats;
    auto scan = std::make_unique<ScanOperator>(
        table, std::vector<std::size_t>{0, 1}, std::vector<std::string>{"key", "val"},
        std::vector<ScanPredicate>{}, std::vector<bool>{}, &stats);
    HashAggregateOperator agg(std::move(scan), group_by, aggs, {"key", "total"});
    std::size_t rows = 0;
    while (std::optional<ExecBatch> batch = agg.next()) rows += batch->row_count();
    benchmark::DoNotOptimize(rows);
  }
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * num_chunks * kRowsPerChunk));
}
BENCHMARK(BM_HashAggregate)->Arg(64);

// -------------------------------------------------------- HashJoinOperator --

static void BM_HashJoin(benchmark::State& state) {
  const std::size_t left_chunks = static_cast<std::size_t>(state.range(0));
  Table left_table = make_grouped_table(left_chunks, 1000);
  Table right_table = make_grouped_table(4, 1000);  // small "dimension" side

  std::vector<ExpressionPtr> group_by;  // unused; reuse table builder for convenience

  for (auto _ : state) {
    ScanStats stats;
    auto left_scan = std::make_unique<ScanOperator>(
        left_table, std::vector<std::size_t>{0, 1}, std::vector<std::string>{"lkey", "lval"},
        std::vector<ScanPredicate>{}, std::vector<bool>{}, &stats);
    auto right_scan = std::make_unique<ScanOperator>(
        right_table, std::vector<std::size_t>{0, 1}, std::vector<std::string>{"rkey", "rval"},
        std::vector<ScanPredicate>{}, std::vector<bool>{}, &stats);
    HashJoinOperator join(std::move(left_scan), std::move(right_scan), JoinType::kInner,
                           /*left_key_index=*/0, /*right_key_index=*/0,
                           {"lkey", "lval", "rkey", "rval"});
    std::size_t rows = 0;
    while (std::optional<ExecBatch> batch = join.next()) rows += batch->row_count();
    benchmark::DoNotOptimize(rows);
  }
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * left_chunks * kRowsPerChunk));
}
BENCHMARK(BM_HashJoin)->Arg(32);

// ------------------------------------------------------------- SortOperator --

static void BM_Sort(benchmark::State& state) {
  const std::size_t num_chunks = static_cast<std::size_t>(state.range(0));
  Table table = make_amount_table(num_chunks);
  std::vector<SortKey> keys;
  keys.push_back(SortKey{std::make_unique<ColumnRefExpr>("amount", 0, ExecType::kInt64), false});

  for (auto _ : state) {
    ScanStats stats;
    auto scan = std::make_unique<ScanOperator>(table, std::vector<std::size_t>{0},
                                                std::vector<std::string>{"amount"},
                                                std::vector<ScanPredicate>{}, std::vector<bool>{},
                                                &stats);
    SortOperator sort(std::move(scan), keys);
    std::size_t rows = 0;
    while (std::optional<ExecBatch> batch = sort.next()) rows += batch->row_count();
    benchmark::DoNotOptimize(rows);
  }
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * num_chunks * kRowsPerChunk));
}
BENCHMARK(BM_Sort)->Arg(16);

// ------------------------------------------------------------ LimitOperator --

static void BM_Limit(benchmark::State& state) {
  const std::size_t num_chunks = static_cast<std::size_t>(state.range(0));
  Table table = make_amount_table(num_chunks);
  for (auto _ : state) {
    ScanStats stats;
    auto scan = std::make_unique<ScanOperator>(table, std::vector<std::size_t>{0},
                                                std::vector<std::string>{"amount"},
                                                std::vector<ScanPredicate>{}, std::vector<bool>{},
                                                &stats);
    LimitOperator limit(std::move(scan), 10);
    std::size_t rows = 0;
    while (std::optional<ExecBatch> batch = limit.next()) rows += batch->row_count();
    benchmark::DoNotOptimize(rows);
  }
}
BENCHMARK(BM_Limit)->Arg(64);

BENCHMARK_MAIN();
