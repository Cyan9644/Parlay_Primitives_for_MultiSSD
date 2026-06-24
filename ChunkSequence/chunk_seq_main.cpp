#include <iostream>
#include <chrono>

#include "absl/log/log.h"
#include "absl/log/initialize.h"

#include "utils/command_line.h"
#include "utils/file_utils.h"
#include "ChunkSequence/chunk_seq.h"
#include "ChunkSequence/chunk_reduce.h"

struct AddMonoid {
    uint64_t identity = 0;
    uint64_t operator()(uint64_t a, uint64_t b) const { return a + b; }
};

// static double now() {
//     using namespace std::chrono;
//     return duration<double>(high_resolution_clock::now().time_since_epoch()).count();
// }

int main(int argc, char* argv[]) {
    absl::InitializeLog();
    ParseGlobalArguments(argc, argv);

    chunk_seq seq = ChunkSequenceOps::tabulate(1'00'000'000, "deez", [](size_t i) { return i * 8; });
    seq.consolidate("deadbeef");
    /*
    const size_t n = (argc > 1) ? std::stoull(argv[1]) : 1'000'000'00ULL;
    const double data_gb = (double)(n * sizeof(uint64_t)) / 1e9;

    // ── perm ─────────────────────────────────────────────────────────────────
    double t0 = now();
    chunk_seq seq = ChunkSequenceOps::perm(n);
    double perm_s = now() - t0;

    std::cout << "perm:   n=" << n
              << "  chunks=" << seq.chunks.size()
              << "  drives=" << GetSSDList().size()
              << "  time=" << perm_s << "s"
              << "  throughput=" << data_gb / perm_s << " GB/s\n";

    // ── reduce (sum) to verify correctness ───────────────────────────────────
    double t1 = now();
    const uint64_t sum = ChunkReduce<uint64_t>(seq, AddMonoid{});
    double reduce_s = now() - t1;

    const uint64_t expected = (uint64_t)n * ((uint64_t)n - 1) / 2;
    const bool correct = (sum == expected);

    std::cout << "reduce: sum=" << sum
              << "  expected=" << expected
              << "  " << (correct ? "OK" : "WRONG")
              << "  time=" << reduce_s << "s"
              << "  throughput=" << data_gb / reduce_s << " GB/s\n";

    return correct ? 0 : 1;
    */
}
