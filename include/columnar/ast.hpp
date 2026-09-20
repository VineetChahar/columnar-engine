#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "columnar/expression.hpp"
#include "columnar/lexer.hpp"

namespace columnar {

// Raw, unresolved syntax -- what the parser produces before any column or
// table name has been checked against the Catalog. Kept as a separate tree
// from the typed Expression/LogicalPlan (used post-binding) so parsing
// (pure grammar) and binding (name/type resolution) are two independently
// testable stages, matching how hand-written SQL front ends are usually
// split in practice.
enum class AstExprKind { kIntLit, kFloatLit, kStringLit, kBoolLit, kColumnRef, kStar, kBinary, kUnary, kAggCall };
enum class AggFunc { kCount, kCountStar, kSum, kAvg, kMin, kMax };

struct AstExpr;
using AstExprPtr = std::unique_ptr<AstExpr>;

struct AstExpr {
  AstExprKind kind;
  Token position;  // for error messages

  std::int64_t int_value = 0;
  double double_value = 0.0;
  std::string string_value;
  bool bool_value = false;

  std::string table_qualifier;  // empty if unqualified
  std::string column_name;

  BinaryOp bin_op{};
  UnaryOp un_op{};
  AstExprPtr lhs;
  AstExprPtr rhs;
  AstExprPtr operand;

  AggFunc agg_func{};
  AstExprPtr agg_arg;  // nullptr iff kind == kAggCall && agg_func == kCountStar
};

struct AstSelectItem {
  AstExprPtr expr;
  std::string alias;  // empty if none given
};

enum class JoinType { kInner, kLeft };

struct AstJoin {
  JoinType type;
  std::string table;
  std::string alias;  // empty if none given; qualifier defaults to `table`
  AstExprPtr on;
  Token position;
};

struct AstOrderItem {
  AstExprPtr expr;
  bool descending = false;
};

struct AstQuery {
  bool select_star = false;
  std::vector<AstSelectItem> select_list;  // empty iff select_star
  std::string from_table;
  std::string from_alias;  // empty if none given; qualifier defaults to from_table
  Token from_position;
  std::vector<AstJoin> joins;
  AstExprPtr where;  // nullable
  std::vector<AstExprPtr> group_by;
  std::vector<AstOrderItem> order_by;
  std::optional<std::uint64_t> limit;
};

}  // namespace columnar
