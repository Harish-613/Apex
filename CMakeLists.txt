cmake_minimum_required(VERSION 3.20)
project(apex LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(APEX_NATIVE "Tune for the build machine's CPU (-march=native)" OFF)
option(APEX_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)

find_package(Threads REQUIRED)

# Header-only library.
add_library(apex INTERFACE)
target_include_directories(apex INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(apex INTERFACE Threads::Threads)

add_library(apex_warnings INTERFACE)
target_compile_options(apex_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion)
if(APEX_NATIVE)
  target_compile_options(apex_warnings INTERFACE -march=native)
endif()
if(APEX_SANITIZE)
  target_compile_options(apex_warnings INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(apex_warnings INTERFACE -fsanitize=address,undefined)
endif()

enable_testing()
add_executable(apex_tests
  tests/test_main.cpp
  tests/test_components.cpp
  tests/test_order_book.cpp
  tests/test_differential.cpp)
target_link_libraries(apex_tests PRIVATE apex apex_warnings)
add_test(NAME apex_tests COMMAND apex_tests)

add_executable(bench_book bench/bench_book.cpp)
target_link_libraries(bench_book PRIVATE apex apex_warnings)

add_executable(bench_pipeline bench/bench_pipeline.cpp)
target_link_libraries(bench_pipeline PRIVATE apex apex_warnings)

add_executable(apex_cli apps/apex_cli.cpp)
target_link_libraries(apex_cli PRIVATE apex apex_warnings)
