#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace columnar {

enum class TokenType {
  kIdentifier,
  kIntegerLiteral,
  kFloatLiteral,
  kStringLiteral,
  // Keywords
  kSelect, kFrom, kWhere, kGroup, kBy, kOrder, kLimit, kJoin, kInner, kLeft,
  kOn, kAs, kAnd, kOr, kNot, kAsc, kDesc, kTrue, kFalse, kNullKw,
  kCount, kSum, kAvg, kMin, kMax,
  // Punctuation / operators
  kComma, kDot, kStar, kLParen, kRParen, kSemicolon,
  kPlus, kMinus, kSlash,
  kEq, kNe, kLt, kLe, kGt, kGe,
  kEndOfFile,
};

struct Token {
  TokenType type;
  std::string text;
  std::int64_t int_value = 0;
  double double_value = 0.0;
  std::size_t line = 1;
  std::size_t col = 1;
};

// Thrown by the lexer, parser, and binder alike -- one error shape all the
// way from "stray character" to "unknown column" to "type mismatch",
// always carrying a 1-based line/col so the caller can point at the exact
// token. See DESIGN.md/Phase 2: "real error messages with column
// positions" is a hard requirement, not a nice-to-have.
//
// Derives from std::exception (rather than being a bare struct) so a
// caller that only catches `const std::exception&` -- generic error
// handling in the server, a test harness's default reporter, anything not
// specifically SQL-aware -- still gets a sane message instead of an opaque
// "unknown exception".
struct QueryError : std::exception {
  QueryError(std::string msg, std::size_t line_, std::size_t col_)
      : message(std::move(msg)),
        line(line_),
        col(col_),
        formatted(std::to_string(line_) + ":" + std::to_string(col_) + ": " + message) {}

  const char* what() const noexcept override { return formatted.c_str(); }

  std::string message;
  std::size_t line;
  std::size_t col;

 private:
  std::string formatted;
};

// Hand-written, single-pass, no lookahead beyond one character -- no
// yacc/bison/re2c, so every token boundary decision here is explicit and
// inspectable. Returns the whole token stream up front (SQL queries are
// small; there's no benefit to lazy/streaming tokenization here).
std::vector<Token> tokenize(std::string_view source);

const char* to_string(TokenType type);

}  // namespace columnar
