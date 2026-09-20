#pragma once

#include <string_view>

#include "columnar/ast.hpp"

namespace columnar {

// Hand-written recursive-descent parser for the SQL subset in DESIGN.md's
// out-of-scope list (no subqueries/CTEs/window functions). Throws
// QueryError with a 1-based line/col on the offending token for any syntax
// error. Pure grammar -- doesn't know a Catalog exists.
AstQuery parse_query(std::string_view sql);

}  // namespace columnar
