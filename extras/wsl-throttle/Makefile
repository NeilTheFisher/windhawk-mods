CXX ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter
# Static libstdc++/libgcc keeps the binary self-contained on any glibc distro
# without pulling in a static libc.
LDFLAGS ?= -static-libstdc++ -static-libgcc

LIB_SOURCES = policy.cpp docker.cpp json.cpp sys.cpp
HEADERS = policy.hpp docker.hpp json.hpp sys.hpp

all: wsl-throttle-agent

wsl-throttle-agent: agent.cpp $(LIB_SOURCES) $(HEADERS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ agent.cpp $(LIB_SOURCES)

tests/run_tests: tests/run_tests.cpp $(LIB_SOURCES) $(HEADERS)
	$(CXX) $(CXXFLAGS) -I. -o $@ tests/run_tests.cpp $(LIB_SOURCES)

test: tests/run_tests
	./tests/run_tests

clean:
	rm -f wsl-throttle-agent tests/run_tests

.PHONY: all test clean
