#include "columnar/parser.hpp"

#include "columnar/lexer.hpp"

namespace columnar {

namespace {

class Parser {
 public:
  explicit Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

  AstQuery parse() {
    AstQuery q = parse_select_core();
    match(TokenType::kSemicolon);  // optional trailing ';'
    if (!at_end()) {
      fail("unexpected trailing input after query");
    }
    return q;
  }

 private:
  const Token& peek() const { return tokens_[pos_]; }
  const Token& previous() const { return tokens_[pos_ - 1]; }
  bool check(TokenType t) const { return peek().type == t; }
  bool at_end() const { return check(TokenType::kEndOfFile); }

  const Token& advance() {
    if (!at_end()) ++pos_;
    return previous();
  }

  bool match(TokenType t) {
    if (check(t)) {
      advance();
      return true;
    }
    return false;
  }

  const Token& expect(TokenType t, std::string_view what_for) {
    if (check(t)) return advance();
    throw QueryError{"expected " + std::string(to_string(t)) + " " + std::string(what_for) +
                          ", got " + to_string(peek().type) +
                          (peek().text.empty() ? "" : " ('" + peek().text + "')"),
                      peek().line, peek().col};
  }

  [[noreturn]] void fail(std::string message) const {
    throw QueryError{std::move(message), peek().line, peek().col};
  }

  // ------------------------------------------------------------- query --

  AstQuery parse_select_core() {
    AstQuery q;
    expect(TokenType::kSelect, "to start a query");
    parse_select_list(q);
    expect(TokenType::kFrom, "after select list");
    const Token& table_tok = expect(TokenType::kIdentifier, "table name after FROM");
    q.from_table = table_tok.text;
    q.from_position = table_tok;
    if (match(TokenType::kAs)) {
      q.from_alias = expect(TokenType::kIdentifier, "after AS").text;
    } else if (check(TokenType::kIdentifier)) {
      q.from_alias = advance().text;
    }

    while (check(TokenType::kJoin) || check(TokenType::kInner) || check(TokenType::kLeft)) {
      q.joins.push_back(parse_join());
    }
    if (match(TokenType::kWhere)) {
      q.where = parse_expr();
    }
    if (match(TokenType::kGroup)) {
      expect(TokenType::kBy, "after GROUP");
      q.group_by.push_back(parse_expr());
      while (match(TokenType::kComma)) q.group_by.push_back(parse_expr());
    }
    if (match(TokenType::kOrder)) {
      expect(TokenType::kBy, "after ORDER");
      q.order_by.push_back(parse_order_item());
      while (match(TokenType::kComma)) q.order_by.push_back(parse_order_item());
    }
    if (match(TokenType::kLimit)) {
      const Token& n = expect(TokenType::kIntegerLiteral, "after LIMIT");
      if (n.int_value < 0) fail("LIMIT must be non-negative");
      q.limit = static_cast<std::uint64_t>(n.int_value);
    }
    return q;
  }

  void parse_select_list(AstQuery& q) {
    if (check(TokenType::kStar)) {
      advance();
      q.select_star = true;
      return;
    }
    q.select_list.push_back(parse_select_item());
    while (match(TokenType::kComma)) q.select_list.push_back(parse_select_item());
  }

  AstSelectItem parse_select_item() {
    AstSelectItem item;
    item.expr = parse_expr();
    if (match(TokenType::kAs)) {
      item.alias = expect(TokenType::kIdentifier, "after AS").text;
    } else if (check(TokenType::kIdentifier)) {
      item.alias = advance().text;
    }
    return item;
  }

  AstJoin parse_join() {
    AstJoin join;
    join.position = peek();
    join.type = JoinType::kInner;
    if (match(TokenType::kInner)) {
      join.type = JoinType::kInner;
    } else if (match(TokenType::kLeft)) {
      join.type = JoinType::kLeft;
    }
    expect(TokenType::kJoin, "to introduce a join");
    join.table = expect(TokenType::kIdentifier, "table name after JOIN").text;
    if (match(TokenType::kAs)) {
      join.alias = expect(TokenType::kIdentifier, "after AS").text;
    } else if (check(TokenType::kIdentifier)) {
      join.alias = advance().text;
    }
    expect(TokenType::kOn, "after joined table name");
    join.on = parse_expr();
    return join;
  }

