#pragma once

#include "columnar/logical_plan.hpp"
#include "columnar/operators.hpp"

namespace columnar {

// Rule-based, not cost-based-search: applies a fixed sequence of rewrites
// once, in a single bottom-up pass. Real optimizers explore a search space
// (join orderings, alternative access paths) and pick the cheapest plan by
// estimated cost; this applies rules that are unconditionally profitable
// (or, for join build-side choice, decided directly from a cardinality
// estimate) and stops there. See DESIGN.md's out-of-scope list.
//
// Rewrites applied:
//  - Constant folding: `1 + 2` becomes the literal `3` wherever it appears.
//  - Predicate pushdown + zone-map pruning: a Filter directly over a Scan
//    gets its simple `column OP literal` conjuncts copied onto the Scan as
//    ScanPredicates (a pruning *hint*; the Filter itself is always kept,
//    since a zone map can only prove "definitely not," never "definitely
//    yes"). ScanOperator uses these to skip whole chunks.
//  - Projection pushdown: for a single-table (no JOIN) query shape, the set
//    of columns actually referenced above the scan is computed and stored
//    on the Scan so ScanOperator can skip decoding the rest. Not attempted
//    across a JOIN -- see LEARNING.md.
LogicalPlanPtr optimize(LogicalPlanPtr plan);

// A crude, rule-of-thumb cardinality estimator over zone-map/dictionary
// statistics actually present in `db` (e.g. a dictionary column's real
// dictionary_size() feeds an equality predicate's selectivity) where
// available, and fixed heuristics (equality ~10%, range ~33%, ...)
// otherwise. Good enough to compare against actual measured row counts in
// EXPLAIN output -- not good enough to drive a real cost-based join-order
// search, which is why join "reordering" here is limited to picking which
// side of a hash join gets built vs. probed (see build_physical_plan).
double estimate_row_count(const LogicalPlan& plan, const Database& db);

}  // namespace columnar
