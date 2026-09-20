// Benchmarks backing DESIGN.md section 4's [TODO: benchmark] markers:
// dictionary encoding and dictionary+RLE vs. plain storage, across
// cardinality, for both construction cost and the operation they're
// actually meant to speed up (an equality scan -- the core of `WHERE col =
// 'x'`). Run with: ./columnar_bench_encoding --benchmark_counters_tabular=true

#include <algorithm>
#include <cstdint>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <vector>

#include <benchmark/benchmark.h>

#include "columnar/column_chunk.hpp"

using namespace columnar;

namespace {

constexpr std::size_t kRows = kVectorSize;  // one execution vector's worth: 2048

std::vector<std::string> make_data(std::size_t rows, std::size_t cardinality, bool sorted) {
  std::vector<std::string> out(rows);
  std::mt19937 rng(42);
  std::uniform_int_distribution<std::size_t> dist(0, cardinality - 1);
  for (std::size_t i = 0; i < rows; ++i) {
    out[i] = "val_" + std::to_string(dist(rng));
  }
  if (sorted) {
    std::sort(out.begin(), out.end());
  }
  return out;
}

std::vector<std::string_view> as_views(const std::vector<std::string>& data) {
  std::vector<std::string_view> views;
  views.reserve(data.size());
  for (const std::string& s : data) {
    views.push_back(s);
  }
  return views;
}

// std::vector<bool> is bit-packed, not a real contiguous bool array, so it
// can't bind to std::span<const bool>. A unique_ptr<bool[]> gets a real
// array without a raw new/delete in this file.
std::unique_ptr<bool[]> make_all_valid(std::size_t n) {
  auto mask = std::make_unique<bool[]>(n);
  std::fill_n(mask.get(), n, true);
  return mask;
}

}  // namespace

// ---------------------------------------------------------------- build --

static void BM_BuildVarcharPlain(benchmark::State& state) {
  const auto cardinality = static_cast<std::size_t>(state.range(0));
  auto data = make_data(kRows, cardinality, /*sorted=*/false);
  auto views = as_views(data);
  auto valid = make_all_valid(kRows);
  const std::span<const bool> validity(valid.get(), kRows);

  for (auto _ : state) {
    VarcharColumnChunk chunk(views, validity);
    benchmark::DoNotOptimize(chunk);
  }
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * kRows));
}
BENCHMARK(BM_BuildVarcharPlain)->Arg(8)->Arg(64)->Arg(512)->Arg(2048);

static void BM_BuildDictionary(benchmark::State& state) {
  const auto cardinality = static_cast<std::size_t>(state.range(0));
  auto data = make_data(kRows, cardinality, /*sorted=*/false);
  auto views = as_views(data);
  auto valid = make_all_valid(kRows);
  const std::span<const bool> validity(valid.get(), kRows);

  for (auto _ : state) {
    DictionaryColumnChunk chunk(views, validity);
    benchmark::DoNotOptimize(chunk);
  }
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * kRows));
}
BENCHMARK(BM_BuildDictionary)->Arg(8)->Arg(64)->Arg(512)->Arg(2048);

static void BM_BuildDictionaryRleSorted(benchmark::State& state) {
  const auto cardinality = static_cast<std::size_t>(state.range(0));
  auto data = make_data(kRows, cardinality, /*sorted=*/true);
  auto views = as_views(data);
  auto valid = make_all_valid(kRows);
  const std::span<const bool> validity(valid.get(), kRows);

  for (auto _ : state) {
    DictionaryRleColumnChunk chunk(views, validity);
    benchmark::DoNotOptimize(chunk);
  }
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * kRows));
}
BENCHMARK(BM_BuildDictionaryRleSorted)->Arg(8)->Arg(64)->Arg(512)->Arg(2048);

