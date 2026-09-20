#include "columnar/operators.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

#include "columnar/optimizer.hpp"

namespace columnar {

namespace {

constexpr std::uint32_t kNullRow = std::numeric_limits<std::uint32_t>::max();

ExecColumn gather_with_nulls(const ExecColumn& src, const std::vector<std::uint32_t>& indices) {
  ExecColumn out = ExecColumn::make(src.type, indices.size());
  for (std::size_t i = 0; i < indices.size(); ++i) {
    const std::uint32_t idx = indices[i];
    if (idx == kNullRow) {
      out.validity[i] = 0;
      continue;
    }
    out.validity[i] = src.validity[idx];
    switch (src.type) {
      case ExecType::kInt64: out.ints()[i] = src.ints()[idx]; break;
      case ExecType::kDouble: out.doubles()[i] = src.doubles()[idx]; break;
      case ExecType::kBool: out.bools()[i] = src.bools()[idx]; break;
      case ExecType::kText: out.texts()[i] = src.texts()[idx]; break;
    }
  }
  return out;
}

void append_column(ExecColumn& dest, const ExecColumn& src) {
  dest.validity.insert(dest.validity.end(), src.validity.begin(), src.validity.end());
  switch (dest.type) {
    case ExecType::kInt64:
      dest.ints().insert(dest.ints().end(), src.ints().begin(), src.ints().end());
      break;
    case ExecType::kDouble:
      dest.doubles().insert(dest.doubles().end(), src.doubles().begin(), src.doubles().end());
      break;
    case ExecType::kBool:
      dest.bools().insert(dest.bools().end(), src.bools().begin(), src.bools().end());
      break;
    case ExecType::kText:
      dest.texts().insert(dest.texts().end(), src.texts().begin(), src.texts().end());
      break;
  }
}

ExecBatch materialize_all(Operator& op) {
  ExecBatch full;
  bool initialized = false;
  while (std::optional<ExecBatch> batch = op.next()) {
    if (!initialized) {
      full.column_names = batch->column_names;
      full.columns.resize(batch->columns.size());
      for (std::size_t i = 0; i < batch->columns.size(); ++i) {
        full.columns[i].type = batch->columns[i].type;
        full.columns[i].data = ExecColumn::make(batch->columns[i].type, 0).data;
      }
      initialized = true;
    }
    for (std::size_t i = 0; i < batch->columns.size(); ++i) {
      append_column(full.columns[i], batch->columns[i]);
    }
  }
  return full;
}

std::vector<ExecBatch> split_into_batches(const ExecBatch& full, std::size_t vector_size) {
  std::vector<ExecBatch> out;
  const std::size_t n = full.row_count();
  for (std::size_t start = 0; start < n; start += vector_size) {
    const std::size_t end = std::min(start + vector_size, n);
    std::vector<std::uint32_t> idx(end - start);
    for (std::size_t i = start; i < end; ++i) idx[i - start] = static_cast<std::uint32_t>(i);
    ExecBatch batch;
    batch.column_names = full.column_names;
    batch.columns.reserve(full.columns.size());
    for (const ExecColumn& col : full.columns) batch.columns.push_back(gather_with_nulls(col, idx));
    out.push_back(std::move(batch));
  }
  return out;
}

std::string stringify_key(const ExecColumn& col, std::size_t row) {
  if (!col.validity[row]) return "\x01NULL";
  switch (col.type) {
    case ExecType::kInt64: return "i" + std::to_string(col.ints()[row]);
    case ExecType::kDouble: return "d" + std::to_string(col.doubles()[row]);
    case ExecType::kBool: return col.bools()[row] ? "b1" : "b0";
    case ExecType::kText: return "s" + col.texts()[row];
  }
  return "";
}

}  // namespace

// -------------------------------------------------------------- decode --

ExecColumn decode_chunk(const ColumnChunkVariant& chunk) {
  return std::visit(
      [](const auto& c) -> ExecColumn {
        using ChunkT = std::decay_t<decltype(c)>;
        const std::size_t n = c.row_count();

        if constexpr (std::is_same_v<ChunkT, PlainColumnChunk<std::int32_t>>) {
          ExecColumn out = ExecColumn::make(ExecType::kInt64, n);
          for (std::size_t i = 0; i < n; ++i) {
            out.validity[i] = c.is_null(i) ? 0 : 1;
            out.ints()[i] = c.value(i);
          }
          return out;
        } else if constexpr (std::is_same_v<ChunkT, PlainColumnChunk<std::int64_t>>) {
          ExecColumn out = ExecColumn::make(ExecType::kInt64, n);
          for (std::size_t i = 0; i < n; ++i) {
            out.validity[i] = c.is_null(i) ? 0 : 1;
            out.ints()[i] = c.value(i);
          }
          return out;
        } else if constexpr (std::is_same_v<ChunkT, PlainColumnChunk<double>>) {
          ExecColumn out = ExecColumn::make(ExecType::kDouble, n);
          for (std::size_t i = 0; i < n; ++i) {
            out.validity[i] = c.is_null(i) ? 0 : 1;
            out.doubles()[i] = c.value(i);
          }
          return out;
        } else if constexpr (std::is_same_v<ChunkT, BoolColumnChunk>) {
          ExecColumn out = ExecColumn::make(ExecType::kBool, n);
          for (std::size_t i = 0; i < n; ++i) {
            out.validity[i] = c.is_null(i) ? 0 : 1;
            out.bools()[i] = c.value(i) ? 1 : 0;
          }
          return out;
        } else if constexpr (std::is_same_v<ChunkT, VarcharColumnChunk>) {
          ExecColumn out = ExecColumn::make(ExecType::kText, n);
          for (std::size_t i = 0; i < n; ++i) {
            out.validity[i] = c.is_null(i) ? 0 : 1;
            if (out.validity[i]) out.texts()[i] = std::string(c.value(i));
          }
          return out;
        } else if constexpr (std::is_same_v<ChunkT, DictionaryColumnChunk>) {
          ExecColumn out = ExecColumn::make(ExecType::kText, n);
          for (std::size_t i = 0; i < n; ++i) {
            out.validity[i] = c.is_null(i) ? 0 : 1;
            if (out.validity[i]) out.texts()[i] = std::string(c.value(i));
          }
          return out;
        } else if constexpr (std::is_same_v<ChunkT, DictionaryRleColumnChunk>) {
          ExecColumn out = ExecColumn::make(ExecType::kText, n);
          std::size_t row = 0;
          for (const RleRun& run : c.runs()) {
            for (std::uint32_t k = 0; k < run.length; ++k, ++row) {
              out.validity[row] = run.is_null ? 0 : 1;
              if (!run.is_null) out.texts()[row] = std::string(c.dictionary_entry(run.code));
            }
          }
          return out;
        } else {
          static_assert(!sizeof(ChunkT*), "unhandled ColumnChunkVariant alternative");
        }
      },
      chunk);
}

// --------------------------------------------------------- zone map prune --

namespace {

template <typename T>
bool numeric_zone_permits(const ZoneMap<T>& zm, BinaryOp op, T value) {
  if (!zm.has_values) return false;  // all rows null -> never matches a value predicate
  switch (op) {
    case BinaryOp::kEq: return value >= zm.min && value <= zm.max;
    case BinaryOp::kLt: return zm.min < value;
    case BinaryOp::kLe: return zm.min <= value;
    case BinaryOp::kGt: return zm.max > value;
    case BinaryOp::kGe: return zm.max >= value;
    default: return true;  // kNe and anything else: not provably prunable
  }
}

bool string_zone_permits(const StringZoneMap& zm, BinaryOp op, const std::string& value) {
  if (!zm.has_values) return false;
  switch (op) {
    case BinaryOp::kEq: return value >= zm.min && value <= zm.max;
    case BinaryOp::kLt: return zm.min < value;
    case BinaryOp::kLe: return zm.min <= value;
    case BinaryOp::kGt: return zm.max > value;
    case BinaryOp::kGe: return zm.max >= value;
    default: return true;
  }
}

}  // namespace

bool zone_map_may_match(const ColumnChunkVariant& chunk, const ScanPredicate& pred) {
  return std::visit(
      [&](const auto& c) -> bool {
        using ChunkT = std::decay_t<decltype(c)>;
        if constexpr (std::is_same_v<ChunkT, PlainColumnChunk<std::int32_t>> ||
                      std::is_same_v<ChunkT, PlainColumnChunk<std::int64_t>>) {
          if (pred.type != ExecType::kInt64) return true;
          return numeric_zone_permits(c.zone_map(), pred.op,
                                       static_cast<decltype(c.zone_map().min)>(pred.int_value));
        } else if constexpr (std::is_same_v<ChunkT, PlainColumnChunk<double>>) {
          if (pred.type != ExecType::kDouble) return true;
          return numeric_zone_permits(c.zone_map(), pred.op, pred.double_value);
        } else if constexpr (std::is_same_v<ChunkT, VarcharColumnChunk> ||
                              std::is_same_v<ChunkT, DictionaryColumnChunk> ||
                              std::is_same_v<ChunkT, DictionaryRleColumnChunk>) {
          if (pred.type != ExecType::kText) return true;
          return string_zone_permits(c.zone_map(), pred.op, pred.text_value);
        } else {
          return true;  // bool zone maps: not worth pruning on (cardinality 2)
        }
      },
      chunk);
}

// -------------------------------------------------------------- Scan --

ScanOperator::ScanOperator(const Table& table, std::vector<std::size_t> column_indices,
                            std::vector<std::string> column_names,
                            std::vector<ScanPredicate> predicates,
                            std::vector<bool> decode_mask, ScanStats* stats)
    : table_(table),
      column_indices_(std::move(column_indices)),
      column_names_(std::move(column_names)),
      predicates_(std::move(predicates)),
      decode_mask_(std::move(decode_mask)),
      stats_(stats) {
  if (decode_mask_.empty()) decode_mask_.assign(column_indices_.size(), true);
}

namespace {
ExecType widen_type_id(TypeId t) {
  switch (t) {
    case TypeId::kInt32:
    case TypeId::kInt64: return ExecType::kInt64;
    case TypeId::kDouble: return ExecType::kDouble;
    case TypeId::kBool: return ExecType::kBool;
    case TypeId::kVarchar: return ExecType::kText;
  }
  return ExecType::kInt64;
}
}  // namespace

std::optional<ExecBatch> ScanOperator::next() {
  if (column_indices_.empty()) return std::nullopt;
  const std::size_t chunk_count = table_.columns[column_indices_[0]].chunks.size();

  while (next_chunk_ < chunk_count) {
    const std::size_t ci = next_chunk_++;
    if (stats_) ++stats_->total_chunks;

    bool prunable = false;
    for (const ScanPredicate& pred : predicates_) {
      const ColumnChunkVariant& col_chunk = table_.columns[column_indices_[pred.column_index]].chunks[ci];
      if (!zone_map_may_match(col_chunk, pred)) {
        prunable = true;
        break;
      }
    }
    if (prunable) {
      if (stats_) ++stats_->pruned_chunks;
      continue;
    }

    ExecBatch batch;
    batch.column_names = column_names_;
    batch.columns.reserve(column_indices_.size());
    for (std::size_t i = 0; i < column_indices_.size(); ++i) {
      const ColumnChunkVariant& col_chunk = table_.columns[column_indices_[i]].chunks[ci];
      if (decode_mask_[i]) {
        batch.columns.push_back(decode_chunk(col_chunk));
      } else {
        // Projection pushdown: nothing downstream references this column,
        // so skip decoding it entirely -- just keep positional alignment
        // with an empty (all-invalid) placeholder of the right width.
        ExecColumn placeholder = ExecColumn::make(widen_type_id(type_id(col_chunk)), row_count(col_chunk));
        std::fill(placeholder.validity.begin(), placeholder.validity.end(), 0);
        batch.columns.push_back(std::move(placeholder));
      }
    }
    if (stats_) stats_->rows_produced += batch.row_count();
    return batch;
  }
  return std::nullopt;
}

// ------------------------------------------------------------- Filter --

FilterOperator::FilterOperator(OperatorPtr input, const Expression& predicate)
    : input_(std::move(input)), predicate_(predicate) {}

std::optional<ExecBatch> FilterOperator::next() {
  while (std::optional<ExecBatch> batch = input_->next()) {
    ExecColumn mask = predicate_.evaluate(*batch, /*selection=*/nullptr);
    std::vector<std::uint32_t> selection;
    selection.reserve(batch->row_count());
    for (std::size_t i = 0; i < batch->row_count(); ++i) {
      if (mask.validity[i] && mask.bools()[i]) selection.push_back(static_cast<std::uint32_t>(i));
    }
    if (selection.empty()) continue;

    ExecBatch out;
    out.column_names = batch->column_names;
    out.columns.reserve(batch->columns.size());
    for (const ExecColumn& col : batch->columns) out.columns.push_back(gather_with_nulls(col, selection));
    return out;
  }
  return std::nullopt;
}

// ------------------------------------------------------------ Project --

ProjectOperator::ProjectOperator(OperatorPtr input, const std::vector<ExpressionPtr>& exprs,
                                  std::vector<std::string> names)
    : input_(std::move(input)), exprs_(exprs), names_(std::move(names)) {}

std::optional<ExecBatch> ProjectOperator::next() {
  std::optional<ExecBatch> batch = input_->next();
  if (!batch) return std::nullopt;
  ExecBatch out;
  out.column_names = names_;
  out.columns.reserve(exprs_.size());
  for (const ExpressionPtr& e : exprs_) out.columns.push_back(e->evaluate(*batch, nullptr));
  return out;
}

// -------------------------------------------------------- HashAggregate --

namespace {

struct AggAccumulator {
  AggFunc func;
  ExecType type;
  std::int64_t count = 0;
  double sum = 0.0;
  bool has_value = false;
  std::int64_t min_i = 0, max_i = 0;
  double min_d = 0.0, max_d = 0.0;
  std::string min_s, max_s;

