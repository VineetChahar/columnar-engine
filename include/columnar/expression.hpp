#pragma once

#include <memory>
#include <string>
#include <vector>

#include "columnar/exec_batch.hpp"

namespace columnar {

enum class BinaryOp { kAdd, kSub, kMul, kDiv, kEq, kNe, kLt, kLe, kGt, kGe, kAnd, kOr };
enum class UnaryOp { kNeg, kNot };

// A scalar expression tree, evaluated one whole batch at a time (never one
// row at a time) -- evaluate() walks the tree once per call, and each node
// implementation loops over the batch internally. The virtual call count
// per batch is O(number of expression nodes), not O(rows): exactly the
// "amortize the dispatch, not eliminate it" idea from DESIGN.md section 5.
//
// Aggregate calls (COUNT/SUM/AVG/MIN/MAX) are deliberately NOT an
// Expression variant here -- they only ever appear as top-level SELECT-list
// items bound directly to a LogicalAggregate node (see logical_plan.hpp).
// `SUM(a) + SUM(b)` works (both SUMs become aggregate output columns, the
// `+` is an ordinary BinaryExpr over the aggregate's output batch);
// `SUM(a + MAX(b))` does not, matching most hand-written query compilers'
// first cut at this. Documented, not hidden.
class Expression {
 public:
  virtual ~Expression() = default;
  virtual ExecType type() const = 0;
  // selection == nullptr means "every row in batch"; otherwise only the
  // listed row indices are evaluated, and the result is dense over just
  // those rows (result.size() == selection->size()). This is what keeps a
  // chain of filters from ever computing a value for an already-excluded
  // row.
  virtual ExecColumn evaluate(const ExecBatch& batch,
                               const std::vector<std::uint32_t>* selection) const = 0;
  virtual std::string to_string() const = 0;
};

using ExpressionPtr = std::unique_ptr<Expression>;

class LiteralExpr : public Expression {
 public:
  static ExpressionPtr make_int(std::int64_t v);
  static ExpressionPtr make_double(double v);
  static ExpressionPtr make_bool(bool v);
  static ExpressionPtr make_text(std::string v);

  ExecType type() const override { return type_; }
  ExecColumn evaluate(const ExecBatch& batch,
                       const std::vector<std::uint32_t>* selection) const override;
  std::string to_string() const override;

  std::int64_t int_value() const { return int_value_; }
  double double_value() const { return double_value_; }
  const std::string& text_value() const { return text_value_; }

 private:
  ExecType type_;
  std::int64_t int_value_ = 0;
  double double_value_ = 0.0;
  std::string text_value_;
};

class ColumnRefExpr : public Expression {
 public:
  ColumnRefExpr(std::string name, std::size_t index, ExecType type)
      : name_(std::move(name)), index_(index), type_(type) {}

  ExecType type() const override { return type_; }
  ExecColumn evaluate(const ExecBatch& batch,
                       const std::vector<std::uint32_t>* selection) const override;
  std::string to_string() const override { return name_; }

  std::size_t column_index() const { return index_; }
  const std::string& name() const { return name_; }

 private:
  std::string name_;
  std::size_t index_;
  ExecType type_;
};

class BinaryExpr : public Expression {
 public:
  BinaryExpr(BinaryOp op, ExpressionPtr lhs, ExpressionPtr rhs, ExecType result_type)
      : op_(op), lhs_(std::move(lhs)), rhs_(std::move(rhs)), type_(result_type) {}

  ExecType type() const override { return type_; }
  ExecColumn evaluate(const ExecBatch& batch,
                       const std::vector<std::uint32_t>* selection) const override;
  std::string to_string() const override;

  BinaryOp op() const { return op_; }
  const Expression& lhs() const { return *lhs_; }
  const Expression& rhs() const { return *rhs_; }

 private:
  BinaryOp op_;
  ExpressionPtr lhs_;
  ExpressionPtr rhs_;
  ExecType type_;
};

class UnaryExpr : public Expression {
 public:
  UnaryExpr(UnaryOp op, ExpressionPtr operand, ExecType result_type)
      : op_(op), operand_(std::move(operand)), type_(result_type) {}

  ExecType type() const override { return type_; }
  ExecColumn evaluate(const ExecBatch& batch,
                       const std::vector<std::uint32_t>* selection) const override;
  std::string to_string() const override;

  UnaryOp op() const { return op_; }
  const Expression& operand() const { return *operand_; }

 private:
  UnaryOp op_;
  ExpressionPtr operand_;
  ExecType type_;
};

const char* to_string(BinaryOp op);
const char* to_string(UnaryOp op);
const char* to_string(ExecType t);

}  // namespace columnar
