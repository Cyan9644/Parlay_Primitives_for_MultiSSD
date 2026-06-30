CXX      := g++
CXXFLAGS := -std=c++17 -O2 -Wall -fno-omit-frame-pointer
INCLUDES := -I. -Ideps/parlaylib -Ideps/abseil-cpp/install/include
LDFLAGS  := -luring -lpthread

BINDIR := bazel-bin
$(shell mkdir -p $(BINDIR))

# Detect Nix environment and add liburing include/lib paths
ifdef NIX_CFLAGS_COMPILE
  INCLUDES += $(patsubst -isystem%,-I%,$(filter -isystem%,$(NIX_CFLAGS_COMPILE)))
endif
ifdef NIX_LDFLAGS
  LDFLAGS  += $(filter -L%,$(NIX_LDFLAGS))
endif

ABSL_LIBDIR := $(firstword $(wildcard deps/abseil-cpp/install/lib deps/abseil-cpp/install/lib64))
ABSL_LIBS   := $(shell find $(ABSL_LIBDIR) -name '*.a' 2>/dev/null | sort)

UTIL_SRCS := utils/logger.cpp utils/command_line.cpp \
             utils/file_utils.cpp utils/random_number_generator.cpp
UTIL_OBJS := $(UTIL_SRCS:.cpp=.o)

BENCH_SRCS := benchmarks/io_benchmarks.cpp \
              benchmarks/distribution_benchmarks.cpp \
              benchmarks/in_memory_benchmarks.cpp
BENCH_OBJS := $(BENCH_SRCS:.cpp=.o)

BINARIES := $(BINDIR)/speed_test $(BINDIR)/io_uring_test $(BINDIR)/sample_sort \
            $(BINDIR)/permutation $(BINDIR)/sequence  $(BINDIR)/plaidlaymain $(BINDIR)/externalSeqMain \
            $(BINDIR)/scanVerify $(BINDIR)/primesBench $(BINDIR)/primesScaling \
            $(BINDIR)/find_if_benchmark

# Headers under Plaidlay/ are included by quote ("ExternalBoolean.h" etc.); add
# -IPlaidlay so the examples can be compiled from outside that directory.
PLAIDLAY_INC := -IPlaidlay

LINK = $(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@ $(LDFLAGS) -Wl,--start-group $(ABSL_LIBS) -Wl,--end-group

.PHONY: all clean distclean deps

all:
	$(MAKE) deps
	$(MAKE) $(BINARIES)

# ── dependency fetching ────────────────────────────────────────────────────────

deps: deps/parlaylib deps/abseil-cpp/install

deps/parlaylib:
	mkdir -p deps
	git clone https://github.com/ParAlg/parlaylib.git deps/parlaylib-full
	cd deps/parlaylib-full && git checkout 6b4a4cdbfeb3c481608a42db0230eb6ebb87bf8d
	mv deps/parlaylib-full/include deps/parlaylib
	rm -rf deps/parlaylib-full

deps/abseil-cpp/install:
	mkdir -p deps
	git clone --depth 1 --branch 20240722.1 \
	    https://github.com/abseil/abseil-cpp.git deps/abseil-cpp
	rm -rf deps/abseil-cpp/.git
	cd deps/abseil-cpp && cmake -S . -B build \
	    -DCMAKE_BUILD_TYPE=Release \
	    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
	    -DABSL_BUILD_TESTING=OFF \
	    -DABSL_ENABLE_INSTALL=ON \
	    -DBUILD_SHARED_LIBS=OFF \
	    -DABSL_PROPAGATE_CXX_STD=ON \
	    -DCMAKE_CXX_STANDARD=17 \
	    -DCMAKE_INSTALL_PREFIX=$(CURDIR)/deps/abseil-cpp/install && \
	    cmake --build build -j$$(nproc) && \
	    cmake --install build

# ── compilation rules ──────────────────────────────────────────────────────────

%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# ── binaries ───────────────────────────────────────────────────────────────────

$(BINDIR)/speed_test: speed-test.cpp $(UTIL_OBJS) $(BENCH_OBJS)
	$(LINK)

$(BINDIR)/io_uring_test: io_uring_test.cpp $(UTIL_OBJS)
	$(LINK)

$(BINDIR)/sample_sort: sample-sort.cpp $(UTIL_OBJS)
	$(LINK)

$(BINDIR)/permutation: permutation.cpp $(UTIL_OBJS)
	$(LINK)

$(BINDIR)/sequence: sequence.cpp $(UTIL_OBJS)
	$(LINK)

$(BINDIR)/plaidlaymain: Plaidlay/plaidlay_main.cpp $(UTIL_OBJS)
	$(LINK)

$(BINDIR)/externalSeqMain: Plaidlay/external_main.cpp $(UTIL_OBJS)
	$(LINK)

$(BINDIR)/scanVerify: Plaidlay/scan_verify.cpp $(UTIL_OBJS)
	$(LINK)

$(BINDIR)/primesBench: Plaidlay/examples/external/primes_benchmark.cpp $(UTIL_OBJS)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(PLAIDLAY_INC) $^ -o $@ $(LDFLAGS) -Wl,--start-group $(ABSL_LIBS) -Wl,--end-group

$(BINDIR)/primesScaling: Plaidlay/examples/external/primes_scaling_benchmark.cpp $(UTIL_OBJS)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(PLAIDLAY_INC) $^ -o $@ $(LDFLAGS) -Wl,--start-group $(ABSL_LIBS) -Wl,--end-group

$(BINDIR)/find_if_benchmark: Plaidlay/examples/external/find_if_benchmark.cpp $(UTIL_OBJS)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(PLAIDLAY_INC) $^ -o $@ $(LDFLAGS) -Wl,--start-group $(ABSL_LIBS) -Wl,--end-group

# ── cleanup ────────────────────────────────────────────────────────────────────

clean:
	rm -f $(UTIL_OBJS) $(BENCH_OBJS) $(BINARIES)

distclean: clean
	rm -rf deps $(BINDIR)
