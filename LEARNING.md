# LEARNING.md — Phase 1: Storage Layer

What actually got exercised while building the storage layer, and why each
mechanic was the right tool rather than a stylistic choice.

## 1. RAII around a raw allocation, not avoidance of raw allocation

`AlignedBuffer<T>` (`include/columnar/aligned_buffer.hpp`) is the one place
in the codebase that calls `::operator new`/`::operator delete` directly --
with the C++17 aligned-new overload, `::operator new(size, std::align_val_t{64})`,
because `std::vector<T>`'s allocator has no portable way to request
over-alignment before C++17, and even after, wiring a custom allocator into
`std::vector` for this is more machinery than just owning the buffer
directly. The "no `new`/`delete` in application code" rule isn't about never
calling an allocation function -- it's about not scattering ad hoc
allocation through business logic. Every other class in this phase
(`Bitmap`, `PlainColumnChunk<T>`, `VarcharColumnChunk`, ...) never touches
allocation at all; they hold an `AlignedBuffer`, a `std::vector`, or a
`std::string`, and get correct copy/move/destroy behavior for free.

## 2. `std::variant` + `std::visit` + `if constexpr` instead of virtual dispatch

`ColumnChunkVariant` is a `std::variant` over the seven concrete chunk
types, not a `ColumnChunk` base class with virtual methods. Two reasons,
both load-bearing:

- This project's whole thesis is that per-row virtual dispatch is what
  makes tuple-at-a-time execution slow. Using virtual dispatch at the
  *storage* layer, even though Phase 1 doesn't scan row-by-row yet, would
  build the wrong habit into the codebase for Phase 3.
- `std::visit` with `if constexpr` branches (see `write_chunk` in
  `file_format.cpp`) is *exhaustiveness-checked at compile time*: the
  `else { static_assert(!sizeof(ChunkT*), ...) }` fallback means adding an
  eighth alternative to the variant and forgetting to handle it in the
  writer is a compile error, not a silent runtime gap. A virtual method
  can't give you that -- forgetting to override it in a new subclass just
  silently calls the base implementation (or is a pure-virtual link error
  at best).

The cost, honestly: `std::visit` call sites are less readable than a
virtual call, and adding a variant alternative means touching every
`visit`/`if constexpr` site instead of just writing one new subclass. For
seven closed, compile-time-known alternatives that's a fine trade;
it would not be if this were meant to be an open/extensible type set.

## 3. Explicit template instantiation

`PlainColumnChunk<T>`'s constructor is declared in the header but *defined*
in `column_chunk.cpp`, with `template class PlainColumnChunk<std::int32_t>;`
(and `int64_t`, `double`) at the bottom. Normally a template's definition
has to be visible wherever it's instantiated, which is why templates are
usually header-only. Explicit instantiation is the escape hatch: since
there are exactly three concrete `T`s this engine will ever use, the
definition can live in one `.cpp`, get compiled once, and every other
translation unit that includes the header just links against those three
pre-built symbols instead of re-instantiating (and re-optimizing) the same
code repeatedly. Trade-off: instantiating a fourth type (say `int16_t`)
later requires editing `column_chunk.cpp`, not just using the type --
explicit instantiation trades header-only flexibility for compile time and
binary size, and that's a knowingly-made choice, not an oversight.

## 4. `std::string_view` + `vector::reserve` to dodge a real dangling-pointer bug

`DictionaryColumnChunk`'s constructor builds a
`std::unordered_map<std::string_view, std::uint32_t>` where each key is a
view into an element already stored in `dictionary_` (a
`std::vector<std::string>`). The first version of this code did *not*
reserve `dictionary_` up front, and it would have been a live ASan failure:
every `std::vector::push_back` that exceeds capacity reallocates, which
*moves* every existing element to new storage. For a `std::string` using
small-string optimization, "moving" a short string means copying its
characters into the new object's inline buffer at a new address --
invalidating any `string_view` that pointed at the old address, not just
the one for the element that grew the vector. `dictionary_.reserve(row_count_)`
up front (worst case: every row a distinct value) means the vector's buffer
never moves during the loop, so every `string_view` taken into it stays
valid for the object's whole lifetime. This is the canonical
"iterator/reference/pointer invalidation on reallocation" gotcha, just with
a `string_view` standing in for an iterator.

