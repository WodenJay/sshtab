CXX ?= g++
CPPFLAGS ?= -Isrc
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -pedantic
LDFLAGS ?=

SRC := $(wildcard src/*.cpp)
LIB_SRC := $(filter-out src/main.cpp, $(SRC))
LIB_OBJ := $(LIB_SRC:src/%.cpp=build/%.o)
MAIN_OBJ := build/main.o
OBJ := $(LIB_OBJ) $(MAIN_OBJ)
BIN := sshtab
TEST_SRC := $(wildcard tests/*.cpp)
TEST_OBJ := $(TEST_SRC:tests/%.cpp=build/tests_%.o)
TEST_BIN := sshtab_tests

.PHONY: all clean test

all: $(BIN)

$(BIN): $(OBJ)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(OBJ) -o $@ $(LDFLAGS)

build/%.o: src/%.cpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

build/tests_%.o: tests/%.cpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(TEST_BIN): $(LIB_OBJ) $(TEST_OBJ)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LIB_OBJ) $(TEST_OBJ) -o $@ $(LDFLAGS)

test: $(TEST_BIN)
	./$(TEST_BIN)

build:
	mkdir -p $@

-include $(OBJ:.o=.d) $(TEST_OBJ:.o=.d)

clean:
	rm -rf build $(BIN) $(TEST_BIN)