static void BM_BuildDictionaryRleUnsorted(benchmark::State& state) {
  // Same cardinalities, shuffled instead of sorted: run_count() should
  // approach row_count() here, showing RLE buys nothing (and costs a run
  // header per row) when data isn't sorted/clustered -- DESIGN.md's caveat
  // on when RLE helps, made visible as a number instead of an assertion.
  const auto cardinality = static_cast<std::size_t>(state.range(0));
  auto data = make_data(kRows, cardinality, /*sorted=*/false);
  auto views = as_views(data);
  auto valid = make_all_valid(kRows);
  const std::span<const bool> validity(valid.get(), kRows);

  for (auto _ : state) {
    DictionaryRleColumnChunk chunk(views, validity);
    benchmark::DoNotOptimize(chunk);
  }
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * kRows));
}
BENCHMARK(BM_BuildDictionaryRleUnsorted)->Arg(8)->Arg(64)->Arg(512)->Arg(2048);

// -------------------------------------------------- equality scan (WHERE) --
//
// Simulates the core of `WHERE country = 'val_0'`: count matching rows.
// This is the operation dictionary/RLE encoding is supposed to speed up --
// turning string comparisons into integer comparisons (dictionary), or
// skipping whole runs at once without touching individual rows (RLE).

static void BM_ScanEqualityPlain(benchmark::State& state) {
  const auto cardinality = static_cast<std::size_t>(state.range(0));
  auto data = make_data(kRows, cardinality, /*sorted=*/false);
  auto views = as_views(data);
  auto valid = make_all_valid(kRows);
  VarcharColumnChunk chunk(views, std::span<const bool>(valid.get(), kRows));
  const std::string target = "val_0";

  for (auto _ : state) {
    std::size_t count = 0;
    for (std::size_t i = 0; i < chunk.row_count(); ++i) {
      if (!chunk.is_null(i) && chunk.value(i) == target) {
        ++count;
      }
    }
    benchmark::DoNotOptimize(count);
  }
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * kRows));
}
BENCHMARK(BM_ScanEqualityPlain)->Arg(8)->Arg(2048);

static void BM_ScanEqualityDictionary(benchmark::State& state) {
  const auto cardinality = static_cast<std::size_t>(state.range(0));
  auto data = make_data(kRows, cardinality, /*sorted=*/false);
  auto views = as_views(data);
  auto valid = make_all_valid(kRows);
  DictionaryColumnChunk chunk(views, std::span<const bool>(valid.get(), kRows));
  const std::string target = "val_0";

  for (auto _ : state) {
    // Resolve the constant to a dictionary code once (what a real optimizer
    // does at plan time), then compare integers per row instead of strings.
    std::uint32_t target_code = static_cast<std::uint32_t>(chunk.dictionary_size());
    for (std::uint32_t code = 0; code < chunk.dictionary_size(); ++code) {
      if (chunk.dictionary_entry(code) == target) {
        target_code = code;
        break;
      }
    }
    std::size_t count = 0;
    for (std::uint32_t code : chunk.codes()) {
      count += (code == target_code) ? 1 : 0;
    }
    benchmark::DoNotOptimize(count);
  }
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * kRows));
}
BENCHMARK(BM_ScanEqualityDictionary)->Arg(8)->Arg(2048);

static void BM_ScanEqualityDictionaryRleSorted(benchmark::State& state) {
  const auto cardinality = static_cast<std::size_t>(state.range(0));
  auto data = make_data(kRows, cardinality, /*sorted=*/true);
  auto views = as_views(data);
  auto valid = make_all_valid(kRows);
  DictionaryRleColumnChunk chunk(views, std::span<const bool>(valid.get(), kRows));
  const std::string target = "val_0";

  for (auto _ : state) {
    std::size_t count = 0;
    for (const RleRun& run : chunk.runs()) {
      if (!run.is_null && chunk.dictionary_entry(run.code) == target) {
        count += run.length;
      }
    }
    benchmark::DoNotOptimize(count);
  }
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * kRows));
}
BENCHMARK(BM_ScanEqualityDictionaryRleSorted)->Arg(8)->Arg(2048);

BENCHMARK_MAIN();