## 5. `std::vector<bool>` is not a container of `bool`

Every test and benchmark in this phase builds validity masks with
`std::array<bool, N>` or `std::unique_ptr<bool[]>`, never `std::vector<bool>`.
`vector<bool>` is a bit-packed specialization with no real, contiguous
`bool*` backing it, so it cannot bind to `std::span<const bool>` --
attempting it is a hard compile error, not a subtle bug, but it's exactly
the kind of "this looks like it should obviously work" trap that's worth
having hit once on purpose.

## 6. `std::initializer_list` always copies

Building a `Table` by writing
`table.columns.push_back({schema, {ColumnChunkVariant(chunk)}})` doesn't
compile: `ColumnChunkVariant` is move-only (it holds an `AlignedBuffer`
transitively), and every element of a braced-init-list is `const` and gets
*copied* into the resulting container, never moved -- even when the source
is an rvalue temporary. This is a real limitation of `std::initializer_list`
(it owns nothing and can't safely hand out non-const/movable references to
its own backing array), not a compiler bug. The fix is to construct
piecewise and `push_back(std::move(...))`, which is why every place in this
codebase that builds a `Table`/`Column` does that instead of nested
brace-init.

## 7. `std::popcount` for branchless bit counting

`Bitmap::count_set()` uses `std::popcount` (C++20, `<bit>`) per byte rather
than a per-bit loop with a branch. This is both simpler to read and faster:
`popcount` compiles to a single hardware instruction on both ARM64 (`cnt`)
and modern x86_64 (`popcnt`), where a naive bit-by-bit loop is a
data-dependent branch per bit -- exactly the kind of thing this whole
project is about learning to avoid.

## 8. RAII around an OS resource, not just heap memory

`MappedFile` in `file_format.cpp` wraps `open`/`fstat`/`mmap` and releases
`munmap`/`close` in its destructor, with the copy constructor/assignment
deleted (mmap'd regions and file descriptors aren't safely copyable). This
is the same RAII discipline as `AlignedBuffer`, applied to an OS handle
instead of a `malloc`'d buffer -- the pattern generalizes past memory.

## What Phase 1's mmap reader does *not* do yet

`read_table` mmaps the file (so the OS handles paging instead of one big
`read()` into a manually managed buffer) but then `memcpy`s every chunk's
bytes into a freshly allocated, natively-aligned `AlignedBuffer` --
it materializes rather than returning zero-copy views into the mapping.
A fully zero-copy reader would need the chunk types to support a
non-owning "view" mode (a `PlainColumnChunkView<T>` holding a raw pointer
instead of an `AlignedBuffer`), which doubles the type surface for a win
that hasn't been measured yet. Deferred on purpose -- pick it up if a
later benchmark shows the materializing copy actually costs something on
the workloads Phase 6 cares about.

## The dictionary/RLE numbers (Apple M-series, Release build, `kVectorSize=2048`)

| Benchmark | cardinality=8 | cardinality=2048 (~unique) |
|---|---|---|
| Build: plain varchar | 27.5 μs | 29.5 μs |
| Build: dictionary | 43.2 μs | 103.7 μs |
| Build: dictionary+RLE (sorted) | 21.8 μs | 108.8 μs |
| Build: dictionary+RLE (shuffled) | 50.7 μs | 121.1 μs |
| Scan `WHERE col = const`: plain | 2453 ns | 1680 ns |
| Scan `WHERE col = const`: dictionary | 302 ns (8.1x) | 718 ns (2.3x) |
| Scan `WHERE col = const`: dict+RLE (sorted) | **7.8 ns (313x)** | 854 ns (~parity with dict) |