  AstOrderItem parse_order_item() {
    AstOrderItem item;
    item.expr = parse_expr();
    if (match(TokenType::kDesc)) {
      item.descending = true;
    } else {
      match(TokenType::kAsc);
    }
    return item;
  }

  // ---------------------------------------------------------- expressions --

  AstExprPtr parse_expr() { return parse_or(); }

  AstExprPtr parse_or() {
    AstExprPtr lhs = parse_and();
    while (check(TokenType::kOr)) {
      const Token& op_tok = advance();
      AstExprPtr rhs = parse_and();
      lhs = make_binary(BinaryOp::kOr, std::move(lhs), std::move(rhs), op_tok);
    }
    return lhs;
  }

  AstExprPtr parse_and() {
    AstExprPtr lhs = parse_not();
    while (check(TokenType::kAnd)) {
      const Token& op_tok = advance();
      AstExprPtr rhs = parse_not();
      lhs = make_binary(BinaryOp::kAnd, std::move(lhs), std::move(rhs), op_tok);
    }
    return lhs;
  }

  AstExprPtr parse_not() {
    if (check(TokenType::kNot)) {
      const Token& op_tok = advance();
      AstExprPtr operand = parse_not();
      auto e = std::make_unique<AstExpr>();
      e->kind = AstExprKind::kUnary;
      e->un_op = UnaryOp::kNot;
      e->operand = std::move(operand);
      e->position = op_tok;
      return e;
    }
    return parse_comparison();
  }

  AstExprPtr parse_comparison() {
    AstExprPtr lhs = parse_additive();
    static const std::pair<TokenType, BinaryOp> ops[] = {
        {TokenType::kEq, BinaryOp::kEq},   {TokenType::kNe, BinaryOp::kNe},
        {TokenType::kLt, BinaryOp::kLt},   {TokenType::kLe, BinaryOp::kLe},
        {TokenType::kGt, BinaryOp::kGt},   {TokenType::kGe, BinaryOp::kGe},
    };
    for (const auto& [tok_type, op] : ops) {
      if (check(tok_type)) {
        const Token& op_tok = advance();
        AstExprPtr rhs = parse_additive();
        return make_binary(op, std::move(lhs), std::move(rhs), op_tok);
      }
    }
    return lhs;
  }

  AstExprPtr parse_additive() {
    AstExprPtr lhs = parse_multiplicative();
    while (check(TokenType::kPlus) || check(TokenType::kMinus)) {
      const Token& op_tok = advance();
      const BinaryOp op = op_tok.type == TokenType::kPlus ? BinaryOp::kAdd : BinaryOp::kSub;
      AstExprPtr rhs = parse_multiplicative();
      lhs = make_binary(op, std::move(lhs), std::move(rhs), op_tok);
    }
    return lhs;
  }

  AstExprPtr parse_multiplicative() {
    AstExprPtr lhs = parse_unary();
    while (check(TokenType::kStar) || check(TokenType::kSlash)) {
      const Token& op_tok = advance();
      const BinaryOp op = op_tok.type == TokenType::kStar ? BinaryOp::kMul : BinaryOp::kDiv;
      AstExprPtr rhs = parse_unary();
      lhs = make_binary(op, std::move(lhs), std::move(rhs), op_tok);
    }
    return lhs;
  }

  AstExprPtr parse_unary() {
    if (check(TokenType::kMinus)) {
      const Token& op_tok = advance();
      AstExprPtr operand = parse_unary();
      auto e = std::make_unique<AstExpr>();
      e->kind = AstExprKind::kUnary;
      e->un_op = UnaryOp::kNeg;
      e->operand = std::move(operand);
      e->position = op_tok;
      return e;
    }
    if (check(TokenType::kPlus)) {
      advance();
      return parse_unary();
    }
    return parse_primary();
  }

