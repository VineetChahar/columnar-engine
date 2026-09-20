#include "columnar/logical_plan.hpp"

#include <algorithm>
#include <sstream>

namespace columnar {

namespace {

struct ScopeColumn {
  std::string qualifier;  // table name this column came from
  std::string name;
  ExecType type;
  std::size_t index;  // position in the current plan node's output_schema()
};
using Scope = std::vector<ScopeColumn>;

ExecType widen(TypeId t) {
  switch (t) {
    case TypeId::kInt32:
    case TypeId::kInt64: return ExecType::kInt64;
    case TypeId::kDouble: return ExecType::kDouble;
    case TypeId::kBool: return ExecType::kBool;
    case TypeId::kVarchar: return ExecType::kText;
  }
  return ExecType::kInt64;
}

bool is_numeric(ExecType t) { return t == ExecType::kInt64 || t == ExecType::kDouble; }

[[noreturn]] void fail_at(const Token& pos, std::string message) {
  throw QueryError{std::move(message), pos.line, pos.col};
}

Scope scope_from_schema(const std::string& qualifier, const std::vector<OutputColumn>& schema) {
  Scope s;
  s.reserve(schema.size());
  for (std::size_t i = 0; i < schema.size(); ++i) {
    s.push_back(ScopeColumn{qualifier, schema[i].name, schema[i].type, i});
  }
  return s;
}

Scope concat_scope(const Scope& left, const Scope& right, std::size_t left_width) {
  Scope out = left;
  for (const auto& col : right) {
    out.push_back(ScopeColumn{col.qualifier, col.name, col.type, col.index + left_width});
  }
  return out;
}

class Binder {
 public:
  explicit Binder(const Catalog& catalog) : catalog_(catalog) {}

  LogicalPlanPtr bind(const AstQuery& query) {
    auto [plan, scope] = bind_from_and_joins(query);

    if (query.where) {
      ExpressionPtr predicate = bind_expr(*query.where, scope, /*allow_agg=*/false);
      if (predicate->type() != ExecType::kBool) {
        fail_at(query.where->position, "WHERE clause must be a boolean expression");
      }
      plan = std::make_unique<LogicalFilter>(std::move(plan), std::move(predicate));
    }

    const bool has_group_by = !query.group_by.empty();
    const bool select_has_agg =
        !query.select_star &&
        std::any_of(query.select_list.begin(), query.select_list.end(),
                    [](const AstSelectItem& item) { return contains_agg_call(*item.expr); });

    if (has_group_by || select_has_agg) {
      if (query.select_star) {
        fail_at(query.from_position, "SELECT * cannot be combined with GROUP BY or aggregates");
      }
      plan = bind_aggregate(query, std::move(plan), scope);
    } else if (!query.select_star) {
      plan = bind_project(query, std::move(plan), scope);
    }
    // select_star with no aggregation: pass the source plan through as-is.

    if (!query.order_by.empty()) {
      Scope post_select = scope_from_schema("", plan->output_schema());
      std::vector<SortKey> keys;
      keys.reserve(query.order_by.size());
      for (const AstOrderItem& item : query.order_by) {
        ExpressionPtr e = bind_expr(*item.expr, post_select, /*allow_agg=*/false);
        keys.push_back(SortKey{std::move(e), item.descending});
      }
      plan = std::make_unique<LogicalSort>(std::move(plan), std::move(keys));
    }

    if (query.limit) {
      plan = std::make_unique<LogicalLimit>(std::move(plan), *query.limit);
    }

    // `plan` is a structured-binding name (from `auto [plan, scope] = ...`
    // above), and structured bindings are never treated as a "named
    // automatic object" for the implicit-move-on-return rule -- unlike an
    // ordinary local variable, returning one by value requires an explicit
    // std::move or it copies (and here, fails to compile at all, since
    // LogicalPlanPtr is move-only).
    return std::move(plan);
  }

 private:
  const Catalog& catalog_;

