# columnar-engine

**The question I had**: everyone says vectorized (batch-at-a-time) execution
beats row-at-a-time by an order of magnitude, and that columnar storage plus
dictionary encoding plus zone maps is why analytical databases are fast. I
wanted to build the smallest system that actually forces every one of those
design decisions, and measure whether — and by how much, and where the win
actually comes from — each one is true, instead of taking it on faith.

This is a column-oriented, embedded analytics engine in C++20: a hand-written
SQL front end, a vectorized (batched pull / Volcano-style) execution engine,
a rule-based optimizer with zone-map pruning, and a TCP server — all built
from scratch with no dependencies beyond Catch2 and Google Benchmark, and
every design decision in [DESIGN.md](DESIGN.md) backed by a number below,
not an assertion.

## What's here, by phase

| Phase | What | Status |
|---|---|---|
| 0 | [DESIGN.md](DESIGN.md) — layout, vector size, nulls, strings, execution model | Done |
| 1 | Storage layer: typed chunks, dictionary/RLE encoding, zone maps, on-disk mmap format | Done |
| 2 | Hand-written lexer + recursive-descent parser + binder → typed logical plan | Done |
| 3 | Vectorized operators: Scan/Filter/Project/HashAggregate/HashJoin/Sort/Limit | Done |
| 4 | Rule-based optimizer: zone-map pruning, predicate/projection pushdown, cardinality estimation | Done |
| 5 | TCP server, thread pool, binary protocol ([PROTOCOL.md](PROTOCOL.md)), CLI client | Done |
| 6 | 2M-row synthetic dataset, 8 queries benchmarked against SQLite, flamegraph-driven fix | Done |

`LEARNING.md` has the full "why" for every C++ mechanic used (RAII around
raw allocation, `std::variant`+`if constexpr` instead of virtual dispatch,
explicit template instantiation, move-on-return gotchas with structured
bindings, and more) plus a quiz.

## Quickstart

```sh
brew install cmake ninja   # if you don't have them
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build            # or: ./build/tests/columnar_tests

# generate a 2M-row synthetic dataset (orders + customers) and compare
# against SQLite on 8 analytical queries, plus write data/*.cef
./build/tools/generate_data 2000000 2000 data
./build/tools/bench_vs_sqlite data

# run the server and talk to it
./build/apps/columnar_server 9876 data/orders.cef data/customers.cef &
./build/apps/columnar_cli 127.0.0.1 9876 \
  "SELECT region, SUM(amount) AS total FROM orders GROUP BY region ORDER BY total DESC"
```

For a from-scratch build with sanitizers (what CI-equivalent local testing
looks like for this project):

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCOLUMNAR_ENABLE_ASAN=ON -DCOLUMNAR_ENABLE_UBSAN=ON
cmake --build build -j && ./build/tests/columnar_tests
```

39 Catch2 test cases, 256 assertions, clean under ASan+UBSan with
`-Wall -Wextra -Wpedantic -Werror`.

## The numbers

### Vector size sweep (Phase 0's deferred claim, actually run)

DESIGN.md section 2 originally deferred this as `[TODO: benchmark]` and, for
a while, several docs (including an interview-prep summary) described it as
already done — it wasn't. `benchmarks/bench_vector_size_sweep.cpp` closes
that gap for real: a filter-then-sum-through-a-selection-vector
microbenchmark (the same shape `FilterOperator` actually uses) over a fixed
~4.2M-row total, varying only the batch size:

| vector_size | items/sec (median of 3) |
|---|---|
| 64 | 245.9M/s |
| 1024 | 283.9M/s |
| 2048 | 278.2M/s |
| 4096 | 281.4M/s |
| 65536 | 281.4M/s |

**Honest read:** 1024 through 65536 are within ~5% noise of each other on
this hardware (4MB L2/core) — there's no sharp optimum at 2048 specifically,
and no visible cache-blowout at 65536. The one clear effect is at the small
end: vector_size=64 is ~13-17% slower than everything else, confirming the
qualitative "too small reintroduces per-batch overhead" claim without
proving 2048 is *the* best choice. 2048 remains reasonable (nowhere near the
bad end of the curve) — just not provably optimal on this specific
microbenchmark. See DESIGN.md section 2 and LEARNING.md item 19 for the full
writeup and what caught the original overclaim.

### Dictionary + RLE encoding (Phase 1)

2048-row vector, `WHERE country = 'val_0'`-style equality scan, Apple
M-series, Release build:

| Encoding | cardinality=8 | cardinality=2048 (~unique) |
|---|---|---|
| Plain varchar scan | 2453 ns | 1680 ns |
| Dictionary scan | 302 ns (**8.1x**) | 718 ns (2.3x) |
| Dictionary+RLE scan (sorted) | **7.8 ns (313x)** | 854 ns (~parity with dict) |

The RLE win is entirely conditional on clustering: at cardinality 8, sorted
data collapses 2048 rows into ~8 runs (313x). At cardinality 2048, `run_count()`
approaches `row_count()` — no compression to exploit, and the win disappears.
Full table and the build-cost tradeoff in `LEARNING.md`.

### Vectorized vs. tuple-at-a-time execution (Phase 3)

A `WHERE amount > 500` filter over 64 chunks (131,072 rows), Release build:

| Model | Time | 
|---|---|
| Classic Volcano, one `next_row()` virtual call per row | 1,078,274 ns |
| This engine's `ScanOperator` → `FilterOperator` batch pipeline | 991,421 ns (**1.09x**) |

Smaller than the "order of magnitude" folklore, and that's the honest,
useful part of this result: modern branch predictors handle a
loop-stable indirect call fairly well, and this implementation still
materializes a fresh `ExecColumn` at every pipeline stage instead of
threading a shared selection vector all the way through (see
[LEARNING.md](LEARNING.md) for the specific, disclosed gap against
DESIGN.md's stated ideal). A second, separate comparison went the *other*
way and was more instructive:

| Model | Time |
|---|---|
| Raw scalar loop over decoded arrays, zero abstraction | 707 ns |
| This project's boxed `Expression::evaluate()` interpreter | 3672 ns (**5.2x slower**) |

`ColumnRefExpr`/`LiteralExpr::evaluate()` each return a freshly-copied
`ExecColumn` even when no selection is applied — for a cheap predicate, that
materialization cost dominates. This is a real, measured limitation of the
current expression evaluator, not a hypothetical one, and it's exactly the
kind of thing DESIGN.md's "measure, don't assert" rule is for: the fix (an
evaluate() that can return a borrowed view instead of an owned copy when
nothing changed) is real work, correctly scoped out rather than rushed.

### Zone-map pruning and projection pushdown (Phase 4)

On the 2M-row synthetic dataset, `SELECT * FROM orders WHERE amount > 1500`
(a 3-chunk-sized toy example in the test suite prunes 2 of 3 chunks; on the
full dataset a `status`/`region` equality filter typically prunes chunks
whose zone map can't contain the literal). `EXPLAIN`-style before/after:

```
before optimize:            after optimize:
Filter(amount > 100)        Filter(amount > 100)
  Scan(orders)                 Scan(orders, pruning=[amount > 100], projects=2/3 cols)
