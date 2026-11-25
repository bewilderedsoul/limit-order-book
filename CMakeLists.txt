cmake_minimum_required(VERSION 3.16)
project(limit_order_book LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(LOB_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
option(LOB_TSAN "Build with ThreadSanitizer" OFF)

find_package(Threads REQUIRED)

add_library(lob INTERFACE)
target_include_directories(lob INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(lob INTERFACE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)

if(LOB_SANITIZE)
  target_compile_options(lob INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(lob INTERFACE -fsanitize=address,undefined)
endif()
if(LOB_TSAN)
  target_compile_options(lob INTERFACE -fsanitize=thread)
  target_link_options(lob INTERFACE -fsanitize=thread)
endif()

add_executable(lob_tests tests/test_main.cpp)
target_link_libraries(lob_tests PRIVATE lob Threads::Threads)

add_executable(lob_bench bench/bench.cpp)
target_link_libraries(lob_bench PRIVATE lob Threads::Threads)

add_executable(lob_replay apps/replay.cpp)
target_link_libraries(lob_replay PRIVATE lob)

enable_testing()
add_test(NAME lob_tests COMMAND lob_tests)