  static bool contains_agg_call(const AstExpr& e) {
    if (e.kind == AstExprKind::kAggCall) return true;
    if (e.lhs && contains_agg_call(*e.lhs)) return true;
    if (e.rhs && contains_agg_call(*e.rhs)) return true;
    if (e.operand && contains_agg_call(*e.operand)) return true;
    return false;
  }

  const CatalogTable& require_table(const std::string& name, const Token& pos) {
    const CatalogTable* t = catalog_.find_table(name);
    if (!t) fail_at(pos, "no such table: " + name);
    return *t;
  }

  std::pair<LogicalPlanPtr, Scope> bind_from_and_joins(const AstQuery& query) {
    const CatalogTable& table = require_table(query.from_table, query.from_position);
    std::vector<OutputColumn> schema;
    schema.reserve(table.columns.size());
    for (const CatalogColumn& c : table.columns) schema.push_back({c.name, widen(c.type)});

    LogicalPlanPtr plan = std::make_unique<LogicalScan>(query.from_table, schema);
    const std::string& from_qualifier =
        query.from_alias.empty() ? query.from_table : query.from_alias;
    Scope scope = scope_from_schema(from_qualifier, plan->output_schema());

    for (const AstJoin& join : query.joins) {
      const CatalogTable& right_table = require_table(join.table, join.position);
      std::vector<OutputColumn> right_schema;
      right_schema.reserve(right_table.columns.size());
      for (const CatalogColumn& c : right_table.columns) {
        right_schema.push_back({c.name, widen(c.type)});
      }
      LogicalPlanPtr right_scan = std::make_unique<LogicalScan>(join.table, right_schema);
      const std::string& right_qualifier = join.alias.empty() ? join.table : join.alias;
      Scope right_scope = scope_from_schema(right_qualifier, right_scan->output_schema());

      const std::size_t left_width = plan->output_schema().size();
      Scope combined = concat_scope(scope, right_scope, left_width);

      ExpressionPtr condition = bind_expr(*join.on, combined, /*allow_agg=*/false);
      if (condition->type() != ExecType::kBool) {
        fail_at(join.position, "JOIN ... ON condition must be a boolean expression");
      }

      std::vector<OutputColumn> joined_schema = plan->output_schema();
      const auto& rs = right_scan->output_schema();
      joined_schema.insert(joined_schema.end(), rs.begin(), rs.end());

      plan = std::make_unique<LogicalJoin>(std::move(plan), std::move(right_scan), join.type,
                                            std::move(condition), std::move(joined_schema));
      scope = std::move(combined);
    }

    return {std::move(plan), std::move(scope)};
  }

  LogicalPlanPtr bind_project(const AstQuery& query, LogicalPlanPtr input, const Scope& scope) {
    std::vector<ExpressionPtr> exprs;
    std::vector<OutputColumn> schema;
    exprs.reserve(query.select_list.size());
    schema.reserve(query.select_list.size());
    for (const AstSelectItem& item : query.select_list) {
      if (contains_agg_call(*item.expr)) {
        fail_at(item.expr->position, "aggregate function used without GROUP BY");
      }
      ExpressionPtr e = bind_expr(*item.expr, scope, /*allow_agg=*/false);
      std::string name = !item.alias.empty()
                              ? item.alias
                              : (item.expr->kind == AstExprKind::kColumnRef
                                     ? item.expr->column_name
                                     : e->to_string());
      schema.push_back({name, e->type()});
      exprs.push_back(std::move(e));
    }
    return std::make_unique<LogicalProject>(std::move(input), std::move(exprs),
                                             std::move(schema));
  }

