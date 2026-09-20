#include "columnar/expression.hpp"

#include <algorithm>
#include <stdexcept>

namespace columnar {

namespace {

std::size_t result_size(const ExecBatch& batch, const std::vector<std::uint32_t>* selection) {
  return selection ? selection->size() : batch.row_count();
}

template <typename T>
std::vector<T> gather(const std::vector<T>& src, const std::vector<std::uint32_t>* selection) {
  if (!selection) return src;
  std::vector<T> out;
  out.reserve(selection->size());
  for (std::uint32_t idx : *selection) out.push_back(src[idx]);
  return out;
}

std::vector<std::uint8_t> gather_validity(const std::vector<std::uint8_t>& src,
                                           const std::vector<std::uint32_t>* selection) {
  return gather(src, selection);
}

}  // namespace

// -------------------------------------------------------------- Literal --

ExpressionPtr LiteralExpr::make_int(std::int64_t v) {
  auto e = std::make_unique<LiteralExpr>();
  e->type_ = ExecType::kInt64;
  e->int_value_ = v;
  return e;
}
ExpressionPtr LiteralExpr::make_double(double v) {
  auto e = std::make_unique<LiteralExpr>();
  e->type_ = ExecType::kDouble;
  e->double_value_ = v;
  return e;
}
ExpressionPtr LiteralExpr::make_bool(bool v) {
  auto e = std::make_unique<LiteralExpr>();
  e->type_ = ExecType::kBool;
  e->int_value_ = v ? 1 : 0;
  return e;
}
ExpressionPtr LiteralExpr::make_text(std::string v) {
  auto e = std::make_unique<LiteralExpr>();
  e->type_ = ExecType::kText;
  e->text_value_ = std::move(v);
  return e;
}

ExecColumn LiteralExpr::evaluate(const ExecBatch& batch,
                                  const std::vector<std::uint32_t>* selection) const {
  const std::size_t n = result_size(batch, selection);
  ExecColumn out = ExecColumn::make(type_, n);
  switch (type_) {
    case ExecType::kInt64:
      std::fill(out.ints().begin(), out.ints().end(), int_value_);
      break;
    case ExecType::kDouble:
      std::fill(out.doubles().begin(), out.doubles().end(), double_value_);
      break;
    case ExecType::kBool:
      std::fill(out.bools().begin(), out.bools().end(), static_cast<std::uint8_t>(int_value_));
      break;
    case ExecType::kText:
      std::fill(out.texts().begin(), out.texts().end(), text_value_);
      break;
  }
  return out;
}

std::string LiteralExpr::to_string() const {
  switch (type_) {
    case ExecType::kInt64: return std::to_string(int_value_);
    case ExecType::kDouble: return std::to_string(double_value_);
    case ExecType::kBool: return int_value_ ? "TRUE" : "FALSE";
    case ExecType::kText: return "'" + text_value_ + "'";
  }
  return "?";
}

// ------------------------------------------------------------- ColumnRef --

ExecColumn ColumnRefExpr::evaluate(const ExecBatch& batch,
                                    const std::vector<std::uint32_t>* selection) const {
  const ExecColumn& src = batch.columns[index_];
  ExecColumn out;
  out.type = src.type;
  out.validity = gather_validity(src.validity, selection);
  switch (src.type) {
    case ExecType::kInt64: out.data = gather(src.ints(), selection); break;
    case ExecType::kDouble: out.data = gather(src.doubles(), selection); break;
    case ExecType::kBool: out.data = gather(src.bools(), selection); break;
    case ExecType::kText: out.data = gather(src.texts(), selection); break;
  }
  return out;
}

// -------------------------------------------------------------- Binary --

namespace {

template <typename T>
ExecColumn numeric_binary(const std::vector<T>& lhs, const std::vector<T>& rhs, BinaryOp op,
                           ExecType result_type) {
  const std::size_t n = lhs.size();
  ExecColumn out = ExecColumn::make(result_type, n);
  for (std::size_t i = 0; i < n; ++i) {
    switch (op) {
      case BinaryOp::kAdd:
        if (result_type == ExecType::kDouble) out.doubles()[i] = static_cast<double>(lhs[i] + rhs[i]);
        else out.ints()[i] = static_cast<std::int64_t>(lhs[i] + rhs[i]);
        break;
      case BinaryOp::kSub:
        if (result_type == ExecType::kDouble) out.doubles()[i] = static_cast<double>(lhs[i] - rhs[i]);
        else out.ints()[i] = static_cast<std::int64_t>(lhs[i] - rhs[i]);
        break;
      case BinaryOp::kMul:
        if (result_type == ExecType::kDouble) out.doubles()[i] = static_cast<double>(lhs[i] * rhs[i]);
        else out.ints()[i] = static_cast<std::int64_t>(lhs[i] * rhs[i]);
        break;
      case BinaryOp::kDiv:
        out.doubles()[i] = static_cast<double>(lhs[i]) / static_cast<double>(rhs[i]);
        break;
      case BinaryOp::kEq: out.bools()[i] = (lhs[i] == rhs[i]); break;
      case BinaryOp::kNe: out.bools()[i] = (lhs[i] != rhs[i]); break;
      case BinaryOp::kLt: out.bools()[i] = (lhs[i] < rhs[i]); break;
      case BinaryOp::kLe: out.bools()[i] = (lhs[i] <= rhs[i]); break;
      case BinaryOp::kGt: out.bools()[i] = (lhs[i] > rhs[i]); break;
      case BinaryOp::kGe: out.bools()[i] = (lhs[i] >= rhs[i]); break;
      default: throw std::logic_error("numeric_binary: unsupported op");
    }
  }
  return out;
}

ExecColumn text_binary(const std::vector<std::string>& lhs, const std::vector<std::string>& rhs,
                        BinaryOp op) {
  const std::size_t n = lhs.size();
  ExecColumn out = ExecColumn::make(ExecType::kBool, n);
  for (std::size_t i = 0; i < n; ++i) {
    switch (op) {
      case BinaryOp::kEq: out.bools()[i] = (lhs[i] == rhs[i]); break;
      case BinaryOp::kNe: out.bools()[i] = (lhs[i] != rhs[i]); break;
      case BinaryOp::kLt: out.bools()[i] = (lhs[i] < rhs[i]); break;
      case BinaryOp::kLe: out.bools()[i] = (lhs[i] <= rhs[i]); break;
      case BinaryOp::kGt: out.bools()[i] = (lhs[i] > rhs[i]); break;
      case BinaryOp::kGe: out.bools()[i] = (lhs[i] >= rhs[i]); break;
      default: throw std::logic_error("text_binary: unsupported op");
    }
  }
  return out;
}

ExecColumn bool_binary(const std::vector<std::uint8_t>& lhs, const std::vector<std::uint8_t>& rhs,
                        BinaryOp op) {
  const std::size_t n = lhs.size();
  ExecColumn out = ExecColumn::make(ExecType::kBool, n);
  for (std::size_t i = 0; i < n; ++i) {
    switch (op) {
      case BinaryOp::kAnd: out.bools()[i] = (lhs[i] && rhs[i]); break;
      case BinaryOp::kOr: out.bools()[i] = (lhs[i] || rhs[i]); break;
      case BinaryOp::kEq: out.bools()[i] = (lhs[i] == rhs[i]); break;
      case BinaryOp::kNe: out.bools()[i] = (lhs[i] != rhs[i]); break;
      default: throw std::logic_error("bool_binary: unsupported op");
    }
  }
  return out;
}

// AND-combine two validity arrays: a row is valid in the result only if
// both operands were valid for it (SQL null-propagation, simplified: no
// three-valued logic short-circuiting, just "null in, null out").
std::vector<std::uint8_t> combine_validity(const std::vector<std::uint8_t>& a,
                                            const std::vector<std::uint8_t>& b) {
  std::vector<std::uint8_t> out(a.size());
  for (std::size_t i = 0; i < a.size(); ++i) out[i] = a[i] && b[i];
  return out;
}

}  // namespace

ExecColumn BinaryExpr::evaluate(const ExecBatch& batch,
                                 const std::vector<std::uint32_t>* selection) const {
  ExecColumn lhs = lhs_->evaluate(batch, selection);
  ExecColumn rhs = rhs_->evaluate(batch, selection);

  ExecColumn out;
  if (lhs.type == ExecType::kText || rhs.type == ExecType::kText) {
    out = text_binary(lhs.texts(), rhs.texts(), op_);
  } else if (lhs.type == ExecType::kBool && rhs.type == ExecType::kBool) {
    out = bool_binary(lhs.bools(), rhs.bools(), op_);
  } else if (lhs.type == ExecType::kDouble || rhs.type == ExecType::kDouble ||
             op_ == BinaryOp::kDiv) {
    // Promote whichever side is int64 up to double for mixed arithmetic.
    std::vector<double> lvals = lhs.type == ExecType::kDouble
                                     ? lhs.doubles()
                                     : std::vector<double>(lhs.ints().begin(), lhs.ints().end());
    std::vector<double> rvals = rhs.type == ExecType::kDouble
                                     ? rhs.doubles()
                                     : std::vector<double>(rhs.ints().begin(), rhs.ints().end());
    const bool is_comparison = op_ != BinaryOp::kAdd && op_ != BinaryOp::kSub &&
                                op_ != BinaryOp::kMul && op_ != BinaryOp::kDiv;
    out = numeric_binary<double>(lvals, rvals, op_,
                                  is_comparison ? ExecType::kBool : ExecType::kDouble);
  } else {
    out = numeric_binary<std::int64_t>(lhs.ints(), rhs.ints(), op_, type_);
  }

  out.validity = combine_validity(lhs.validity, rhs.validity);
  return out;
}

std::string BinaryExpr::to_string() const {
  return "(" + lhs_->to_string() + " " + columnar::to_string(op_) + " " + rhs_->to_string() + ")";
}

// -------------------------------------------------------------- Unary --

ExecColumn UnaryExpr::evaluate(const ExecBatch& batch,
                                const std::vector<std::uint32_t>* selection) const {
  ExecColumn operand = operand_->evaluate(batch, selection);
  ExecColumn out = ExecColumn::make(type_, operand.size());
  out.validity = operand.validity;

  if (op_ == UnaryOp::kNot) {
    for (std::size_t i = 0; i < operand.size(); ++i) {
      out.bools()[i] = !operand.bools()[i];
    }
  } else {  // kNeg
    if (type_ == ExecType::kDouble) {
      for (std::size_t i = 0; i < operand.size(); ++i) out.doubles()[i] = -operand.doubles()[i];
    } else {
      for (std::size_t i = 0; i < operand.size(); ++i) out.ints()[i] = -operand.ints()[i];
    }
  }
  return out;
}

std::string UnaryExpr::to_string() const {
  return std::string(columnar::to_string(op_)) + operand_->to_string();
}

// ------------------------------------------------------------- to_string --

const char* to_string(BinaryOp op) {
  switch (op) {
    case BinaryOp::kAdd: return "+";
    case BinaryOp::kSub: return "-";
    case BinaryOp::kMul: return "*";
    case BinaryOp::kDiv: return "/";
    case BinaryOp::kEq: return "=";
    case BinaryOp::kNe: return "!=";
    case BinaryOp::kLt: return "<";
    case BinaryOp::kLe: return "<=";
    case BinaryOp::kGt: return ">";
    case BinaryOp::kGe: return ">=";
    case BinaryOp::kAnd: return "AND";
    case BinaryOp::kOr: return "OR";
  }
  return "?";
}

const char* to_string(UnaryOp op) {
  switch (op) {
    case UnaryOp::kNeg: return "-";
    case UnaryOp::kNot: return "NOT ";
  }
  return "?";
}

const char* to_string(ExecType t) {
  switch (t) {
    case ExecType::kInt64: return "INT64";
    case ExecType::kDouble: return "DOUBLE";
    case ExecType::kBool: return "BOOL";
    case ExecType::kText: return "TEXT";
  }
  return "?";
}

}  // namespace columnar