Reading this the way the interview question will actually be asked:

- **Dictionary encoding costs more to build** (hashing + dictionary lookups
  during ingest) **and pays it back on every subsequent scan** by turning a
  string comparison into an integer comparison -- a classic write-once,
  read-many trade that only makes sense for analytical (not OLTP) workloads.
- **RLE's payoff is entirely conditional on clustering.** At cardinality 8,
  sorted data collapses 2048 rows into ~8 runs, so a scan only has to check
  8 things instead of 2048 -- the 313x number. At cardinality 2048 (data
  that's "sorted" but still nearly all-unique), `run_count()` approaches
  `row_count()`, there's no compression to exploit, and RLE's scan cost
  converges to plain dictionary's -- exactly DESIGN.md's predicted "this
  stops paying for itself" crossover, now measured instead of asserted.
- Notice dictionary+RLE unsorted build is the *slowest* build path at every
  cardinality: run-header overhead (12 bytes/run) on data with no runs to
  exploit is pure loss. RLE is a bet you make when you know (or detect) the
  data is clustered -- never unconditionally.

Full sweep: `./build_release/benchmarks/columnar_bench_encoding`.

## Quiz (Phase 1)

1. Why does `AlignedBuffer` reject `T`s that aren't trivially copyable, and
   what would go wrong if it allowed, say, `std::string`?
2. `DictionaryColumnChunk` reserves `dictionary_` to `row_count_` up front
   even though most columns will have far fewer distinct values than rows.
   What bug does this prevent, and what would it take to reserve tighter
   (say, an estimated cardinality) without reintroducing that bug?
3. Why can a null in the middle of an otherwise-repeating run legitimately
   *end* an RLE run instead of being folded into it? What would change about
   `DictionaryRleColumnChunk` if nulls were tracked with a separate validity
   `Bitmap` instead of an `is_null` flag per run?
4. `write_chunk` uses `std::visit` with `if constexpr` and a
   `static_assert(!sizeof(ChunkT*), ...)` fallback branch. What does that
   fallback actually protect against, and why is `!sizeof(ChunkT*)` used
   instead of just `static_assert(false, ...)`?
5. The on-disk format pads each chunk's data block to a 64-byte boundary,
   but `read_table` still `memcpy`s everything into fresh buffers instead of
   pointing into the mapping. Given that, what does the 64-byte alignment
   in the *file* actually buy today, if anything -- and what would have to
   change in the chunk types for it to start mattering?

---

# Phases 2-6: front end, execution, optimizer, server, proof

## 9. AST vs. bound plan as two separate trees, not one mutable pass

`parser.cpp` produces an `AstQuery`/`AstExpr` tree with zero knowledge of
any `Catalog` -- pure grammar. A separate `Binder` (`logical_plan.cpp`)
walks that tree once, resolving every identifier against a `Scope` and
producing an entirely different, typed tree (`Expression`/`LogicalPlan`).
Two trees, not "annotate the AST in place," because the two passes have
genuinely different failure modes (syntax vs. semantics) and different
testability needs -- `parse_query` can be unit-tested with zero catalog
setup at all.

## 10. A structured binding is not a named variable

```cpp
auto [plan, scope] = bind_from_and_joins(query);
...
return plan;  // ERROR: copies a move-only LogicalPlanPtr, fails to compile
return std::move(plan);  // correct
```

The implicit move-on-return rule (a local variable named in a `return`
statement is treated as an rvalue for overload resolution) only applies to
an actual named variable. `plan` here is a structured-binding name --
under the hood it refers to a member of an invisible tuple-like object --
and the standard's wording for the implicit-move rule doesn't cover that
case. `bind()` in `logical_plan.cpp` has the real example and a comment
explaining exactly this.

## 11. `std::variant` + `if constexpr` again, now for physical operators

`decode_chunk()` and `zone_map_may_match()` (`operators.cpp`) both dispatch
on `ColumnChunkVariant` the same way `column_chunk.cpp`'s writer does:
`std::visit` with an `if constexpr` chain and a `static_assert(!sizeof(ChunkT*))`
fallback. Same reasoning as Phase 1, reused deliberately rather than
switching to virtual dispatch just because this is "the execution layer
now" -- the type set is still closed and compile-time known.

By contrast, `Operator` (Scan/Filter/Project/...) genuinely *is* a virtual
base class with `next()` a real virtual call. That's not a contradiction:
DESIGN.md's "no per-row virtual dispatch" concern is about *rows*, and an
`Operator::next()` call happens once per *batch* (up to 2048 rows). Paying
one virtual call per 2048 rows is exactly the "amortize the dispatch, don't
eliminate it" idea the whole vectorized-execution model is built on.

## 12. `dynamic_cast` as an acceptable, deliberate cost

The optimizer (`optimizer.cpp`) uses `dynamic_cast<const ColumnRefExpr*>`,
`dynamic_cast<const BinaryExpr*>`, etc. to pattern-match expression shapes
(`try_extract_scan_predicate`, `collect_column_refs`). This runs once per
query at optimization time, not per row -- RTTI's runtime cost is
irrelevant at that frequency, and `dynamic_cast` is simpler and safer than
adding a hand-rolled `kind()` tag to every `Expression` subclass just to
avoid it. Matching the cost of a mechanism to how often it actually runs is
the same judgment call DESIGN.md makes about vector size and virtual calls,
just applied one layer up.

## 13. A predicate's *type* has to come from the column, not the literal

`amount > 100` parses `100` as an `int64` literal even though `amount` is a
`double` column -- perfectly normal SQL, and `BinaryExpr::evaluate()`
handles it correctly by promoting int to double at comparison time. The
first version of `try_extract_scan_predicate` stored the *literal's* type
(`int64`) into the `ScanPredicate` used for zone-map pruning; since the
zone map itself is typed `double`, `zone_map_may_match` saw a type mismatch
and silently refused to prune -- for the single most natural way to write
that predicate. The fix reads the *column's* type off the `ColumnRefExpr`
and promotes the literal to match. Caught by an end-to-end test that
actually asserted on `chunks_pruned`, not just on query correctness --
the lesson: a feature whose whole point is a side effect (fewer chunks
touched) needs a test that checks the side effect, not just the answer.

## 14. Qualifier matching has to be asymmetric

`ORDER BY o.id` failed to resolve after a `JOIN`, even though `o.id` was
exactly the column selected as `id`. The post-SELECT `Scope` built for
`ORDER BY` binding carries no table qualifier at all (its columns came from
arbitrary expressions, not one table), so a strict `scope_column.qualifier
== e.table_qualifier` check rejected every candidate. The fix: a qualifier
mismatch only disqualifies a candidate when the *scope* column also has a
non-empty qualifier to disagree with. A pre-join/scan `Scope` always has
real qualifiers (so this doesn't loosen anything there); a post-select
`Scope` never does (so a user-supplied qualifier there is advisory, not
load-bearing). Two different scope "shapes" needing different matching
strictness, discovered by an ASan-clean but *logically* wrong test result,
not a crash.

## 15. Throwing a type that isn't `std::exception` is a real ergonomics bug

`QueryError` started as a bare struct (`{message, line, col}`). Thrown and
left uncaught inside a Catch2 `TEST_CASE`, it surfaced as "unexpected
exception... Unknown exception" -- Catch2 (like most generic exception
handling, logging, and test-runner code) knows how to print anything
caught as `const std::exception&`, and nothing else. Deriving `QueryError`
from `std::exception` with a real `what()` fixed this for every future
caller, not just the one test -- a good example of a "boring" fix that's
actually a correctness improvement in disguise (a production server
catching `const std::exception&` around a request handler would otherwise
have silently swallowed every parse/bind error with no message at all).

## 16. RAII around a caller-owned "arena," not a `static` cache

Lowering a hash join sometimes needs to build a *new* reordering
`Expression` tree that no `LogicalPlan` node owns (see
`build_physical_plan`'s join build-side swap in `operators.cpp`). The
tempting quick fix -- a function-local `static std::vector<...>` to keep it
alive -- is a real bug: it never frees across queries (unbounded growth in
a long-running server) and isn't thread-safe. The actual fix threads an
`ExprArena` (`std::deque<std::vector<ExpressionPtr>>`) through
`build_physical_plan` as an out-parameter the *caller* owns for exactly the
query's lifetime. `std::deque`, not `std::vector`, for the same reason
Phase 1's dictionary builder needed `reserve()`: pushing a new entry must
never invalidate a reference an earlier `ProjectOperator` already took to
`arena.back()`.

## 17. A benchmark's result is data -- verify what it's actually measuring

The first cut of the "tuple-at-a-time vs. vectorized" benchmark compared a
raw scalar loop over a decoded array against `Expression::evaluate()`, and
the "tuple-at-a-time" loop *won* -- the opposite of what DESIGN.md predicts.
The loop wasn't actually testing what DESIGN.md means by tuple-at-a-time
(virtual dispatch per row through an iterator chain); it was a
branch-predictable, auto-vectorizable scalar reduction, which is closer in
spirit to what a *good* vectorized kernel compiles to than to classic
Volcano. The fix was building an actual `RowIterator` virtual-dispatch
chain (`TupleScanIterator` -> `TupleFilterIterator`) for a fair comparison,
and keeping the original (now correctly labeled) scalar-loop-vs-interpreter
comparison as a *separate*, still-genuine data point -- see README.md for
both numbers and what each one actually shows.

## 18. `sample` (or `perf record`) plus a steady-state driver, no extra tooling

Getting a real flamegraph-equivalent profile needed no downloaded tooling:
`tools/profile_join.cpp` just runs one query in a tight loop for several
seconds so a sampler has a steady-state target, and macOS's built-in
`sample <pid> <seconds> -file out.txt` (an `strace`/`perf record` analog
that ships with Xcode command-line tools) produces a weighted call tree
plus a "sort by top of stack" leaf-function summary -- everything a
flamegraph shows, in text form. `profiling/q8_join_before_optimization.sample.txt`
is that raw output, committed as evidence for the optimization in
README.md rather than just asserted.

## Quiz (Phases 2-6)

1. Why are the AST (`ast.hpp`) and the bound logical plan
   (`logical_plan.hpp`) two separate type hierarchies instead of one tree
   that gets annotated in place during binding?
2. `HashAggregateOperator` and `HashJoinOperator` both call the general
   string-keyed `stringify_key()`, but only `HashJoinOperator` got the
   int64 fast-path fix in Phase 6. Walk through what `build_right_side()`
   and `compute()`'s grouping loop would need to change to apply the same
   fix to `HashAggregateOperator`, and estimate (before running it) whether
   you'd expect a bigger or smaller win than the join got, and why.
3. `optimizer.cpp`'s `try_extract_scan_predicate` only recognizes
   `column OP literal` or `literal OP column` at the top of a conjunct.
   Why does the whole system stay *correct* (never wrong answers) even
   when a predicate can't be decomposed this way -- what guarantees that?
4. Why does `ScanOperator`'s projection pushdown decode a placeholder
   column (rather than, say, omitting the column from the batch entirely)
   for a column nothing downstream references?
5. The wire protocol (PROTOCOL.md) sends an entire result set in one frame.
   Sketch (you don't have to implement it) what would need to change in
   `Frame`/`MessageType` to stream a large result across multiple frames
   without breaking a client that only understands the current protocol
   version.
