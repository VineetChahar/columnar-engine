#pragma once

#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "columnar/exec_batch.hpp"
#include "columnar/expression.hpp"
#include "columnar/file_format.hpp"
#include "columnar/logical_plan.hpp"

namespace columnar {

// Every operator pulls from its child and returns one batch (<= kVectorSize
// rows) at a time -- "batched pull" / vectorized Volcano, per DESIGN.md
// section 5. The virtual call here happens once per BATCH, not once per
// row: the thing the rest of this project is about avoiding is per-row
// dispatch, not dispatch itself.
class Operator {
 public:
  virtual ~Operator() = default;
  virtual std::optional<ExecBatch> next() = 0;
};
using OperatorPtr = std::unique_ptr<Operator>;

struct ScanStats {
  std::size_t total_chunks = 0;
  std::size_t pruned_chunks = 0;
  std::size_t rows_produced = 0;
};

// Pulls every batch from `op` and concatenates them into one ExecBatch.
// Used internally by HashAggregate/Sort/HashJoin's build side (anything
// that needs to see all its input before producing output), and by the
// server to materialize a whole result set for one wire response.
ExecBatch materialize_all(Operator& op);

// Decodes one storage ColumnChunkVariant (whatever its encoding) into a
// dense ExecColumn. This is the boundary where storage encoding stops
// mattering to the rest of the engine -- dictionary/RLE columns get
// expanded back to plain values here, once per chunk, rather than every
// operator having to understand every encoding.
ExecColumn decode_chunk(const ColumnChunkVariant& chunk);

// True if `chunk`'s zone map proves the predicate cannot match any row in
// it. Conservative: returns true (must scan) for anything it can't prove.
bool zone_map_may_match(const ColumnChunkVariant& chunk, const ScanPredicate& pred);

class ScanOperator : public Operator {
 public:
  // `decode_mask` (same length as column_indices) says which columns are
  // actually read off disk this query; false entries get a cheap all-null
  // placeholder instead (this is projection pushdown -- Phase 4 sets this
  // from which columns the plan above the scan actually references). An
  // empty `decode_mask` means "decode everything" (no pushdown applied).
  ScanOperator(const Table& table, std::vector<std::size_t> column_indices,
               std::vector<std::string> column_names, std::vector<ScanPredicate> predicates,
               std::vector<bool> decode_mask, ScanStats* stats);
  std::optional<ExecBatch> next() override;

 private:
  const Table& table_;
  std::vector<std::size_t> column_indices_;
  std::vector<std::string> column_names_;
  std::vector<ScanPredicate> predicates_;
  std::vector<bool> decode_mask_;
  ScanStats* stats_;
  std::size_t next_chunk_ = 0;
};

// Evaluates `predicate` over each incoming batch and gathers surviving rows
// into a dense output batch. See LEARNING.md for the documented gap
// against DESIGN.md's selection-vector-all-the-way-through ideal: this
// compacts immediately rather than deferring compaction across a chain of
// operators.
class FilterOperator : public Operator {
 public:
  FilterOperator(OperatorPtr input, const Expression& predicate);
  std::optional<ExecBatch> next() override;

 private:
  OperatorPtr input_;
  const Expression& predicate_;
};

class ProjectOperator : public Operator {
 public:
  ProjectOperator(OperatorPtr input, const std::vector<ExpressionPtr>& exprs,
                   std::vector<std::string> names);
  std::optional<ExecBatch> next() override;

 private:
  OperatorPtr input_;
  const std::vector<ExpressionPtr>& exprs_;
  std::vector<std::string> names_;
};

class HashAggregateOperator : public Operator {
 public:
  HashAggregateOperator(OperatorPtr input, const std::vector<ExpressionPtr>& group_by,
                         const std::vector<AggregateItem>& aggregates,
                         std::vector<std::string> output_names);
  std::optional<ExecBatch> next() override;

 private:
  void compute();

  OperatorPtr input_;
  const std::vector<ExpressionPtr>& group_by_;
  const std::vector<AggregateItem>& aggregates_;
  std::vector<std::string> output_names_;
  bool computed_ = false;
  std::vector<ExecBatch> output_batches_;
  std::size_t next_batch_ = 0;
};

class HashJoinOperator : public Operator {
 public:
  HashJoinOperator(OperatorPtr left, OperatorPtr right, JoinType type,
                    std::size_t left_key_index, std::size_t right_key_index,
                    std::vector<std::string> output_names);
  std::optional<ExecBatch> next() override;

 private:
  void build_right_side();

  OperatorPtr left_;
  OperatorPtr right_;
  JoinType type_;
  std::size_t left_key_index_;
  std::size_t right_key_index_;
  std::vector<std::string> output_names_;
  bool built_ = false;
  ExecBatch build_batch_;
  std::unordered_map<std::string, std::vector<std::uint32_t>> build_index_;
};

class SortOperator : public Operator {
 public:
  SortOperator(OperatorPtr input, const std::vector<SortKey>& keys);
  std::optional<ExecBatch> next() override;

 private:
  void compute();

  OperatorPtr input_;
  const std::vector<SortKey>& keys_;
  bool computed_ = false;
  std::vector<ExecBatch> output_batches_;
  std::size_t next_batch_ = 0;
};

class LimitOperator : public Operator {
 public:
  LimitOperator(OperatorPtr input, std::uint64_t limit);
  std::optional<ExecBatch> next() override;

 private:
  OperatorPtr input_;
  std::uint64_t remaining_;
};

using Database = std::unordered_map<std::string, Table>;

// Lowering a join sometimes has to synthesize a small reordering
// projection (see build_physical_plan's join build-side swap) that isn't
// owned by any LogicalPlan node. Those live here instead. A deque (not a
// vector) specifically so pushing a new entry never invalidates a
// reference an earlier ProjectOperator already took to `arena.back()` --
// the same reallocation-invalidation hazard as Phase 1's dictionary
// builder, just at the physical-plan layer.
using ExprArena = std::deque<std::vector<ExpressionPtr>>;

// Lowers a bound LogicalPlan into an Operator tree over real data. The
// returned Operator (and every Expression it evaluates) borrows from
// `plan`, `db`, and `arena` -- all three must outlive the operator tree.
OperatorPtr build_physical_plan(const LogicalPlan& plan, const Database& db, ScanStats* stats,
                                 ExprArena& arena);

}  // namespace columnar
