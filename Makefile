CXX := clang++
CPPFLAGS := -Iinclude
CXXFLAGS := -std=c++20 -O2 -g -Wall -Wextra -Wpedantic -Wconversion -Wshadow -pthread
LDFLAGS := -pthread

.PHONY: all test clean sanitize thread-sanitize benchmark
all: build/strata

build:
	mkdir -p build

build/strata: src/engine.cpp src/server.cpp include/strata.hpp scripts/build_metadata.py | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) src/engine.cpp src/server.cpp $(LDFLAGS) -o $@
	python3 scripts/build_metadata.py --binary $@ --compiler '$(CXX)' --flags='$(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS)' --source src/engine.cpp --source src/server.cpp --source include/strata.hpp

build/core-test: src/engine.cpp tests/core_test.cpp include/strata.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) src/engine.cpp tests/core_test.cpp $(LDFLAGS) -o $@

build/strata-bench: src/engine.cpp src/benchmark.cpp include/strata.hpp scripts/build_metadata.py | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) src/engine.cpp src/benchmark.cpp $(LDFLAGS) -o $@
	python3 scripts/build_metadata.py --binary $@ --compiler '$(CXX)' --flags='$(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS)' --source src/engine.cpp --source src/benchmark.cpp --source include/strata.hpp

test: all build/core-test
	./build/core-test
	python3 -m unittest discover -s tests -v

benchmark: all build/strata-bench
	python3 scripts/benchmark.py --output evidence/benchmark.json

sanitize: | build
	$(CXX) $(CPPFLAGS) -std=c++20 -O1 -g -Wall -Wextra -Wpedantic -pthread -fsanitize=address,undefined -fno-omit-frame-pointer src/engine.cpp src/server.cpp -o build/strata-asan
	$(CXX) $(CPPFLAGS) -std=c++20 -O1 -g -Wall -Wextra -Wpedantic -pthread -fsanitize=address,undefined -fno-omit-frame-pointer src/engine.cpp tests/core_test.cpp -o build/core-test-asan
	./build/core-test-asan
	STRATA_BIN='$(CURDIR)/build/strata-asan' python3 -m unittest discover -s tests -v

thread-sanitize: | build
	$(CXX) $(CPPFLAGS) -std=c++20 -O1 -g -Wall -Wextra -Wpedantic -pthread -fsanitize=thread src/engine.cpp src/server.cpp -o build/strata-tsan
	STRATA_BIN='$(CURDIR)/build/strata-tsan' python3 -m unittest discover -s tests -v

clean:
	rm -rf build
