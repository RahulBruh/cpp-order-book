cmake_minimum_required(VERSION 3.20)
project(cpp_order_book LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(LOB_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
option(LOB_WERROR "Treat warnings as errors" OFF)
option(LOB_BUILD_TESTS "Build the test suite" ON)

# Header-only library.
add_library(lob INTERFACE)
target_include_directories(lob INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_library(lob_warnings INTERFACE)
if(MSVC)
  target_compile_options(lob_warnings INTERFACE /W4 $<$<BOOL:${LOB_WERROR}>:/WX>)
else()
  target_compile_options(lob_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion
                                                $<$<BOOL:${LOB_WERROR}>:-Werror>)
endif()

if(LOB_SANITIZE AND NOT MSVC)
  add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

add_executable(lob_bench bench/bench.cpp)
target_link_libraries(lob_bench PRIVATE lob lob_warnings)

if(LOB_BUILD_TESTS)
  enable_testing()
  # Prefer a system GoogleTest; otherwise fetch a pinned release.
  find_package(GTest QUIET)
  if(NOT GTest_FOUND)
    include(FetchContent)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_Declare(googletest
      URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz)
    FetchContent_MakeAvailable(googletest)
  endif()
  include(GoogleTest)

  add_executable(lob_tests
    tests/test_matching.cpp
    tests/test_differential.cpp
    tests/test_flat_id_map.cpp)
  target_link_libraries(lob_tests PRIVATE lob lob_warnings GTest::gtest_main)
  gtest_discover_tests(lob_tests DISCOVERY_TIMEOUT 60)

  # Separate binary: it replaces global operator new to count allocations.
  # Skipped under sanitizers, which install their own allocator.
  if(NOT LOB_SANITIZE)
    add_executable(lob_alloc_test tests/test_no_alloc.cpp)
    target_link_libraries(lob_alloc_test PRIVATE lob lob_warnings GTest::gtest_main)
    gtest_discover_tests(lob_alloc_test DISCOVERY_TIMEOUT 60)
  endif()
endif()
