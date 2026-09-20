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

## Quiz

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
