add_executable(columnar_tests
  test_aligned_buffer.cpp
  test_bitmap.cpp
  test_zone_map.cpp
  test_column_chunk.cpp
  test_file_format.cpp
  test_engine.cpp
  test_stubs.cpp
)

target_link_libraries(columnar_tests PRIVATE columnar_core Catch2::Catch2WithMain)

include(${catch2_SOURCE_DIR}/extras/Catch.cmake)
catch_discover_tests(columnar_tests)