  // HashAggregateOperator (Phase 3) always emits columns in a fixed
  // canonical order: every GROUP BY key, then every aggregate, regardless
  // of how the user ordered/subset them in the SELECT list. So this binds
  // the Aggregate node with that canonical schema, then wraps it in a
  // trivial LogicalProject that reorders/renames/subsets into exactly what
  // the user wrote -- the same separation of concerns real planners use
  // (an aggregate operator's physical output order and a query's requested
  // output order are different things).
  LogicalPlanPtr bind_aggregate(const AstQuery& query, LogicalPlanPtr input,
                                 const Scope& scope) {
    std::vector<ExpressionPtr> group_by;
    std::vector<std::string> group_by_text;
    std::vector<OutputColumn> canonical_schema;
    for (const AstExprPtr& g : query.group_by) {
      ExpressionPtr e = bind_expr(*g, scope, /*allow_agg=*/false);
      group_by_text.push_back(e->to_string());
      canonical_schema.push_back({e->to_string(), e->type()});
      group_by.push_back(std::move(e));
    }
    const std::size_t num_group_by = group_by.size();

    std::vector<AggregateItem> aggregates;
    std::vector<ExpressionPtr> final_exprs;
    std::vector<OutputColumn> final_schema;

    for (const AstSelectItem& item : query.select_list) {
      if (item.expr->kind == AstExprKind::kAggCall) {
        AggregateItem agg;
        agg.func = item.expr->agg_func;
        if (item.expr->agg_arg) {
          if (contains_agg_call(*item.expr->agg_arg)) {
            fail_at(item.expr->position, "nested aggregate functions are not supported");
          }
          agg.arg = bind_expr(*item.expr->agg_arg, scope, /*allow_agg=*/false);
          if ((item.expr->agg_func == AggFunc::kSum || item.expr->agg_func == AggFunc::kAvg) &&
              !is_numeric(agg.arg->type())) {
            fail_at(item.expr->position, "SUM/AVG require a numeric argument");
          }
        }
        const ExecType result_type = aggregate_result_type(agg);
        const std::size_t canonical_index = num_group_by + aggregates.size();
        agg.output_name = describe_agg(*item.expr);
        canonical_schema.push_back({agg.output_name, result_type});

        final_exprs.push_back(
            std::make_unique<ColumnRefExpr>(agg.output_name, canonical_index, result_type));
        final_schema.push_back(
            {!item.alias.empty() ? item.alias : agg.output_name, result_type});
        aggregates.push_back(std::move(agg));
      } else {
        if (contains_agg_call(*item.expr)) {
          fail_at(item.expr->position,
                   "mixing aggregates with other expressions in one SELECT item is not "
                   "supported -- select the aggregate and the group-by column separately");
        }
        ExpressionPtr e = bind_expr(*item.expr, scope, /*allow_agg=*/false);
        const std::string text = e->to_string();
        auto it = std::find(group_by_text.begin(), group_by_text.end(), text);
        if (it == group_by_text.end()) {
          fail_at(item.expr->position,
                   "column '" + text + "' must appear in GROUP BY or be used in an aggregate");
        }
        const std::size_t gi = static_cast<std::size_t>(it - group_by_text.begin());
        std::string name = !item.alias.empty()
                                ? item.alias
                                : (item.expr->kind == AstExprKind::kColumnRef
                                       ? item.expr->column_name
                                       : text);
        final_exprs.push_back(
            std::make_unique<ColumnRefExpr>(group_by_text[gi], gi, group_by[gi]->type()));
        final_schema.push_back({name, group_by[gi]->type()});
      }
    }

    LogicalPlanPtr agg_node = std::make_unique<LogicalAggregate>(
        std::move(input), std::move(group_by), std::move(aggregates), std::move(canonical_schema));
    return std::make_unique<LogicalProject>(std::move(agg_node), std::move(final_exprs),
                                             std::move(final_schema));
  }

  static std::string describe_agg(const AstExpr& e) {
    static const char* names[] = {"COUNT", "COUNT", "SUM", "AVG", "MIN", "MAX"};
    std::string fn = names[static_cast<int>(e.agg_func)];
    if (e.agg_func == AggFunc::kCountStar) return "COUNT(*)";
    return fn + "(" + (e.agg_arg ? e.agg_arg->column_name : std::string()) + ")";
  }

