#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "columnar/ast.hpp"
#include "columnar/catalog.hpp"
#include "columnar/expression.hpp"

namespace columnar {

enum class LogicalNodeType { kScan, kFilter, kProject, kAggregate, kJoin, kSort, kLimit };

// Every node's output schema -- a flat list of (name, ExecType) -- is
// computed once at bind time and stored on the node itself. Expression
// trees hung off a node (e.g. LogicalFilter's predicate) resolve
// ColumnRefExpr indices against the node's *input* schema; a node's own
// output_schema is what the *next* node up binds against. This is what
// lets the same Expression evaluation code work at every level without
// carrying a separate "current scope" object through execution.
struct OutputColumn {
  std::string name;
  ExecType type;
};

class LogicalPlan {
 public:
  explicit LogicalPlan(std::vector<OutputColumn> schema) : output_schema_(std::move(schema)) {}
  virtual ~LogicalPlan() = default;
  virtual LogicalNodeType node_type() const = 0;
  virtual std::string describe() const = 0;  // one line, no children
  const std::vector<OutputColumn>& output_schema() const { return output_schema_; }

 protected:
  std::vector<OutputColumn> output_schema_;
};

using LogicalPlanPtr = std::unique_ptr<LogicalPlan>;

// A predicate simple enough to test against a chunk's zone map without
// decoding it: `column OP literal`. Produced by the optimizer's predicate
// pushdown (optimizer.cpp) from a Filter directly above a Scan; the Filter
// itself is always kept too, since a zone map can only ever prove a chunk
// *can't* match -- it's a pruning hint, not a substitute for evaluation.
struct ScanPredicate {
  std::size_t column_index;  // index into this scan's own output columns
  BinaryOp op;
  ExecType type;
  std::int64_t int_value = 0;
  double double_value = 0.0;
  std::string text_value;
};

class LogicalScan : public LogicalPlan {
 public:
  LogicalScan(std::string table_name, std::vector<OutputColumn> schema)
      : LogicalPlan(std::move(schema)), table_name_(std::move(table_name)) {}

  LogicalNodeType node_type() const override { return LogicalNodeType::kScan; }
  std::string describe() const override;
  const std::string& table_name() const { return table_name_; }

  // Filled in by the optimizer's predicate pushdown (zone-map pruning hint;
  // see ScanPredicate above). Empty means nothing was pushed down.
  std::vector<ScanPredicate> zone_map_predicates;
  // Filled in by the optimizer's projection pushdown: indices (into this
  // scan's own column list) that are actually referenced above it. Empty
  // means "not computed / read everything" -- see optimizer.cpp. Only
  // computed for single-table (no-JOIN) query shapes; see LEARNING.md.
  std::vector<std::size_t> projected_column_indices;

 private:
  std::string table_name_;
};

class LogicalFilter : public LogicalPlan {
 public:
  LogicalFilter(LogicalPlanPtr input, ExpressionPtr predicate)
      : LogicalPlan(input->output_schema()), input_(std::move(input)),
        predicate_(std::move(predicate)) {}

  LogicalNodeType node_type() const override { return LogicalNodeType::kFilter; }
  std::string describe() const override { return "Filter(" + predicate_->to_string() + ")"; }

  const LogicalPlan& input() const { return *input_; }
  LogicalPlan& input() { return *input_; }
  LogicalPlanPtr take_input() { return std::move(input_); }
  void set_input(LogicalPlanPtr p) { input_ = std::move(p); }
  const Expression& predicate() const { return *predicate_; }
  ExpressionPtr take_predicate() { return std::move(predicate_); }

 private:
  LogicalPlanPtr input_;
  ExpressionPtr predicate_;
};

class LogicalProject : public LogicalPlan {
 public:
  LogicalProject(LogicalPlanPtr input, std::vector<ExpressionPtr> exprs,
                  std::vector<OutputColumn> schema)
      : LogicalPlan(std::move(schema)), input_(std::move(input)), exprs_(std::move(exprs)) {}

  LogicalNodeType node_type() const override { return LogicalNodeType::kProject; }
  std::string describe() const override {
    std::string s = "Project(";
    for (std::size_t i = 0; i < exprs_.size(); ++i) {
      if (i) s += ", ";
      s += output_schema_[i].name + " = " + exprs_[i]->to_string();
    }
    return s + ")";
  }

  const LogicalPlan& input() const { return *input_; }
  LogicalPlan& input() { return *input_; }
  const std::vector<ExpressionPtr>& exprs() const { return exprs_; }

