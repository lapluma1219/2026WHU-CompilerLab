CXX ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic -O2
CPPFLAGS ?= -Isrc

SRCS := src/main.cpp src/lexer/lexer.cpp src/lexer/token.cpp src/parser/parser.cpp \
        src/sema/sema.cpp src/codegen/codegen.cpp src/codegen/optimize.cpp
HEADERS := src/lexer/lexer.hpp src/lexer/token.hpp src/common/diagnostic.hpp \
           src/ast/ast.hpp src/parser/parser.hpp src/sema/sema.hpp src/codegen/codegen.hpp

.PHONY: all clean test

all: compiler

compiler: $(SRCS) $(HEADERS)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ $(SRCS)

test: compiler
	bash tests/run_tests.sh

clean:
	rm -f compiler tests/lexer_locations_test tests/ast_shape_test
	rm -rf build
