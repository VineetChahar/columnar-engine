# sqlite3 is linked ONLY into these two comparison/data-generation tools --
# never into columnar_core -- per the project's "no dependencies except
# Catch2/GoogleTest/Google Benchmark" rule for the engine itself. SQLite
# here plays the role of an external baseline to measure against, the same
# way a compiler project might diff its output against a reference
# compiler without that reference becoming part of the compiler.
find_library(SQLITE3_LIBRARY sqlite3 REQUIRED)

add_executable(generate_data generate_data.cpp)
target_link_libraries(generate_data PRIVATE columnar_core ${SQLITE3_LIBRARY})

add_executable(bench_vs_sqlite bench_vs_sqlite.cpp)
target_link_libraries(bench_vs_sqlite PRIVATE columnar_core ${SQLITE3_LIBRARY})

add_executable(profile_join profile_join.cpp)
target_link_libraries(profile_join PRIVATE columnar_core)