  static ExecType aggregate_result_type(const AggregateItem& agg) {
    switch (agg.func) {
      case AggFunc::kCount:
      case AggFunc::kCountStar: return ExecType::kInt64;
      case AggFunc::kAvg: return ExecType::kDouble;
      case AggFunc::kSum:
      case AggFunc::kMin:
      case AggFunc::kMax: return agg.arg ? agg.arg->type() : ExecType::kInt64;
    }
    return ExecType::kInt64;
  }

  ExpressionPtr bind_expr(const AstExpr& e, const Scope& scope, bool allow_agg) {
    switch (e.kind) {
      case AstExprKind::kIntLit: return LiteralExpr::make_int(e.int_value);
      case AstExprKind::kFloatLit: return LiteralExpr::make_double(e.double_value);
      case AstExprKind::kStringLit: return LiteralExpr::make_text(e.string_value);
      case AstExprKind::kBoolLit: return LiteralExpr::make_bool(e.bool_value);
      case AstExprKind::kStar:
        fail_at(e.position, "'*' is not allowed here");
      case AstExprKind::kAggCall:
        if (!allow_agg) {
          fail_at(e.position, "aggregate functions are not allowed here");
        }
        fail_at(e.position, "internal: aggregate call reached generic binder");
      case AstExprKind::kColumnRef: return bind_column_ref(e, scope);
      case AstExprKind::kUnary: return bind_unary(e, scope);
      case AstExprKind::kBinary: return bind_binary(e, scope);
    }
    fail_at(e.position, "internal: unhandled expression kind");
  }

  ExpressionPtr bind_column_ref(const AstExpr& e, const Scope& scope) {
    const ScopeColumn* found = nullptr;
    for (const ScopeColumn& col : scope) {
      // Qualifier mismatch only disqualifies a candidate when the scope
      // column actually HAS a qualifier to disagree with. Post-SELECT
      // scopes (built for ORDER BY, see bind()) carry no table qualifier
      // at all -- their columns came from arbitrary expressions, not one
      // table -- so `ORDER BY o.id` must still be able to match an output
      // column literally named `id` even though `o` no longer means
      // anything at that point in the plan.
      if (!e.table_qualifier.empty() && !col.qualifier.empty() &&
          col.qualifier != e.table_qualifier) {
        continue;
      }
      if (col.name != e.column_name) continue;
      if (found != nullptr) {
        fail_at(e.position, "ambiguous column reference: " + e.column_name);
      }
      found = &col;
    }
    if (!found) {
      std::string qualified =
          e.table_qualifier.empty() ? e.column_name : e.table_qualifier + "." + e.column_name;
      fail_at(e.position, "no such column: " + qualified);
    }
    return std::make_unique<ColumnRefExpr>(found->name, found->index, found->type);
  }

  ExpressionPtr bind_unary(const AstExpr& e, const Scope& scope) {
    ExpressionPtr operand = bind_expr(*e.operand, scope, /*allow_agg=*/false);
    if (e.un_op == UnaryOp::kNot) {
      if (operand->type() != ExecType::kBool) {
        fail_at(e.position, "NOT requires a boolean operand");
      }
      return std::make_unique<UnaryExpr>(UnaryOp::kNot, std::move(operand), ExecType::kBool);
    }
    if (!is_numeric(operand->type())) {
      fail_at(e.position, "unary '-' requires a numeric operand");
    }
    ExecType t = operand->type();
    return std::make_unique<UnaryExpr>(UnaryOp::kNeg, std::move(operand), t);
  }

