CXX ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic -O2
CPPFLAGS ?= -Isrc

SRCS := src/main.cpp src/lexer/lexer.cpp src/lexer/token.cpp
HEADERS := src/lexer/lexer.hpp src/lexer/token.hpp src/common/diagnostic.hpp

.PHONY: all clean test

all: compiler

compiler: $(SRCS) $(HEADERS)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ $(SRCS)

test: compiler
	bash tests/run_tests.sh

clean:
	rm -f compiler tests/lexer_locations_test
	rm -rf build
