add_library(columnar_core STATIC
  column_chunk.cpp
  file_format.cpp
  lexer.cpp
  parser.cpp
  expression.cpp
  logical_plan.cpp
  operators.cpp
  optimizer.cpp
)

target_include_directories(columnar_core PUBLIC
  ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(columnar_core PUBLIC columnar_warnings columnar_sanitizers)
