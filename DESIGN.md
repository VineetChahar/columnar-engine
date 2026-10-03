# DESIGN.md — Phase 0

## What this project is

A column-oriented, embedded, vectorized analytics engine in C++20. The point is
not to compete with DuckDB — it's to build the smallest system that still forces
every decision a real analytical engine has to make, and to measure the payoff of
each one instead of asserting it. Every claim in this document gets a benchmark
attached to it before Phase 6 is done; where a claim is still unverified, it's
marked `[TODO: benchmark]`.

Working name: **columnar-engine** (renameable once it has a personality).

---

## 1. Columnar vs. row layout

**Row layout** (like a `struct` array / most OLTP engines): all fields of one
record are adjacent in memory. Good for "give me everything about row 47"
(point lookups, OLTP updates). Bad for "sum column `amount` across 10M rows" —
you drag every other column through cache for no reason, and there is no
regular stride the CPU can prefetch or vectorize over, because the compiler
sees a stride of `sizeof(struct)` bytes with unrelated data in between.

**Columnar layout**: each column is a separate, densely packed, homogeneously
typed array. This buys three things simultaneously:

1. **Cache efficiency for scans.** A query touching 3 of 20 columns reads
   3/20th of the data from memory/disk. Zone maps and projection pushdown
   (Phase 4) only make sense because the storage is already column-shaped.
2. **Compressibility.** A column of one type has low entropy relative to a
   row (dates next to prices next to strings compress poorly together;
   same-typed, sorted-ish data compresses well — this is *why* dictionary
   encoding and RLE live in Phase 1 as column-level, not row-level,
   transforms).
3. **Vectorization.** A tight loop over `int32_t data[2048]` is exactly the
   shape auto-vectorizers (and manual SIMD later) want: contiguous, single
   type, no branches, no pointer chasing. A loop over `struct{Row} rows[2048]`
   is not, even if the total bytes touched were somehow the same.

**What we give up:** point lookups and single-row inserts/updates are worse —
inserting one row means touching N column arrays instead of one contiguous
record, and there's no row to "delete" cheaply (that's why real columnar
systems use delete bitmaps / MVCC instead of in-place row removal — out of
scope here, but worth saying out loud in an interview: this engine is
read/scan-optimized and would be a poor OLTP backend).

`[TODO: benchmark]` Phase 3 will include a microbenchmark: sum one column
out of a 10-column, 1M-row table, columnar vs. an equivalent
array-of-structs layout, same compiler flags. This is the number that
justifies this entire section.

---

## 2. Chunk size and the cache hierarchy

Data is processed in **batches of a fixed vector size**, not one row at a
time and not "all of it." Start with **2048 rows per vector**
(this matches DuckDB's default and MonetDB/X100's original choice, which is
a signal the number isn't arbitrary — but it will be re-derived from measured
numbers, not taken on faith).

Why 2048, in terms of cache math:

- A typical Apple Silicon / x86 desktop core: L1D is 32–128 KB, L2 is
  512 KB–2 MB, cache line is 64 bytes.
- An operator touching a batch usually needs *several* buffers alive at once:
  the input column(s), a selection vector (`uint32_t[2048]` = 8 KB — an index
  into which rows in the batch survived a filter, so we don't compact/copy
  data just to drop rows), possibly a null bitmap, and an output buffer.
- For `int32`/`int64`/`double` columns: 2048 rows = 8 KB / 16 KB per buffer.
  Multiply by 3–4 live buffers in a single operator (source, selection
  vector, output, maybe a hash-table accumulator) and you're at roughly
  32–64 KB — comfortably inside L1 on most cores, so an operator's inner
  loop doesn't evict its own inputs while producing output.
- Too small a vector (e.g. 64 rows) reintroduces per-batch overhead — virtual
  dispatch between operators, function-call overhead, branch misprediction
  at batch boundaries — at nearly the same frequency as tuple-at-a-time
  execution, which defeats the point (Phase 3's whole side-by-side exists to
  show this curve).
- Too large a vector (e.g. 1M rows) blows past L1/L2, forcing every operator
  to stream from L3/DRAM even for values it just produced, and inflates the
  transient memory cost of selection vectors and intermediate buffers.

2048 is the accepted sweet spot industry-wide for this reason, but "the
industry does it" is not a justification I can defend alone — the plan was
to sweep vector size (64 / 256 / 1024 / 2048 / 4096 / 16384) against a
filter+aggregate microbenchmark and plot throughput vs. size, so the
interview answer would be a graph, not a citation.

**Update, after actually running it** (`benchmarks/bench_vector_size_sweep.cpp`,
a representative filter-then-sum-through-a-selection-vector microbenchmark,
the same two-pass shape `FilterOperator::next()` actually uses, run over a
fixed ~4.2M-row total so only batch size varies): the result is more modest
than the framing above implied, and is reported as measured rather than
smoothed into a cleaner story.

