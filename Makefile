# Convenience build without CMake.
CXX      ?= g++
CXXFLAGS ?= -std=c++20 -O2 -march=native -Wall -Wextra -Wpedantic -Wconversion -Wshadow
INC      := -Iinclude
HDRS     := $(wildcard include/lob/*.hpp) $(wildcard tests/*.hpp) bench/workload.hpp

all: build/lob_tests build/lob_bench build/lob_replay

build/lob_tests: tests/test_main.cpp $(HDRS) | build
	$(CXX) $(CXXFLAGS) $(INC) $< -o $@ -pthread

build/lob_bench: bench/bench.cpp $(HDRS) | build
	$(CXX) $(CXXFLAGS) $(INC) $< -o $@ -pthread

build/lob_replay: apps/replay.cpp $(HDRS) | build
	$(CXX) $(CXXFLAGS) $(INC) $< -o $@

build:
	mkdir -p build

test: build/lob_tests
	./build/lob_tests

bench: build/lob_bench
	./build/lob_bench

demo: build/lob_replay
	./build/lob_replay examples/session.txt

clean:
	rm -rf build

.PHONY: all test bench demo clean
