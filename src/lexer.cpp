#include "columnar/lexer.hpp"

#include <cctype>
#include <unordered_map>

namespace columnar {

namespace {

const std::unordered_map<std::string, TokenType>& keyword_table() {
  static const std::unordered_map<std::string, TokenType> table = {
      {"SELECT", TokenType::kSelect}, {"FROM", TokenType::kFrom},
      {"WHERE", TokenType::kWhere},   {"GROUP", TokenType::kGroup},
      {"BY", TokenType::kBy},         {"ORDER", TokenType::kOrder},
      {"LIMIT", TokenType::kLimit},   {"JOIN", TokenType::kJoin},
      {"INNER", TokenType::kInner},   {"LEFT", TokenType::kLeft},
      {"ON", TokenType::kOn},         {"AS", TokenType::kAs},
      {"AND", TokenType::kAnd},       {"OR", TokenType::kOr},
      {"NOT", TokenType::kNot},       {"ASC", TokenType::kAsc},
      {"DESC", TokenType::kDesc},     {"TRUE", TokenType::kTrue},
      {"FALSE", TokenType::kFalse},   {"NULL", TokenType::kNullKw},
      {"COUNT", TokenType::kCount},   {"SUM", TokenType::kSum},
      {"AVG", TokenType::kAvg},       {"MIN", TokenType::kMin},
      {"MAX", TokenType::kMax},
  };
  return table;
}

std::string to_upper(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return out;
}

class Scanner {
 public:
  explicit Scanner(std::string_view src) : src_(src) {}

  std::vector<Token> run() {
    std::vector<Token> tokens;
    for (;;) {
      skip_whitespace_and_comments();
      if (at_end()) {
        tokens.push_back(make(TokenType::kEndOfFile, ""));
        break;
      }
      tokens.push_back(next_token());
    }
    return tokens;
  }

 private:
  bool at_end() const { return pos_ >= src_.size(); }
  char peek() const { return at_end() ? '\0' : src_[pos_]; }
  char peek_next() const { return pos_ + 1 < src_.size() ? src_[pos_ + 1] : '\0'; }

  char advance() {
    char c = src_[pos_++];
    if (c == '\n') {
      ++line_;
      col_ = 1;
    } else {
      ++col_;
    }
    return c;
  }

  Token make(TokenType type, std::string text, std::size_t start_line = 0,
             std::size_t start_col = 0) const {
    Token t;
    t.type = type;
    t.text = std::move(text);
    t.line = start_line ? start_line : line_;
    t.col = start_col ? start_col : col_;
    return t;
  }

  void skip_whitespace_and_comments() {
    for (;;) {
      if (at_end()) return;
      char c = peek();
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
        advance();
      } else if (c == '-' && peek_next() == '-') {
        while (!at_end() && peek() != '\n') advance();
      } else {
        return;
      }
    }
  }

  Token next_token() {
    const std::size_t start_line = line_;
    const std::size_t start_col = col_;
    const char c = advance();

    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
      return scan_identifier_or_keyword(c, start_line, start_col);
    }
    if (std::isdigit(static_cast<unsigned char>(c))) {
      return scan_number(c, start_line, start_col);
    }
    if (c == '\'') {
      return scan_string(start_line, start_col);
    }

    switch (c) {
      case ',': return make(TokenType::kComma, ",", start_line, start_col);
      case '.': return make(TokenType::kDot, ".", start_line, start_col);
      case '*': return make(TokenType::kStar, "*", start_line, start_col);
      case '(': return make(TokenType::kLParen, "(", start_line, start_col);
      case ')': return make(TokenType::kRParen, ")", start_line, start_col);
      case ';': return make(TokenType::kSemicolon, ";", start_line, start_col);
      case '+': return make(TokenType::kPlus, "+", start_line, start_col);
      case '-': return make(TokenType::kMinus, "-", start_line, start_col);
      case '/': return make(TokenType::kSlash, "/", start_line, start_col);
      case '=': return make(TokenType::kEq, "=", start_line, start_col);
      case '!':
        if (peek() == '=') {
          advance();
          return make(TokenType::kNe, "!=", start_line, start_col);
        }
        throw QueryError{"unexpected character '!'", start_line, start_col};
      case '<':
        if (peek() == '=') {
          advance();
          return make(TokenType::kLe, "<=", start_line, start_col);
        }
        if (peek() == '>') {
          advance();
          return make(TokenType::kNe, "<>", start_line, start_col);
        }
        return make(TokenType::kLt, "<", start_line, start_col);
      case '>':
        if (peek() == '=') {
          advance();
          return make(TokenType::kGe, ">=", start_line, start_col);
        }
        return make(TokenType::kGt, ">", start_line, start_col);
      default:
        throw QueryError{std::string("unexpected character '") + c + "'", start_line, start_col};
    }
  }