| vector_size | items/sec (median of 3) |
|---|---|
| 64 | 245.9M/s |
| 256 | 269.6M/s |
| 1024 | 283.9M/s |
| 2048 | 278.2M/s |
| 4096 | 281.4M/s |
| 16384 | 271.1M/s |
| 65536 | 281.4M/s |

Throughput is flat — within ~5% noise of each other — from 1024 through
65536 on this hardware (Apple Silicon, 4MB L2 per core); there is no sharp
optimum at 2048 specifically, and no visible cache-blowout penalty even at
65536 rows, likely because this core's L2 is large enough to absorb a
65536-entry `uint32_t` selection vector (256KB) for this particular
microbenchmark's working set. The one clear, real effect: **vector_size=64
is ~13-17% slower than everything else** — confirming the qualitative claim
("too small reintroduces per-batch overhead") without sharply validating
2048 as uniquely better than, say, 4096 or 65536. The honest conclusion:
2048 was a reasonable, defensible choice (it's nowhere near the bad end of
the curve), but this specific microbenchmark doesn't prove it's *the*
optimum — a claim this document no longer makes. This is the kind of result
the project's own "measure, don't assert" rule exists for: it would have
been easy to just assert the sweep validated 2048 and move on, and that
would have been false.

---

## 3. Null representation: validity bitmaps

Each column carries a **separate validity bitmap**: 1 bit per row, packed
into `uint8_t` words, alongside (not inside) the dense data array. This is
the Arrow/Parquet approach, chosen over the alternatives:

- **Sentinel values** (e.g. `INT32_MIN` means null): cheapest to store, but
  steals a legitimate value from the domain, requires every consumer of the
  column to know the sentinel, and silently corrupts results if a real value
  ever equals the sentinel. Rejected.
- **`std::optional<T>` per element / nullable wrapper struct**: destroys the
  columnar density guarantee — `optional<int32_t>` is not densely packed,
  isn't SIMD-friendly, and reintroduces per-element branching. Rejected for
  the same reason row layout was rejected.
- **Separate bitmap (chosen)**: the data array stays dense and homogeneously
  typed (SIMD/vectorization untouched), the bitmap is checked separately and
  can often be skipped entirely (a zone map records "null_count == 0" per
  chunk, Phase 1, so whole chunks with no nulls skip validity checks
  altogether). Cost: 1 bit/row = 256 bytes for a 2048-row vector — negligible
  next to a 8 KB int32 column.
- Bitmaps also compose cleanly with selection vectors and with AND/OR
  operations needed for multi-predicate WHERE clauses (validity of `a AND b`
  is just a bitwise AND of two bitmaps) — that composability is the real
  reason this is standard, not just memory economy.

---

## 4. String storage strategy

Two representations, one for the general case and one as a compression layer
on top:

**General varchar column**: an **offsets array + a single contiguous data
buffer** (Arrow-style), not `std::string` per row. `std::vector<std::string>`
per column would mean N independent heap allocations, pointer chasing on
every access, and no ability to mmap the column straight off disk. Instead:
`offsets[i]` and `offsets[i+1]` bound the bytes for row `i` inside one flat
`char` buffer. This keeps the column mmap-able and gives sequential scans a
predictable access pattern.

**Dictionary encoding**: for **low-cardinality** string columns (country
codes, status enums, categories — the common case in analytical data), store
a small array of unique values plus a per-row array of small integer codes
(`uint8_t`/`uint16_t`/`uint32_t` depending on cardinality). This turns string
comparison/grouping into integer comparison/grouping — dramatically cheaper —
and shrinks storage when cardinality is low relative to row count.
**RLE (run-length encoding)** is applied on top of dictionary codes when data
is sorted or naturally clustered (e.g. a `country` column sorted by region):
instead of storing 2048 repeated codes, store (value, run_length) pairs.

Both are optimizations with a cost (dictionary lookup indirection, RLE
breaking O(1) random access), so Phase 1 explicitly benchmarks: plain varchar
vs. dictionary-encoded vs. dictionary+RLE, on both a low-cardinality and a
high-cardinality synthetic column, measuring scan throughput, filter
throughput, and bytes-on-disk. The engine should be able to say *when*
dictionary encoding stops paying for itself (high cardinality), not just that
it usually helps.

`[TODO: benchmark]` Dictionary vs. RLE vs. plain, Phase 1.

---

## 5. Execution model: batched pull (vectorized Volcano), not push, not tuple-at-a-time

Three real options existed here:

1. **Classic Volcano / iterator model, tuple-at-a-time**: every operator
   exposes `next()` returning one row; a virtual call per row per operator.
   Simple, composable, and exactly what's cheap to reason about — and
   exactly what this whole project exists to show is slow. Rejected as the
   *production* model, but deliberately **kept as a reference implementation**
   for one operator (Phase 3) purely to produce the side-by-side number.