 private:
  LogicalPlanPtr input_;
  std::vector<ExpressionPtr> exprs_;
};

struct AggregateItem {
  AggFunc func;
  ExpressionPtr arg;  // nullptr iff func == kCountStar
  std::string output_name;
};

class LogicalAggregate : public LogicalPlan {
 public:
  LogicalAggregate(LogicalPlanPtr input, std::vector<ExpressionPtr> group_by,
                    std::vector<AggregateItem> aggregates, std::vector<OutputColumn> schema)
      : LogicalPlan(std::move(schema)),
        input_(std::move(input)),
        group_by_(std::move(group_by)),
        aggregates_(std::move(aggregates)) {}

  LogicalNodeType node_type() const override { return LogicalNodeType::kAggregate; }
  std::string describe() const override {
    std::string s = "Aggregate(group_by=[";
    for (std::size_t i = 0; i < group_by_.size(); ++i) {
      if (i) s += ", ";
      s += group_by_[i]->to_string();
    }
    s += "], aggs=[";
    for (std::size_t i = 0; i < aggregates_.size(); ++i) {
      if (i) s += ", ";
      s += aggregates_[i].output_name;
    }
    return s + "])";
  }

  const LogicalPlan& input() const { return *input_; }
  LogicalPlan& input() { return *input_; }
  const std::vector<ExpressionPtr>& group_by() const { return group_by_; }
  const std::vector<AggregateItem>& aggregates() const { return aggregates_; }

 private:
  LogicalPlanPtr input_;
  std::vector<ExpressionPtr> group_by_;
  std::vector<AggregateItem> aggregates_;
};

class LogicalJoin : public LogicalPlan {
 public:
  LogicalJoin(LogicalPlanPtr left, LogicalPlanPtr right, JoinType type, ExpressionPtr condition,
              std::vector<OutputColumn> schema)
      : LogicalPlan(std::move(schema)),
        left_(std::move(left)),
        right_(std::move(right)),
        type_(type),
        condition_(std::move(condition)) {}

  LogicalNodeType node_type() const override { return LogicalNodeType::kJoin; }
  std::string describe() const override {
    return std::string("Join(") + (type_ == JoinType::kInner ? "INNER" : "LEFT") +
           ", on=" + condition_->to_string() + ")";
  }

  const LogicalPlan& left() const { return *left_; }
  const LogicalPlan& right() const { return *right_; }
  LogicalPlan& left() { return *left_; }
  LogicalPlan& right() { return *right_; }
  JoinType type() const { return type_; }
  const Expression& condition() const { return *condition_; }

 private:
  LogicalPlanPtr left_;
  LogicalPlanPtr right_;
  JoinType type_;
  ExpressionPtr condition_;
};

struct SortKey {
  ExpressionPtr expr;
  bool descending;
};

class LogicalSort : public LogicalPlan {
 public:
  LogicalSort(LogicalPlanPtr input, std::vector<SortKey> keys)
      : LogicalPlan(input->output_schema()), input_(std::move(input)), keys_(std::move(keys)) {}

  LogicalNodeType node_type() const override { return LogicalNodeType::kSort; }
  std::string describe() const override {
    std::string s = "Sort(";
    for (std::size_t i = 0; i < keys_.size(); ++i) {
      if (i) s += ", ";
      s += keys_[i].expr->to_string() + (keys_[i].descending ? " DESC" : " ASC");
    }
    return s + ")";
  }

  const LogicalPlan& input() const { return *input_; }
  LogicalPlan& input() { return *input_; }
  const std::vector<SortKey>& keys() const { return keys_; }

 private:
  LogicalPlanPtr input_;
  std::vector<SortKey> keys_;
};

class LogicalLimit : public LogicalPlan {
 public:
  LogicalLimit(LogicalPlanPtr input, std::uint64_t limit)
      : LogicalPlan(input->output_schema()), input_(std::move(input)), limit_(limit) {}

  LogicalNodeType node_type() const override { return LogicalNodeType::kLimit; }
  std::string describe() const override { return "Limit(" + std::to_string(limit_) + ")"; }

  const LogicalPlan& input() const { return *input_; }
  LogicalPlan& input() { return *input_; }
  std::uint64_t limit() const { return limit_; }

 private:
  LogicalPlanPtr input_;
  std::uint64_t limit_;
};

// Resolves an AstQuery against `catalog`: checks table/column names exist,
// checks join/aggregate/order-by shapes make sense, assigns ExecType to
// every expression, and produces the fully typed logical plan tree above.
// Throws QueryError (same shape as lexer/parser errors) with the offending
// token's position on any unresolved name or type mismatch.
LogicalPlanPtr bind_query(const AstQuery& query, const Catalog& catalog);

std::string explain(const LogicalPlan& plan, int indent = 0);

}  // namespace columnar
