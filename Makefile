CXX = g++
CXXFLAGS = -std=c++20 -O3 -march=native -Wall -Wextra -pedantic -mbmi2
OMPFLAGS = -fopenmp

SRCS = board.cpp movegen.cpp notation.cpp tablebase.cpp
OBJS = $(SRCS:.cpp=.o)

all: damas test generate lookup verify

damas: $(OBJS) main.o
	$(CXX) $(CXXFLAGS) -o $@ $^

test: $(OBJS) test.o
	$(CXX) $(CXXFLAGS) -o $@ $^

generate: $(OBJS) generate.o
	$(CXX) $(CXXFLAGS) $(OMPFLAGS) -o $@ $^

generate.o: generate.cpp
	$(CXX) $(CXXFLAGS) $(OMPFLAGS) -c -o $@ $<

lookup: $(OBJS) lookup.o
	$(CXX) $(CXXFLAGS) -o $@ $^

verify: $(OBJS) verify.o
	$(CXX) $(CXXFLAGS) -o $@ $^

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f *.o damas test generate lookup verify

# Dependencies
board.o: board.cpp board.h
movegen.o: movegen.cpp movegen.h board.h
notation.o: notation.cpp notation.h movegen.h board.h
tablebase.o: tablebase.cpp tablebase.h board.h
main.o: main.cpp board.h movegen.h notation.h
test.o: test.cpp board.h movegen.h notation.h tablebase.h
generate.o: generate.cpp tablebase.h board.h movegen.h
lookup.o: lookup.cpp tablebase.h board.h
verify.o: verify.cpp tablebase.h board.h movegen.h

.PHONY: all clean
