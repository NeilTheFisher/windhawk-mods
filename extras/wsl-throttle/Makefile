CXX ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter
# Static libstdc++/libgcc keeps the binary self-contained on any glibc distro
# without pulling in a static libc.
LDFLAGS ?= -static-libstdc++ -static-libgcc

all: wsl-throttle-agent

wsl-throttle-agent: agent.cpp docker.cpp json.cpp sys.cpp docker.hpp json.hpp sys.hpp
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ agent.cpp docker.cpp json.cpp sys.cpp

clean:
	rm -f wsl-throttle-agent

.PHONY: all clean
