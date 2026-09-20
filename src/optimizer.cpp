#include "columnar/optimizer.hpp"

#include <algorithm>

namespace columnar {

namespace {

void collect_column_refs(const Expression& e, std::vector<std::size_t>& out) {
  if (const auto* c = dynamic_cast<const ColumnRefExpr*>(&e)) {
    out.push_back(c->column_index());
  } else if (const auto* b = dynamic_cast<const BinaryExpr*>(&e)) {
    collect_column_refs(b->lhs(), out);
    collect_column_refs(b->rhs(), out);
  } else if (const auto* u = dynamic_cast<const UnaryExpr*>(&e)) {
    collect_column_refs(u->operand(), out);
  }
  // LiteralExpr: nothing to collect.
}

void collect_and_conjuncts(const Expression& e, std::vector<const Expression*>& out) {
  const auto* b = dynamic_cast<const BinaryExpr*>(&e);
  if (b && b->op() == BinaryOp::kAnd) {
    collect_and_conjuncts(b->lhs(), out);
    collect_and_conjuncts(b->rhs(), out);
  } else {
    out.push_back(&e);
  }
}

BinaryOp flip(BinaryOp op) {
  switch (op) {
    case BinaryOp::kLt: return BinaryOp::kGt;
    case BinaryOp::kLe: return BinaryOp::kGe;
    case BinaryOp::kGt: return BinaryOp::kLt;
    case BinaryOp::kGe: return BinaryOp::kLe;
    default: return op;  // Eq/Ne are symmetric
  }
}

// Recognizes `column OP literal` or `literal OP column`; anything else
// (both sides columns, both sides literals, nested expressions) isn't
// prunable by a zone map and is left for the Filter operator to evaluate
// normally -- this only ever narrows what gets scanned, never what's
// correct.
bool try_extract_scan_predicate(const Expression& e, ScanPredicate& out) {
  const auto* bin = dynamic_cast<const BinaryExpr*>(&e);
  if (!bin) return false;
  switch (bin->op()) {
    case BinaryOp::kEq:
    case BinaryOp::kNe:
    case BinaryOp::kLt:
    case BinaryOp::kLe:
    case BinaryOp::kGt:
    case BinaryOp::kGe: break;
    default: return false;
  }

  const ColumnRefExpr* col = dynamic_cast<const ColumnRefExpr*>(&bin->lhs());
  const LiteralExpr* lit = dynamic_cast<const LiteralExpr*>(&bin->rhs());
  BinaryOp op = bin->op();
  if (!col || !lit) {
    col = dynamic_cast<const ColumnRefExpr*>(&bin->rhs());
    lit = dynamic_cast<const LiteralExpr*>(&bin->lhs());
    if (!col || !lit) return false;
    op = flip(op);
  }

  // Store the predicate typed as the *column's* type, not the literal's:
  // `amount > 100` parses `100` as an int64 literal even though `amount` is
  // a double column, and evaluate() promotes int->double at comparison
  // time. If this stored the literal's own (int64) type, zone_map_may_match
  // would see a type mismatch against the column's double zone map and
  // conservatively refuse to prune -- silently disabling pruning for the
  // single most natural way to write this predicate.
  out.column_index = col->column_index();
  out.op = op;
  out.type = col->type();
  switch (col->type()) {
    case ExecType::kInt64:
      out.int_value = lit->type() == ExecType::kDouble
                           ? static_cast<std::int64_t>(lit->double_value())
                           : lit->int_value();
      break;
    case ExecType::kDouble:
      out.double_value = lit->type() == ExecType::kInt64 ? static_cast<double>(lit->int_value())
                                                           : lit->double_value();
      break;
    case ExecType::kText: out.text_value = lit->text_value(); break;
    case ExecType::kBool: out.int_value = lit->int_value(); break;
  }
  return true;
}

}  // namespace

LogicalPlanPtr optimize(LogicalPlanPtr plan) {
  LogicalPlan* cursor = plan.get();
  while (cursor->node_type() == LogicalNodeType::kLimit) {
    cursor = &static_cast<LogicalLimit*>(cursor)->input();
  }
  while (cursor->node_type() == LogicalNodeType::kSort) {
    cursor = &static_cast<LogicalSort*>(cursor)->input();
  }

  const bool has_explicit_projection = cursor->node_type() == LogicalNodeType::kProject;
  std::vector<std::size_t> referenced;
  LogicalPlan* below_shape = cursor;

  if (has_explicit_projection) {
    auto& proj = static_cast<LogicalProject&>(*cursor);
    if (proj.input().node_type() == LogicalNodeType::kAggregate) {
      auto& agg = static_cast<LogicalAggregate&>(proj.input());
      for (const ExpressionPtr& g : agg.group_by()) collect_column_refs(*g, referenced);
      for (const AggregateItem& a : agg.aggregates()) {
        if (a.arg) collect_column_refs(*a.arg, referenced);
      }
      below_shape = &agg.input();
    } else {
      for (const ExpressionPtr& e : proj.exprs()) collect_column_refs(*e, referenced);
      below_shape = &proj.input();
    }
  }

  LogicalScan* scan = nullptr;
  if (below_shape->node_type() == LogicalNodeType::kFilter) {
    auto& filter = static_cast<LogicalFilter&>(*below_shape);
    collect_column_refs(filter.predicate(), referenced);
    if (filter.input().node_type() == LogicalNodeType::kScan) {
      scan = &static_cast<LogicalScan&>(filter.input());
      std::vector<const Expression*> conjuncts;
      collect_and_conjuncts(filter.predicate(), conjuncts);
      std::vector<ScanPredicate> preds;
      for (const Expression* c : conjuncts) {
        ScanPredicate p;
        if (try_extract_scan_predicate(*c, p)) preds.push_back(p);
      }
      scan->zone_map_predicates = std::move(preds);
    }
  } else if (below_shape->node_type() == LogicalNodeType::kScan) {
    scan = &static_cast<LogicalScan&>(*below_shape);
  }
  // A Join here means this is a multi-table query: predicate/projection
  // pushdown through a join is not implemented (see LEARNING.md) -- the
  // query still runs correctly, it just scans every column of both sides.

  if (scan && has_explicit_projection) {
    std::sort(referenced.begin(), referenced.end());
    referenced.erase(std::unique(referenced.begin(), referenced.end()), referenced.end());
    scan->projected_column_indices = std::move(referenced);
  }

  return plan;
}

namespace {

double table_row_count(const Database& db, const std::string& table_name) {
  auto it = db.find(table_name);
  if (it == db.end() || it->second.columns.empty()) return 0.0;
  double n = 0.0;
  for (const ColumnChunkVariant& chunk : it->second.columns[0].chunks) n += static_cast<double>(row_count(chunk));
  return n;
}

// A dictionary column's real dictionary_size() gives a much better
// equality-selectivity estimate than a flat guess -- this is the one place
// the estimator looks at actual data instead of a fixed heuristic.
double dictionary_cardinality(const Database& db, const std::string& table_name,
                               std::size_t column_index) {
  auto it = db.find(table_name);
  if (it == db.end() || column_index >= it->second.columns.size()) return 0.0;
  const auto& chunks = it->second.columns[column_index].chunks;
  if (chunks.empty()) return 0.0;
  return std::visit(
      [](const auto& c) -> double {
        using ChunkT = std::decay_t<decltype(c)>;
        if constexpr (std::is_same_v<ChunkT, DictionaryColumnChunk> ||
                      std::is_same_v<ChunkT, DictionaryRleColumnChunk>) {
          return static_cast<double>(c.dictionary_size());
        } else {
          return 0.0;
        }
      },
      chunks[0]);
}

double estimate_selectivity(const Expression& e, const Database& db, const std::string* table_name) {
  if (const auto* b = dynamic_cast<const BinaryExpr*>(&e)) {
    switch (b->op()) {
      case BinaryOp::kAnd:
        return estimate_selectivity(b->lhs(), db, table_name) *
               estimate_selectivity(b->rhs(), db, table_name);
      case BinaryOp::kOr:
        return std::min(1.0, estimate_selectivity(b->lhs(), db, table_name) +
                                  estimate_selectivity(b->rhs(), db, table_name));
      case BinaryOp::kEq: {
        if (table_name) {
          if (const auto* col = dynamic_cast<const ColumnRefExpr*>(&b->lhs())) {
            const double card = dictionary_cardinality(db, *table_name, col->column_index());
            if (card > 0) return 1.0 / card;
          }
        }
        return 0.1;
      }
      case BinaryOp::kNe: return 0.9;
      case BinaryOp::kLt:
      case BinaryOp::kLe:
      case BinaryOp::kGt:
      case BinaryOp::kGe: return 0.33;
      default: return 0.5;
    }
  }
  if (const auto* u = dynamic_cast<const UnaryExpr*>(&e)) {
    if (u->op() == UnaryOp::kNot) return 1.0 - estimate_selectivity(u->operand(), db, table_name);
  }
  return 0.5;
}

}  // namespace

double estimate_row_count(const LogicalPlan& plan, const Database& db) {
  switch (plan.node_type()) {
    case LogicalNodeType::kScan:
      return table_row_count(db, static_cast<const LogicalScan&>(plan).table_name());
    case LogicalNodeType::kFilter: {
      const auto& f = static_cast<const LogicalFilter&>(plan);
      const std::string* table_name = f.input().node_type() == LogicalNodeType::kScan
                                           ? &static_cast<const LogicalScan&>(f.input()).table_name()
                                           : nullptr;
      return estimate_row_count(f.input(), db) * estimate_selectivity(f.predicate(), db, table_name);
    }
    case LogicalNodeType::kProject:
      return estimate_row_count(static_cast<const LogicalProject&>(plan).input(), db);
    case LogicalNodeType::kAggregate:
      // Heuristic: grouping collapses rows; without real distinct-group
      // stats, assume aggregation reduces volume by an order of magnitude.
      return std::max(1.0, estimate_row_count(static_cast<const LogicalAggregate&>(plan).input(), db) * 0.1);
    case LogicalNodeType::kJoin: {
      const auto& j = static_cast<const LogicalJoin&>(plan);
      const double l = estimate_row_count(j.left(), db);
      const double r = estimate_row_count(j.right(), db);
      return j.type() == JoinType::kInner ? std::min(l, r) : l;
    }
    case LogicalNodeType::kSort:
      return estimate_row_count(static_cast<const LogicalSort&>(plan).input(), db);
    case LogicalNodeType::kLimit: {
      const auto& lim = static_cast<const LogicalLimit&>(plan);
      return std::min(estimate_row_count(lim.input(), db), static_cast<double>(lim.limit()));
    }
  }
  return 0.0;
}

}  // namespace columnar