  ExpressionPtr bind_binary(const AstExpr& e, const Scope& scope) {
    ExpressionPtr lhs = bind_expr(*e.lhs, scope, /*allow_agg=*/false);
    ExpressionPtr rhs = bind_expr(*e.rhs, scope, /*allow_agg=*/false);
    const ExecType lt = lhs->type();
    const ExecType rt = rhs->type();

    switch (e.bin_op) {
      case BinaryOp::kAnd:
      case BinaryOp::kOr:
        if (lt != ExecType::kBool || rt != ExecType::kBool) {
          fail_at(e.position, std::string(columnar::to_string(e.bin_op)) +
                                   " requires boolean operands");
        }
        return std::make_unique<BinaryExpr>(e.bin_op, std::move(lhs), std::move(rhs),
                                             ExecType::kBool);
      case BinaryOp::kEq:
      case BinaryOp::kNe:
      case BinaryOp::kLt:
      case BinaryOp::kLe:
      case BinaryOp::kGt:
      case BinaryOp::kGe:
        if (is_numeric(lt) && is_numeric(rt)) {
          // ok, mixed int/double allowed
        } else if (lt != rt) {
          fail_at(e.position, "cannot compare " + std::string(columnar::to_string(lt)) + " to " +
                                   columnar::to_string(rt));
        }
        return std::make_unique<BinaryExpr>(e.bin_op, std::move(lhs), std::move(rhs),
                                             ExecType::kBool);
      case BinaryOp::kAdd:
      case BinaryOp::kSub:
      case BinaryOp::kMul:
      case BinaryOp::kDiv:
        if (!is_numeric(lt) || !is_numeric(rt)) {
          fail_at(e.position, "arithmetic requires numeric operands, got " +
                                   std::string(columnar::to_string(lt)) + " and " +
                                   columnar::to_string(rt));
        }
        {
          const ExecType result =
              (e.bin_op == BinaryOp::kDiv || lt == ExecType::kDouble || rt == ExecType::kDouble)
                  ? ExecType::kDouble
                  : ExecType::kInt64;
          return std::make_unique<BinaryExpr>(e.bin_op, std::move(lhs), std::move(rhs), result);
        }
    }
    fail_at(e.position, "internal: unhandled binary operator");
  }
};

}  // namespace

std::string LogicalScan::describe() const {
  std::string s = "Scan(" + table_name_;
  if (!zone_map_predicates.empty()) {
    s += ", pruning=[";
    for (std::size_t i = 0; i < zone_map_predicates.size(); ++i) {
      const ScanPredicate& p = zone_map_predicates[i];
      if (i) s += " AND ";
      s += output_schema_[p.column_index].name;
      s += std::string(" ") + columnar::to_string(p.op) + " ";
      switch (p.type) {
        case ExecType::kInt64: s += std::to_string(p.int_value); break;
        case ExecType::kDouble: s += std::to_string(p.double_value); break;
        case ExecType::kText: s += "'" + p.text_value + "'"; break;
        case ExecType::kBool: break;
      }
    }
    s += "]";
  }
  if (!projected_column_indices.empty()) {
    s += ", projects=" + std::to_string(projected_column_indices.size()) + "/" +
         std::to_string(output_schema_.size()) + " cols";
  }
  return s + ")";
}

LogicalPlanPtr bind_query(const AstQuery& query, const Catalog& catalog) {
  return Binder(catalog).bind(query);
}

std::string explain(const LogicalPlan& plan, int indent) {
  std::ostringstream out;
  out << std::string(static_cast<std::size_t>(indent) * 2, ' ') << plan.describe() << "\n";

  switch (plan.node_type()) {
    case LogicalNodeType::kScan:
      break;
    case LogicalNodeType::kFilter:
      out << explain(static_cast<const LogicalFilter&>(plan).input(), indent + 1);
      break;
    case LogicalNodeType::kProject:
      out << explain(static_cast<const LogicalProject&>(plan).input(), indent + 1);
      break;
    case LogicalNodeType::kAggregate:
      out << explain(static_cast<const LogicalAggregate&>(plan).input(), indent + 1);
      break;
    case LogicalNodeType::kJoin: {
      const auto& join = static_cast<const LogicalJoin&>(plan);
      out << explain(join.left(), indent + 1);
      out << explain(join.right(), indent + 1);
      break;
    }
    case LogicalNodeType::kSort:
      out << explain(static_cast<const LogicalSort&>(plan).input(), indent + 1);
      break;
    case LogicalNodeType::kLimit:
      out << explain(static_cast<const LogicalLimit&>(plan).input(), indent + 1);
      break;
  }
  return out.str();
}

}  // namespace columnar