  void add_int(std::int64_t v) {
    ++count;
    sum += static_cast<double>(v);
    if (!has_value) {
      min_i = max_i = v;
      has_value = true;
    } else {
      min_i = std::min(min_i, v);
      max_i = std::max(max_i, v);
    }
  }
  void add_double(double v) {
    ++count;
    sum += v;
    if (!has_value) {
      min_d = max_d = v;
      has_value = true;
    } else {
      min_d = std::min(min_d, v);
      max_d = std::max(max_d, v);
    }
  }
  void add_text(const std::string& v) {
    ++count;
    if (!has_value) {
      min_s = max_s = v;
      has_value = true;
    } else {
      min_s = std::min(min_s, v);
      max_s = std::max(max_s, v);
    }
  }
};

ExecType aggregate_output_type(const AggregateItem& agg) {
  switch (agg.func) {
    case AggFunc::kCount:
    case AggFunc::kCountStar: return ExecType::kInt64;
    case AggFunc::kAvg: return ExecType::kDouble;
    case AggFunc::kSum:
    case AggFunc::kMin:
    case AggFunc::kMax: return agg.arg ? agg.arg->type() : ExecType::kInt64;
  }
  return ExecType::kInt64;
}

}  // namespace

HashAggregateOperator::HashAggregateOperator(OperatorPtr input,
                                              const std::vector<ExpressionPtr>& group_by,
                                              const std::vector<AggregateItem>& aggregates,
                                              std::vector<std::string> output_names)
    : input_(std::move(input)),
      group_by_(group_by),
      aggregates_(aggregates),
      output_names_(std::move(output_names)) {}

void HashAggregateOperator::compute() {
  ExecBatch all = materialize_all(*input_);
  const std::size_t n = all.row_count();

  std::vector<ExecColumn> group_cols;
  group_cols.reserve(group_by_.size());
  for (const ExpressionPtr& g : group_by_) group_cols.push_back(g->evaluate(all, nullptr));

  std::vector<ExecColumn> arg_cols;
  arg_cols.reserve(aggregates_.size());
  for (const AggregateItem& agg : aggregates_) {
    arg_cols.push_back(agg.arg ? agg.arg->evaluate(all, nullptr) : ExecColumn::make(ExecType::kInt64, n));
  }

  std::unordered_map<std::string, std::size_t> key_to_group;
  std::vector<std::vector<std::uint32_t>> group_key_rows;  // one representative row per group, per key col
  std::vector<std::vector<AggAccumulator>> group_accumulators;

  for (std::size_t row = 0; row < n; ++row) {
    std::string key;
    for (const ExecColumn& gc : group_cols) key += stringify_key(gc, row) + "\x02";

    auto it = key_to_group.find(key);
    std::size_t gidx;
    if (it == key_to_group.end()) {
      gidx = group_key_rows.size();
      key_to_group.emplace(key, gidx);
      group_key_rows.push_back(std::vector<std::uint32_t>{static_cast<std::uint32_t>(row)});
      std::vector<AggAccumulator> accs;
      accs.reserve(aggregates_.size());
      for (const AggregateItem& agg : aggregates_) {
        AggAccumulator acc;
        acc.func = agg.func;
        acc.type = agg.arg ? agg.arg->type() : ExecType::kInt64;
        accs.push_back(acc);
      }
      group_accumulators.push_back(std::move(accs));
    } else {
      gidx = it->second;
    }

    for (std::size_t a = 0; a < aggregates_.size(); ++a) {
      const AggregateItem& agg = aggregates_[a];
      AggAccumulator& acc = group_accumulators[gidx][a];
      if (agg.func == AggFunc::kCountStar) {
        ++acc.count;
        continue;
      }
      const ExecColumn& arg = arg_cols[a];
      if (!arg.validity[row]) continue;  // COUNT/SUM/AVG/MIN/MAX ignore nulls
      if (arg.type == ExecType::kInt64) acc.add_int(arg.ints()[row]);
      else if (arg.type == ExecType::kDouble) acc.add_double(arg.doubles()[row]);
      else acc.add_text(arg.texts()[row]);
    }
  }

  const std::size_t num_groups = group_key_rows.size();
  ExecBatch full;
  full.column_names = output_names_;

  std::size_t out_col = 0;
  // Emit in the order the caller's output_names_ imply: group-by columns
  // first only if referenced -- but to keep this simple and correct, the
  // binder always orders SELECT items as written, so we rebuild columns in
  // that exact order using a small index: group columns interleave with
  // aggregate columns according to how build_physical_plan wired them.
  // Here we just emit group columns then aggregate columns, and
  // build_physical_plan is responsible for matching output_names_ order
  // (see its LogicalAggregate handling) -- this operator always produces
  // [group_by..., aggregates...] internally.
  (void)out_col;

  for (std::size_t g = 0; g < group_cols.size(); ++g) {
    ExecColumn col = ExecColumn::make(group_cols[g].type, num_groups);
    for (std::size_t gi = 0; gi < num_groups; ++gi) {
      const std::uint32_t rep_row = group_key_rows[gi][0];
      col.validity[gi] = group_cols[g].validity[rep_row];
      switch (col.type) {
        case ExecType::kInt64: col.ints()[gi] = group_cols[g].ints()[rep_row]; break;
        case ExecType::kDouble: col.doubles()[gi] = group_cols[g].doubles()[rep_row]; break;
        case ExecType::kBool: col.bools()[gi] = group_cols[g].bools()[rep_row]; break;
        case ExecType::kText: col.texts()[gi] = group_cols[g].texts()[rep_row]; break;
      }
    }
    full.columns.push_back(std::move(col));
  }

  for (std::size_t a = 0; a < aggregates_.size(); ++a) {
    const AggregateItem& agg = aggregates_[a];
    const ExecType result_type = aggregate_output_type(agg);
    ExecColumn col = ExecColumn::make(result_type, num_groups);
    for (std::size_t gi = 0; gi < num_groups; ++gi) {
      const AggAccumulator& acc = group_accumulators[gi][a];
      switch (agg.func) {
        case AggFunc::kCount:
        case AggFunc::kCountStar:
          col.ints()[gi] = acc.count;
          break;
        case AggFunc::kSum:
          if (result_type == ExecType::kDouble) col.doubles()[gi] = acc.sum;
          else col.ints()[gi] = static_cast<std::int64_t>(std::llround(acc.sum));
          col.validity[gi] = acc.has_value ? 1 : 0;
          break;
        case AggFunc::kAvg:
          col.doubles()[gi] = acc.count > 0 ? acc.sum / static_cast<double>(acc.count) : 0.0;
          col.validity[gi] = acc.count > 0 ? 1 : 0;
          break;
        case AggFunc::kMin:
        case AggFunc::kMax: {
          col.validity[gi] = acc.has_value ? 1 : 0;
          const bool want_min = agg.func == AggFunc::kMin;
          if (acc.type == ExecType::kInt64) col.ints()[gi] = want_min ? acc.min_i : acc.max_i;
          else if (acc.type == ExecType::kDouble) col.doubles()[gi] = want_min ? acc.min_d : acc.max_d;
          else col.texts()[gi] = want_min ? acc.min_s : acc.max_s;
          break;
        }
      }
    }
    full.columns.push_back(std::move(col));
  }

  output_batches_ = split_into_batches(full, kVectorSize);
}

std::optional<ExecBatch> HashAggregateOperator::next() {
  if (!computed_) {
    compute();
    computed_ = true;
  }
  if (next_batch_ >= output_batches_.size()) return std::nullopt;
  return std::move(output_batches_[next_batch_++]);
}

// ------------------------------------------------------------ HashJoin --

HashJoinOperator::HashJoinOperator(OperatorPtr left, OperatorPtr right, JoinType type,
                                    std::size_t left_key_index, std::size_t right_key_index,
                                    std::vector<std::string> output_names)
    : left_(std::move(left)),
      right_(std::move(right)),
      type_(type),
      left_key_index_(left_key_index),
      right_key_index_(right_key_index),
      output_names_(std::move(output_names)) {}

void HashJoinOperator::build_right_side() {
  build_batch_ = materialize_all(*right_);
  const ExecColumn& key_col = build_batch_.columns[right_key_index_];
  for (std::size_t row = 0; row < build_batch_.row_count(); ++row) {
    if (!key_col.validity[row]) continue;
    build_index_[stringify_key(key_col, row)].push_back(static_cast<std::uint32_t>(row));
  }
}

std::optional<ExecBatch> HashJoinOperator::next() {
  if (!built_) {
    build_right_side();
    built_ = true;
  }

  while (std::optional<ExecBatch> probe = left_->next()) {
    const ExecColumn& key_col = probe->columns[left_key_index_];
    std::vector<std::uint32_t> left_rows, right_rows;

    for (std::size_t row = 0; row < probe->row_count(); ++row) {
      bool matched = false;
      if (key_col.validity[row]) {
        auto it = build_index_.find(stringify_key(key_col, row));
        if (it != build_index_.end()) {
          for (std::uint32_t br : it->second) {
            left_rows.push_back(static_cast<std::uint32_t>(row));
            right_rows.push_back(br);
          }
          matched = true;
        }
      }
      if (!matched && type_ == JoinType::kLeft) {
        left_rows.push_back(static_cast<std::uint32_t>(row));
        right_rows.push_back(kNullRow);
      }
    }

    if (left_rows.empty()) continue;

    ExecBatch out;
    out.column_names = output_names_;
    out.columns.reserve(probe->columns.size() + build_batch_.columns.size());
    for (const ExecColumn& col : probe->columns) out.columns.push_back(gather_with_nulls(col, left_rows));
    for (const ExecColumn& col : build_batch_.columns) out.columns.push_back(gather_with_nulls(col, right_rows));
    return out;
  }
  return std::nullopt;
}

// --------------------------------------------------------------- Sort --

SortOperator::SortOperator(OperatorPtr input, const std::vector<SortKey>& keys)
    : input_(std::move(input)), keys_(keys) {}

void SortOperator::compute() {
  ExecBatch all = materialize_all(*input_);
  const std::size_t n = all.row_count();

  std::vector<ExecColumn> key_cols;
  key_cols.reserve(keys_.size());
  for (const SortKey& k : keys_) key_cols.push_back(k.expr->evaluate(all, nullptr));

  std::vector<std::uint32_t> perm(n);
  for (std::size_t i = 0; i < n; ++i) perm[i] = static_cast<std::uint32_t>(i);

  std::stable_sort(perm.begin(), perm.end(), [&](std::uint32_t a, std::uint32_t b) {
    for (std::size_t k = 0; k < key_cols.size(); ++k) {
      const ExecColumn& col = key_cols[k];
      const bool a_null = !col.validity[a];
      const bool b_null = !col.validity[b];
      if (a_null != b_null) return a_null && keys_[k].descending;  // nulls first ASC, last DESC-ish (documented, not standard SQL NULLS FIRST/LAST)
      if (a_null && b_null) continue;

      int cmp = 0;
      switch (col.type) {
        case ExecType::kInt64: cmp = (col.ints()[a] < col.ints()[b]) ? -1 : (col.ints()[a] > col.ints()[b] ? 1 : 0); break;
        case ExecType::kDouble: cmp = (col.doubles()[a] < col.doubles()[b]) ? -1 : (col.doubles()[a] > col.doubles()[b] ? 1 : 0); break;
        case ExecType::kBool: cmp = (col.bools()[a] < col.bools()[b]) ? -1 : (col.bools()[a] > col.bools()[b] ? 1 : 0); break;
        case ExecType::kText: cmp = col.texts()[a].compare(col.texts()[b]); break;
      }
      if (cmp != 0) return keys_[k].descending ? cmp > 0 : cmp < 0;
    }
    return false;
  });

  ExecBatch sorted;
  sorted.column_names = all.column_names;
  sorted.columns.reserve(all.columns.size());
  for (const ExecColumn& col : all.columns) sorted.columns.push_back(gather_with_nulls(col, perm));

  output_batches_ = split_into_batches(sorted, kVectorSize);
}

std::optional<ExecBatch> SortOperator::next() {
  if (!computed_) {
    compute();
    computed_ = true;
  }
  if (next_batch_ >= output_batches_.size()) return std::nullopt;
  return std::move(output_batches_[next_batch_++]);
}

// -------------------------------------------------------------- Limit --

LimitOperator::LimitOperator(OperatorPtr input, std::uint64_t limit)
    : input_(std::move(input)), remaining_(limit) {}

std::optional<ExecBatch> LimitOperator::next() {
  if (remaining_ == 0) return std::nullopt;
  std::optional<ExecBatch> batch = input_->next();
  if (!batch) return std::nullopt;

  if (static_cast<std::uint64_t>(batch->row_count()) <= remaining_) {
    remaining_ -= batch->row_count();
    return batch;
  }

  std::vector<std::uint32_t> idx(remaining_);
  for (std::uint64_t i = 0; i < remaining_; ++i) idx[i] = static_cast<std::uint32_t>(i);
  ExecBatch out;
  out.column_names = batch->column_names;
  out.columns.reserve(batch->columns.size());
  for (const ExecColumn& col : batch->columns) out.columns.push_back(gather_with_nulls(col, idx));
  remaining_ = 0;
  return out;
}

// ------------------------------------------------------- physical plan --

namespace {

std::vector<std::string> names_from_schema(const std::vector<OutputColumn>& schema) {
  std::vector<std::string> names;
  names.reserve(schema.size());
  for (const OutputColumn& c : schema) names.push_back(c.name);
  return names;
}

}  // namespace

OperatorPtr build_physical_plan(const LogicalPlan& plan, const Database& db, ScanStats* stats,
                                 ExprArena& arena) {
  switch (plan.node_type()) {
    case LogicalNodeType::kScan: {
      const auto& scan = static_cast<const LogicalScan&>(plan);
      auto it = db.find(scan.table_name());
      if (it == db.end()) throw std::runtime_error("no such table loaded: " + scan.table_name());
      const std::size_t n = scan.output_schema().size();
      std::vector<std::size_t> column_indices(n);
      for (std::size_t i = 0; i < n; ++i) column_indices[i] = i;

      std::vector<bool> decode_mask;
      if (!scan.projected_column_indices.empty()) {
        decode_mask.assign(n, false);
        for (std::size_t idx : scan.projected_column_indices) decode_mask[idx] = true;
      }
      return std::make_unique<ScanOperator>(it->second, std::move(column_indices),
                                             names_from_schema(scan.output_schema()),
                                             scan.zone_map_predicates, std::move(decode_mask),
                                             stats);
    }
    case LogicalNodeType::kFilter: {
      const auto& filter = static_cast<const LogicalFilter&>(plan);
      OperatorPtr input = build_physical_plan(filter.input(), db, stats, arena);
      return std::make_unique<FilterOperator>(std::move(input), filter.predicate());
    }
    case LogicalNodeType::kProject: {
      const auto& project = static_cast<const LogicalProject&>(plan);
      OperatorPtr input = build_physical_plan(project.input(), db, stats, arena);
      return std::make_unique<ProjectOperator>(std::move(input), project.exprs(),
                                                names_from_schema(project.output_schema()));
    }
    case LogicalNodeType::kAggregate: {
      const auto& agg = static_cast<const LogicalAggregate&>(plan);
      OperatorPtr input = build_physical_plan(agg.input(), db, stats, arena);
      return std::make_unique<HashAggregateOperator>(std::move(input), agg.group_by(),
                                                       agg.aggregates(),
                                                       names_from_schema(agg.output_schema()));
    }
    case LogicalNodeType::kJoin: {
      const auto& join = static_cast<const LogicalJoin&>(plan);
      const auto* eq = dynamic_cast<const BinaryExpr*>(&join.condition());
      if (!eq || eq->op() != BinaryOp::kEq) {
        throw std::runtime_error("unsupported join condition: hash join requires a simple "
                                  "equality between two columns");
      }
      const auto* lhs_col = dynamic_cast<const ColumnRefExpr*>(&eq->lhs());
      const auto* rhs_col = dynamic_cast<const ColumnRefExpr*>(&eq->rhs());
      if (!lhs_col || !rhs_col) {
        throw std::runtime_error("unsupported join condition: both sides must be plain columns");
      }
      const std::size_t left_width = join.left().output_schema().size();
      const bool lhs_is_left = lhs_col->column_index() < left_width;
      const std::size_t left_key = lhs_is_left ? lhs_col->column_index() : rhs_col->column_index();
      const std::size_t right_key_global =
          lhs_is_left ? rhs_col->column_index() : lhs_col->column_index();
      const std::size_t right_key = right_key_global - left_width;

      OperatorPtr left_op = build_physical_plan(join.left(), db, stats, arena);
      OperatorPtr right_op = build_physical_plan(join.right(), db, stats, arena);

      std::vector<std::string> names = names_from_schema(join.output_schema());

      // "Join reordering": rather than a full N-way join-order search
      // (out of scope -- see DESIGN.md), the one decision a hash join
      // actually has is which side gets built into the hash table. Build
      // the smaller estimated side; probing the larger side once beats
      // building a hash table over it. For a LEFT join the left side must
      // stay the probe side (its unmatched rows drive the output), so this
      // only applies to INNER joins.
      if (join.type() == JoinType::kInner) {
        const double left_rows = estimate_row_count(join.left(), db);
        const double right_rows = estimate_row_count(join.right(), db);
        if (left_rows < right_rows) {
          // Swap: build on the (smaller) left side by swapping operator
          // roles, then swap columns back into the original left++right
          // output order.
          const std::size_t right_width = join.right().output_schema().size();
          auto swapped = std::make_unique<HashJoinOperator>(
              std::move(right_op), std::move(left_op), JoinType::kInner, right_key, left_key,
              names_from_schema(join.output_schema()));
          // HashJoinOperator emits [probe cols..., build cols...] = [right,
          // left] here; wrap in a Project that reorders to [left, right]
          // and applies the real output names.
          std::vector<ExpressionPtr> reorder;
          reorder.reserve(names.size());
          for (std::size_t i = 0; i < left_width; ++i) {
            reorder.push_back(std::make_unique<ColumnRefExpr>(
                names[i], right_width + i, join.left().output_schema()[i].type));
          }
          for (std::size_t i = 0; i < right_width; ++i) {
            reorder.push_back(std::make_unique<ColumnRefExpr>(
                names[left_width + i], i, join.right().output_schema()[i].type));
          }
          arena.push_back(std::move(reorder));
          return std::make_unique<ProjectOperator>(std::move(swapped), arena.back(), names);
        }
      }

      return std::make_unique<HashJoinOperator>(std::move(left_op), std::move(right_op),
                                                 join.type(), left_key, right_key,
                                                 std::move(names));
    }
    case LogicalNodeType::kSort: {
      const auto& sort = static_cast<const LogicalSort&>(plan);
      OperatorPtr input = build_physical_plan(sort.input(), db, stats, arena);
      return std::make_unique<SortOperator>(std::move(input), sort.keys());
    }
    case LogicalNodeType::kLimit: {
      const auto& limit = static_cast<const LogicalLimit&>(plan);
      OperatorPtr input = build_physical_plan(limit.input(), db, stats, arena);
      return std::make_unique<LimitOperator>(std::move(input), limit.limit());
    }
  }
  throw std::runtime_error("build_physical_plan: unhandled node type");
}

}  // namespace columnar