  Token scan_identifier_or_keyword(char first, std::size_t start_line, std::size_t start_col) {
    std::string text(1, first);
    while (!at_end() && (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '_')) {
      text.push_back(advance());
    }
    const std::string upper = to_upper(text);
    const auto& keywords = keyword_table();
    auto it = keywords.find(upper);
    if (it != keywords.end()) {
      return make(it->second, upper, start_line, start_col);
    }
    return make(TokenType::kIdentifier, text, start_line, start_col);
  }

  Token scan_number(char first, std::size_t start_line, std::size_t start_col) {
    std::string text(1, first);
    bool is_float = false;
    while (!at_end() && std::isdigit(static_cast<unsigned char>(peek()))) {
      text.push_back(advance());
    }
    if (!at_end() && peek() == '.' && std::isdigit(static_cast<unsigned char>(peek_next()))) {
      is_float = true;
      text.push_back(advance());
      while (!at_end() && std::isdigit(static_cast<unsigned char>(peek()))) {
        text.push_back(advance());
      }
    }
    Token t = make(is_float ? TokenType::kFloatLiteral : TokenType::kIntegerLiteral, text,
                    start_line, start_col);
    if (is_float) {
      t.double_value = std::stod(text);
    } else {
      t.int_value = std::stoll(text);
    }
    return t;
  }

  Token scan_string(std::size_t start_line, std::size_t start_col) {
    std::string text;
    while (true) {
      if (at_end()) {
        throw QueryError{"unterminated string literal", start_line, start_col};
      }
      char c = advance();
      if (c == '\'') {
        if (peek() == '\'') {  // '' is an escaped quote inside a string
          text.push_back(advance());
          continue;
        }
        break;
      }
      text.push_back(c);
    }
    return make(TokenType::kStringLiteral, text, start_line, start_col);
  }

  std::string_view src_;
  std::size_t pos_ = 0;
  std::size_t line_ = 1;
  std::size_t col_ = 1;
};

}  // namespace

std::vector<Token> tokenize(std::string_view source) { return Scanner(source).run(); }

const char* to_string(TokenType type) {
  switch (type) {
    case TokenType::kIdentifier: return "identifier";
    case TokenType::kIntegerLiteral: return "integer literal";
    case TokenType::kFloatLiteral: return "float literal";
    case TokenType::kStringLiteral: return "string literal";
    case TokenType::kSelect: return "SELECT";
    case TokenType::kFrom: return "FROM";
    case TokenType::kWhere: return "WHERE";
    case TokenType::kGroup: return "GROUP";
    case TokenType::kBy: return "BY";
    case TokenType::kOrder: return "ORDER";
    case TokenType::kLimit: return "LIMIT";
    case TokenType::kJoin: return "JOIN";
    case TokenType::kInner: return "INNER";
    case TokenType::kLeft: return "LEFT";
    case TokenType::kOn: return "ON";
    case TokenType::kAs: return "AS";
    case TokenType::kAnd: return "AND";
    case TokenType::kOr: return "OR";
    case TokenType::kNot: return "NOT";
    case TokenType::kAsc: return "ASC";
    case TokenType::kDesc: return "DESC";
    case TokenType::kTrue: return "TRUE";
    case TokenType::kFalse: return "FALSE";
    case TokenType::kNullKw: return "NULL";
    case TokenType::kCount: return "COUNT";
    case TokenType::kSum: return "SUM";
    case TokenType::kAvg: return "AVG";
    case TokenType::kMin: return "MIN";
    case TokenType::kMax: return "MAX";
    case TokenType::kComma: return "','";
    case TokenType::kDot: return "'.'";
    case TokenType::kStar: return "'*'";
    case TokenType::kLParen: return "'('";
    case TokenType::kRParen: return "')'";
    case TokenType::kSemicolon: return "';'";
    case TokenType::kPlus: return "'+'";
    case TokenType::kMinus: return "'-'";
    case TokenType::kSlash: return "'/'";
    case TokenType::kEq: return "'='";
    case TokenType::kNe: return "'!='";
    case TokenType::kLt: return "'<'";
    case TokenType::kLe: return "'<='";
    case TokenType::kGt: return "'>'";
    case TokenType::kGe: return "'>='";
    case TokenType::kEndOfFile: return "end of input";
  }
  return "?";
}

}  // namespace columnar
