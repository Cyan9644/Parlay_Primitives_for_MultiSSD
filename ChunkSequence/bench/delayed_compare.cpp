// Fusion benchmark: eager vs block-delayed ChunkSequence pipelines.
//
// The eager primitives round-trip every intermediate through the SSDs, so a
// map|reduce chain reads the input, writes a full intermediate, then reads it
// back.  The delayed path fuses the chain into a single read pass — no
// intermediate ever touches disk.  This times two pipelines at increasing chain
// length to show the I/O-fusion win:
//
//   map|reduce       eager: 2 reads + 1 write (3n)   delayed: 1 read (n)
//   map|map|reduce   eager: 3 reads + 2 writes (5n)  delayed: 1 read (n)
//
// Only the operation is timed; perm(n) is generated outside the timed region.
// Eager intermediates are written under bw_dl_* and cleaned up after each step.

#include <iostream>
#include <iomanip>
#include <cstdint>
#include <chrono>
#include <functional>
#include <string>
#include <unistd.h>

#include "parlay/primitives.h"

#include "utils/command_line.h"
#include "utils/file_utils.h"
#include "ChunkSequence/chunk_seq.h"
#include "ChunkSequence/chunk_map.h"
#include "ChunkSequence/chunk_reduce.h"
#include "ChunkSequence/chunk_delayed.h"

namespace cd = ChunkSequenceOps::delayed;

struct SumMonoid {
    uint64_t identity = 0;
    uint64_t operator()(uint64_t a, uint64_t b) const { return a + b; }
};

using Clock = std::chrono::steady_clock;
static double elapsed(Clock::time_point t0) {
    return std::chrono::duration<double>(Clock::now() - t0).count();
}

static size_t chunk_seq_bytes(const chunk_seq& seq) {
    size_t total = 0;
    for (const auto& c : seq.chunks) total += c.used;
    return total;
}
static double to_gb(size_t bytes) { return (double)bytes / (1024.0 * 1024.0 * 1024.0); }

static void cleanup_prefix(const std::string& prefix) {
    const auto& ssds = GetSSDList();
    for (size_t d = 0; d < ssds.size(); d++) unlink(GetFileName(prefix, d).c_str());
}

// time/throughput row; "io_factor" is bytes moved relative to the input volume.
static void print_row(const std::string& label, size_t in_bytes, double io_factor,
                      double secs) {
    std::cout << "  " << std::left << std::setw(22) << label
              << std::right << std::fixed << std::setprecision(3)
              << std::setw(8) << secs << "s"
              << "   ~" << std::setprecision(1) << io_factor << "x I/O"
              << "   " << std::setprecision(2)
              << to_gb((size_t)(in_bytes * io_factor)) / secs << " GB/s\n";
}

int main(int argc, char* argv[]) {
    ParseGlobalArguments(argc, argv);
    const size_t n = (argc > 1) ? std::stoull(argv[1]) : 5'000'000ULL;

    auto add1 = [](uint64_t x) { return x + 1; };
    auto mul2 = [](uint64_t x) { return 2 * x; };
    const std::function<uint64_t(uint64_t)> add1f = add1;
    const std::function<uint64_t(uint64_t)> mul2f = mul2;

    std::cout << "Generating chunk_seq perm(" << n << ")...\n" << std::flush;
    const chunk_seq cseq = ChunkSequenceOps::perm(n);
    const size_t in_bytes = chunk_seq_bytes(cseq);
    std::cout << "  " << cseq.chunks.size() << " chunks, "
              << std::fixed << std::setprecision(3) << to_gb(in_bytes) << " GB\n\n";

    double eager_mr_s, delayed_mr_s, eager_mmr_s, delayed_mmr_s;

    // ── map | reduce ────────────────────────────────────────────────────────────
    std::cout << "--- map(x+1) | reduce(sum) ---\n";
    {
        auto t0 = Clock::now();
        chunk_seq m = ChunkSequenceOps::ChunkMap<uint64_t>(cseq, "bw_dl_m", add1f);
        volatile uint64_t r = ChunkSequenceOps::ChunkReduce<uint64_t>(m, SumMonoid{});
        eager_mr_s = elapsed(t0);
        (void)r;
        print_row("eager", in_bytes, 3.0, eager_mr_s);
        cleanup_prefix("bw_dl_m");
    }
    {
        auto t0 = Clock::now();
        volatile uint64_t r = cd::reduce(cd::map(cd::delay(cseq), add1), SumMonoid{});
        delayed_mr_s = elapsed(t0);
        (void)r;
        print_row("delayed", in_bytes, 1.0, delayed_mr_s);
    }

    // ── map | map | reduce ──────────────────────────────────────────────────────
    std::cout << "\n--- map(x+1) | map(2x) | reduce(sum) ---\n";
    {
        auto t0 = Clock::now();
        chunk_seq m1 = ChunkSequenceOps::ChunkMap<uint64_t>(cseq, "bw_dl_m1", add1f);
        chunk_seq m2 = ChunkSequenceOps::ChunkMap<uint64_t>(m1, "bw_dl_m2", mul2f);
        volatile uint64_t r = ChunkSequenceOps::ChunkReduce<uint64_t>(m2, SumMonoid{});
        eager_mmr_s = elapsed(t0);
        (void)r;
        print_row("eager", in_bytes, 5.0, eager_mmr_s);
        cleanup_prefix("bw_dl_m1"); cleanup_prefix("bw_dl_m2");
    }
    {
        auto t0 = Clock::now();
        volatile uint64_t r =
            cd::reduce(cd::map(cd::map(cd::delay(cseq), add1), mul2), SumMonoid{});
        delayed_mmr_s = elapsed(t0);
        (void)r;
        print_row("delayed", in_bytes, 1.0, delayed_mmr_s);
    }

    std::cout << "\n(throughput = bytes actually moved / wall time; the delayed "
                 "path moves only the input once)\n";

    // CSV for a scaling driver.  Columns: n,eager_mr,delayed_mr,eager_mmr,delayed_mmr
    std::cout << "CSV," << n << ',' << std::setprecision(9)
              << eager_mr_s << ',' << delayed_mr_s << ','
              << eager_mmr_s << ',' << delayed_mmr_s << '\n';
    return 0;
}