2. **Push-based / compiled execution** (Hyper/Impala-style): the query
   compiles to a tight loop (or generates code, or fuses operators via
   templates) that pushes a tuple through the whole pipeline while it's still
   in a register, minimizing materialization between operators. This gets
   the best cache/branch behavior — but requires either JIT/codegen or heavy
   compile-time template metaprogramming, which is a much bigger, differently
   -shaped project than "understand vectorized execution." Rejected primarily
   for scope: it would bury the storage/vectorization lessons this project is
   for under a code-generation project.
3. **Batched pull / vectorized Volcano (MonetDB/X100, DuckDB)** — **chosen**:
   same `next()`-style iterator composition as classic Volcano (so the
   operator interface, plan tree, and mental model stay simple and
   debuggable), but `next()` returns a **batch of up to 2048 rows** instead
   of one row. The virtual-call and branch overhead that killed
   tuple-at-a-time is now amortized over 2048 rows instead of paid per row,
   while each operator's inner loop over the batch is a tight, type-specific,
   auto-vectorizable loop.

**What this gives up** relative to full push/compiled execution: some
intermediate materialization still happens at operator boundaries (a batch
gets fully produced by `scan`, then fully consumed by `filter`, rather than
one value flowing through the whole pipeline fused into one loop), so cache
locality is good but not optimal, and there's no cross-operator fusion. That
gap is explicitly what's *not* being built, and it's the honest answer to
"why didn't you make it even faster" in an interview: push/compiled
execution is the next order of magnitude, at the cost of a code generator.

**Selection vectors, not materialized filtering**: when `filter` drops rows,
it does not copy surviving values into a new dense buffer. It produces a
`uint32_t` array of surviving row indices (the selection vector) that
downstream operators use to index into the original batch. This avoids a
copy per filter and lets a chain of filters compose by intersecting
selection vectors before anything is materialized. Cost: consumers must be
selection-vector-aware (indirect indexing instead of a flat loop) unless a
batch has no selection vector applied (the common/fast path), which is
itself a branch worth benchmarking in Phase 3.

---

## 6. Chunk / on-disk format (sketched here, finalized in Phase 1)

Not implementing yet — flagging the decisions Phase 1 has to make so they're
visible before code exists:

- Per-chunk header: type tag, row count, null count, min/max (zone map),
  encoding (plain/dictionary/RLE), byte length, and alignment padding.
- File footer holds chunk offsets + schema, written last, so a reader mmaps
  the file, seeks to the footer, and knows exactly where every column chunk
  lives without a separate catalog file.
- Alignment: data buffers will be aligned to at least their element size
  (4/8 bytes) and ideally to a cache line (64 bytes) so mmap'd reads can be
  reinterpret_cast to typed spans without unaligned-access penalties (or UB
  on strict-alignment architectures) — exact alignment target and its
  measured effect is a Phase 1 deliverable, not asserted here.
- Endianness: fixed to little-endian on disk (matches x86_64/ARM64 native
  order for every platform this will actually run on) with an explicit
  format-version + endianness byte in the footer, so the format *documents*
  the assumption instead of silently breaking on a big-endian reader. No
  byte-swapping code will be written for a platform this project doesn't
  target — that tradeoff gets named explicitly rather than hidden.

---

## What's deliberately out of scope

- Concurrency control / MVCC / transactions — this is a read-mostly
  analytical engine, not an OLTP store.
- Distributed execution — single process, single machine.
- A cost-based optimizer that rivals a real system's — Phase 4 builds
  cardinality estimation and join reordering from column statistics, but
  it's a teaching-grade optimizer, not Calcite.
- Full SQL surface — Phase 2's grammar is deliberately the subset listed in
  the brief (no subqueries, no window functions, no CTEs) so the parser and
  planner stay hand-writable and understandable end to end.

---

## Summary table (the interview cheat sheet)

| Decision | Chosen | Rejected alternative(s) | Core reason |
|---|---|---|---|
| Layout | Columnar | Row-oriented | Scans touch fewer bytes, vectorize, compress |
| Batch unit | Fixed 2048-row vectors | Tuple-at-a-time / unbounded batches | Amortize overhead while staying in L1/L2 |
| Nulls | Separate validity bitmap | Sentinel values, `optional<T>` | Keeps data array dense + SIMD-able |
| Strings | Offset+buffer, dict-encoded for low cardinality | `vector<string>` per row | mmap-able, avoids per-row heap alloc |
| Execution model | Batched pull (vectorized Volcano) | Tuple-at-a-time Volcano, push/compiled | Simple operator model + amortized overhead, without a codegen project |
| Filtering | Selection vectors | Materialize filtered copies | Avoids copies, composes across chained filters |

Every row in this table gets a number attached to it by Phase 6. If a
decision above turns out to be wrong once measured, this document gets
updated and the reversal gets written up — that's more valuable in an
interview than having been right the first time.
