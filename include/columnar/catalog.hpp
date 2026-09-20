#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "columnar/types.hpp"

namespace columnar {

struct CatalogColumn {
  std::string name;
  TypeId type;
};

struct CatalogTable {
  std::string name;
  std::vector<CatalogColumn> columns;

  std::optional<std::size_t> column_index(std::string_view col_name) const {
    for (std::size_t i = 0; i < columns.size(); ++i) {
      if (columns[i].name == col_name) return i;
    }
    return std::nullopt;
  }
};

// The schema half of what a real system splits into catalog + storage: maps
// table names to column schemas so the parser/binder (Phase 2) can resolve
// identifiers before a single row is read, and report "no such column" at
// parse time with the offending token's position.
class Catalog {
 public:
  void add_table(CatalogTable table) {
    tables_.emplace(table.name, std::move(table));
  }

  const CatalogTable* find_table(std::string_view name) const {
    auto it = tables_.find(std::string(name));
    return it == tables_.end() ? nullptr : &it->second;
  }

 private:
  std::unordered_map<std::string, CatalogTable> tables_;
};

}  // namespace columnar
