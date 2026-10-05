cmake_minimum_required(VERSION 3.28)
project(OrderBook LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(LOB_NATIVE "Tune for the build machine (-march=native / /arch:AVX2)" ON)
option(LOB_BUILD_TESTS "Build unit tests" ON)

include(FetchContent)

# zlib: use the system copy when there is one (Linux), otherwise build it from source (Windows).
find_package(ZLIB QUIET)
if(NOT ZLIB_FOUND)
  message(STATUS "System zlib not found; fetching zlib 1.3.1")
  set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(zlib
    URL https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz
    URL_HASH SHA256=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    EXCLUDE_FROM_ALL)
  FetchContent_MakeAvailable(zlib)
  target_include_directories(zlibstatic INTERFACE ${zlib_SOURCE_DIR} ${zlib_BINARY_DIR})
  add_library(ZLIB::ZLIB ALIAS zlibstatic)
endif()

# Warnings and tuning flags shared by every target. Release optimization (/O2, -O3) comes from CMake.
add_library(lob_options INTERFACE)
if(MSVC)
  target_compile_options(lob_options INTERFACE /W4 /permissive- /Zc:__cplusplus /EHsc)
  target_compile_definitions(lob_options INTERFACE _CRT_SECURE_NO_WARNINGS NOMINMAX)
  if(LOB_NATIVE)
    target_compile_options(lob_options INTERFACE /arch:AVX2)
  endif()
else()
  target_compile_options(lob_options INTERFACE -Wall -Wextra -Wpedantic)
  if(LOB_NATIVE)
    target_compile_options(lob_options INTERFACE -march=native)
  endif()
endif()

add_library(lob
  cpp/src/gz_reader.cpp
  cpp/src/book.cpp)
target_include_directories(lob PUBLIC cpp/include)
target_link_libraries(lob PUBLIC lob_options PRIVATE ZLIB::ZLIB)

foreach(app itch_stats itch_dump itch_filter build_book)
  add_executable(${app} cpp/apps/${app}.cpp)
  target_link_libraries(${app} PRIVATE lob)
endforeach()
target_link_libraries(itch_filter PRIVATE ZLIB::ZLIB)

add_executable(lob_bench cpp/bench/lob_bench.cpp)
target_link_libraries(lob_bench PRIVATE lob)

if(LOB_BUILD_TESTS)
  enable_testing()
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/releases/download/v1.15.2/googletest-1.15.2.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(googletest)

  add_executable(lob_tests cpp/tests/test_gz_reader.cpp cpp/tests/test_itch.cpp cpp/tests/test_book.cpp
  cpp/tests/test_fast_book.cpp)
  target_link_libraries(lob_tests PRIVATE lob ZLIB::ZLIB GTest::gtest_main)
  target_compile_definitions(lob_tests PRIVATE
    LOB_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
  include(GoogleTest)
  gtest_discover_tests(lob_tests)

  # Cross-language checks on the fixture: the C++ and Python decoders must agree on every
  # field, and the two order books on every top-of-book change.
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_FOUND)
    set(fixture ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/sample.itch.gz)
    set(cross_check ${CMAKE_CURRENT_SOURCE_DIR}/scripts/cross_check.py)
    add_test(NAME cross_check_decoders
      COMMAND Python3::Interpreter ${cross_check} dump $<TARGET_FILE:itch_dump> ${fixture})
    add_test(NAME cross_check_books
      COMMAND Python3::Interpreter ${cross_check} book $<TARGET_FILE:build_book> ${fixture})
  endif()
endif()