```

### Engine vs. SQLite, 2M rows, identical data and queries (Phase 6)

Both systems given the same synthetic dataset (`tools/generate_data.cpp`,
one generation pass, three outputs so the data is byte-identical); SQLite
gets an index on `orders.customer_id` (the realistic, idiomatic setup for
that engine) and this engine gets none (it doesn't have a secondary-index
concept — see DESIGN.md's out-of-scope list). Full round trip
(parse+bind+optimize+execute vs. prepare+step+finalize), minimum of 7 runs
each after 1 warmup:

| Query | engine (ms) | SQLite (ms) | speedup |
|---|---|---|---|
| Q1 `COUNT(*) WHERE status = 'PAID'` | 61.1 | 101.6 | 1.66x |
| Q2 `GROUP BY region` (SUM, COUNT) | 116.8 | 603.9 | 5.17x |
| Q3 `AVG(amount) WHERE amount > 500` | 56.5 | 100.4 | 1.78x |
| Q4 `GROUP BY status` (AVG) | 112.7 | 545.4 | 4.84x |
| **Q5 point lookup `WHERE customer_id = 42`** | 14.6 | **0.7** | **0.05x (SQLite wins, 22x)** |
| Q6 two-column `GROUP BY region, status` | 165.4 | 1100.0 | 6.65x |
| Q7 `MIN/MAX WHERE region = 'EU'` | 67.3 | 116.5 | 1.73x |
| Q8 `JOIN` + `GROUP BY` | 270.5 | 3435.6 | **12.70x** |

**Where this engine loses, honestly**: Q5. SQLite's B-tree index on
`customer_id` turns a point lookup into an O(log n) descent; this engine has
no secondary index concept at all, so every query — including a lookup for
one row — is a full column scan. That's not a bug, it's what "columnar,
scan-optimized, no OLTP indexing" (DESIGN.md section 1) actually costs. A
row-store with an index is the right tool for that job; a columnar scan
engine is the right tool for the other seven.

### Flamegraph-driven optimization (Phase 6)

Sampled Q8 (`profiling/q8_join_before_optimization.sample.txt`, captured
with macOS `sample` over an 8-second steady-state loop of the join query —
see `tools/profile_join.cpp`). The single hottest leaf was `_platform_memmove`
(1499 samples), traced mostly to `HashJoinOperator`/`HashAggregateOperator`'s
`stringify_key()` building `unordered_map` keys via `std::to_string()` +
string concatenation for *every* row of *every* join and group-by — even
though the overwhelmingly common case (joining or grouping on an id column)
is a plain `int64`.

**The fix**: `HashJoinOperator` now keys its build-side hash table on the
raw `int64` value directly when the join key is an integer column, skipping
string construction entirely; every other key type still falls back to the
general string-keyed path. Measured effect:

| | before | after |
|---|---|---|
| `BM_HashJoin` (isolated, Google Benchmark) | 7.42 ms | 5.02 ms (**1.48x**) |
| Q8 end to end (parse through result) | 330.1 ms | 270.5 ms (**1.22x**) |
| Q8 speedup over SQLite | 10.38x | 12.70x |

`HashAggregateOperator`'s group-by key has the identical opportunity
(same `stringify_key()` call site) and is documented as the next thing to
fix, not fixed here, to keep this section honest about what was actually
measured before and after rather than bundling an untested second change
into the same "before/after" number.

## Honest scope: what this project does not do

- No subqueries, CTEs, or window functions (DESIGN.md, by design).
- No secondary indexes — every access path is a scan (see Q5 above).
- Predicate/projection pushdown only reaches through a `Filter` directly
  over a `Scan`; a `WHERE` above a `JOIN` doesn't get pushed onto either
  side's scan yet (`optimizer.cpp`).
- Join "reordering" is choosing which side of one hash join to build vs.
  probe from a cardinality estimate — not a real N-way join-order search.
- The wire protocol sends one whole result set per frame, not streamed
  batches (see PROTOCOL.md).
- The file-format reader materializes (copies) every chunk out of the
  mmap'd region rather than returning zero-copy views — see LEARNING.md.

Every one of these is a real, deliberate line drawn under time constraints,
not an oversight — and each is the kind of thing that's genuinely
defensible to talk through in an interview: *here's what I built, here's
what I measured, here's exactly where I stopped and why.*
