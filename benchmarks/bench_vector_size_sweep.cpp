// DESIGN.md section 2 promised a vector-size sweep (64/256/1024/2048/4096/
// 16384) against a filter+aggregate microbenchmark, left as `[TODO:
// benchmark]`. It was never implemented -- caught in a later review, not
// resolved along the way. This closes that gap for real instead of just
// softening the wording: a representative filter+aggregate microbenchmark
// (build a selection vector, then sum through it -- the same two-pass
// shape FilterOperator::next() actually uses), run over a fixed total row
// count, varying only the batch size each pass operates on.
//
// kVectorSize itself is a compile-time constant threaded through the whole
// engine (storage chunk capacity, ExecBatch sizing, ...), so this does not
// recompile the engine at each size; it isolates the one variable the
// design decision actually depends on -- how much work one batch-processing
// call does before paying its next per-batch dispatch/allocation cost --
// without an invasive engine-wide refactor to make kVectorSize runtime-
// configurable.

#include <cstdint>
#include <random>
#include <vector>

#include <benchmark/benchmark.h>

namespace {

// Large enough that per-batch overhead dominates for tiny batches and cache
// effects show up for huge ones, small enough the whole sweep runs in
// seconds, not minutes.
constexpr std::size_t kTotalRows = std::size_t{1} << 22;  // ~4.2M rows

std::vector<std::int64_t> make_data(std::size_t n) {
  std::vector<std::int64_t> v(n);
  std::mt19937_64 rng(1);
  std::uniform_int_distribution<int> dist(0, 999);
  for (auto& x : v) x = dist(rng);
  return v;
}

// Deliberately not inlined: a real Operator::next() call is an actual
// function call (virtual, in fact) once per batch, and this benchmark
// means to charge that cost once per batch too, not let the optimizer fuse
// the whole sweep into one loop regardless of `vector_size`.
//
// Mirrors FilterOperator::next()'s actual shape: build a selection vector
// sized to the batch (a fresh allocation, matching what the real operator
// does -- it does not reuse a buffer across calls), then sum through it.
__attribute__((noinline)) std::int64_t process_batch(const std::int64_t* data, std::size_t n) {
  std::vector<std::uint32_t> selection;
  selection.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    if (data[i] > 500) {
      selection.push_back(static_cast<std::uint32_t>(i));
    }
  }
  std::int64_t total = 0;
  for (std::uint32_t idx : selection) {
    total += data[idx];
  }
  return total;
}

}  // namespace

static void BM_FilterAggregateByVectorSize(benchmark::State& state) {
  const std::size_t vector_size = static_cast<std::size_t>(state.range(0));
  const std::vector<std::int64_t> data = make_data(kTotalRows);

  for (auto _ : state) {
    std::int64_t total = 0;
    for (std::size_t start = 0; start < data.size(); start += vector_size) {
      const std::size_t end = std::min(start + vector_size, data.size());
      total += process_batch(data.data() + start, end - start);
    }
    benchmark::DoNotOptimize(total);
  }
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * kTotalRows));
  state.SetLabel("vector_size=" + std::to_string(vector_size));
}
BENCHMARK(BM_FilterAggregateByVectorSize)
    ->Arg(64)->Arg(256)->Arg(1024)->Arg(2048)->Arg(4096)->Arg(16384)->Arg(65536);

BENCHMARK_MAIN();