  AstExprPtr parse_primary() {
    const Token& tok = peek();

    if (match(TokenType::kIntegerLiteral)) {
      auto e = std::make_unique<AstExpr>();
      e->kind = AstExprKind::kIntLit;
      e->int_value = tok.int_value;
      e->position = tok;
      return e;
    }
    if (match(TokenType::kFloatLiteral)) {
      auto e = std::make_unique<AstExpr>();
      e->kind = AstExprKind::kFloatLit;
      e->double_value = tok.double_value;
      e->position = tok;
      return e;
    }
    if (match(TokenType::kStringLiteral)) {
      auto e = std::make_unique<AstExpr>();
      e->kind = AstExprKind::kStringLit;
      e->string_value = tok.text;
      e->position = tok;
      return e;
    }
    if (check(TokenType::kTrue) || check(TokenType::kFalse)) {
      advance();
      auto e = std::make_unique<AstExpr>();
      e->kind = AstExprKind::kBoolLit;
      e->bool_value = (tok.type == TokenType::kTrue);
      e->position = tok;
      return e;
    }
    if (check(TokenType::kCount) || check(TokenType::kSum) || check(TokenType::kAvg) ||
        check(TokenType::kMin) || check(TokenType::kMax)) {
      return parse_agg_call();
    }
    if (check(TokenType::kIdentifier)) {
      return parse_column_ref();
    }
    if (match(TokenType::kLParen)) {
      AstExprPtr inner = parse_expr();
      expect(TokenType::kRParen, "to close '('");
      return inner;
    }
    fail("expected an expression, got " + std::string(to_string(tok.type)) +
         (tok.text.empty() ? "" : " ('" + tok.text + "')"));
  }

  AstExprPtr parse_column_ref() {
    const Token& first = expect(TokenType::kIdentifier, "column reference");
    auto e = std::make_unique<AstExpr>();
    e->kind = AstExprKind::kColumnRef;
    e->position = first;
    if (match(TokenType::kDot)) {
      const Token& second = expect(TokenType::kIdentifier, "column name after '.'");
      e->table_qualifier = first.text;
      e->column_name = second.text;
    } else {
      e->column_name = first.text;
    }
    return e;
  }

  AstExprPtr parse_agg_call() {
    const Token& fn_tok = advance();
    AggFunc func;
    switch (fn_tok.type) {
      case TokenType::kCount: func = AggFunc::kCount; break;
      case TokenType::kSum: func = AggFunc::kSum; break;
      case TokenType::kAvg: func = AggFunc::kAvg; break;
      case TokenType::kMin: func = AggFunc::kMin; break;
      case TokenType::kMax: func = AggFunc::kMax; break;
      default: fail("internal: not an aggregate token");
    }
    expect(TokenType::kLParen, "after aggregate function name");
    auto e = std::make_unique<AstExpr>();
    e->kind = AstExprKind::kAggCall;
    e->agg_func = func;
    e->position = fn_tok;
    if (func == AggFunc::kCount && check(TokenType::kStar)) {
      advance();
      e->agg_func = AggFunc::kCountStar;
    } else {
      e->agg_arg = parse_expr();
    }
    expect(TokenType::kRParen, "to close aggregate call");
    return e;
  }

  static AstExprPtr make_binary(BinaryOp op, AstExprPtr lhs, AstExprPtr rhs, const Token& pos) {
    auto e = std::make_unique<AstExpr>();
    e->kind = AstExprKind::kBinary;
    e->bin_op = op;
    e->lhs = std::move(lhs);
    e->rhs = std::move(rhs);
    e->position = pos;
    return e;
  }

  std::vector<Token> tokens_;
  std::size_t pos_ = 0;
};

}  // namespace

AstQuery parse_query(std::string_view sql) {
  std::vector<Token> tokens = tokenize(sql);
  return Parser(std::move(tokens)).parse();
}

}  // namespace columnar
