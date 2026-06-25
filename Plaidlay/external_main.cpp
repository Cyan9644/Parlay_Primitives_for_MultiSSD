//claude-built, probably should not trust

// Build & run (from the repo root, on a filesystem that supports O_DIRECT):
//
//   g++ -std=c++17 -O2 -I . -I Plaidlay -I Plaidlay/parlaylib/include \
//       -I deps/abseil-cpp/install/include \
//       Plaidlay/external_main.cpp utils/file_utils.cpp utils/logger.cpp \
//       utils/random_number_generator.cpp -o external_main_test \
//       -Wl,--start-group deps/abseil-cpp/install/lib/*.a -Wl,--end-group \
//       -lpthread -luring
//   ./external_main_test
//
// The tests create and delete their own scratch files in the working directory.

#include "externalSeq.h"
#include "externalFilter.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_set>
#include <vector>

auto add = [](size_t a, size_t b) {return a + b;};

void mapThroughput();
void legacyDemo();

// ===========================================================================
//  Correctness and memory tests for ExternalFilter (Plaidlay/externalFilter.h)
//
//  ExternalFilter reads an input External_Sequence (a list of chunk_headers
//  describing on-disk blocks), applies a predicate to every element, and writes
//  the surviving elements back out as a new External_Sequence. Each input block
//  maps 1:1 to an output block that keeps the input block's `index`, so sorting
//  the output by `index` reproduces a stable global order.
//
//  These tests build a known input on disk, run the filter, read the result
//  back, and compare against an in-memory reference filter. A separate test
//  checks that memory does not grow with the number of batches (i.e. the
//  per-batch buffers are actually freed).
// ===========================================================================

namespace {

int g_failures = 0;

void Check(bool cond, const std::string &name) {
    if (cond) {
        std::cout << "  [PASS] " << name << std::endl;
    } else {
        std::cout << "  [FAIL] " << name << std::endl;
        g_failures++;
    }
}

// Current resident set size (KB), from /proc/self/statm.
size_t CurrentRssKb() {
    std::ifstream statm("/proc/self/statm");
    size_t total_pages = 0, resident_pages = 0;
    statm >> total_pages >> resident_pages;
    long page_kb = sysconf(_SC_PAGESIZE) / 1024;
    return resident_pages * (size_t) page_kb;
}

// Peak resident set size (KB) over the process lifetime, from VmHWM.
size_t PeakRssKb() {
    std::ifstream status("/proc/self/status");
    std::string key;
    while (status >> key) {
        if (key == "VmHWM:") {
            size_t value = 0;
            status >> value;  // value is already in kB
            return value;
        }
        std::string rest;
        std::getline(status, rest);
    }
    return 0;
}

// Build an input External_Sequence on disk: one chunk per file. Each file holds
// that chunk's bytes at offset 0, padded up to an O_DIRECT-aligned length so the
// reader's aligned read is fully satisfied. Records every created file in
// `created_files` for later cleanup.
template<typename T>
External_Sequence BuildInput(const std::string &prefix,
                             const std::vector<std::vector<T>> &chunks,
                             std::vector<std::string> &created_files) {
    External_Sequence seq(chunks.size());
    for (size_t i = 0; i < chunks.size(); i++) {
        const std::string fname = prefix + "_in_" + std::to_string(i);
        const size_t used = chunks[i].size() * sizeof(T);
        const size_t padded = AlignUp(used);

        int fd = open(fname.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        CHECK(fd >= 0) << "could not create input file " << fname;
        if (used > 0) {
            ssize_t w = pwrite(fd, chunks[i].data(), used, 0);
            CHECK(w == (ssize_t) used) << "short write building input " << fname;
        }
        // Zero-pad so an O_DIRECT read of AlignUp(used) bytes stays inside the file.
        CHECK(ftruncate(fd, (off_t) padded) == 0) << "ftruncate failed for " << fname;
        close(fd);
        created_files.push_back(fname);

        chunk_header h;
        h.filename = fname;
        h.begin_address = 0;
        h.used = used;
        h.index = i;  // global order key: chunk i contributes the i-th block
        seq.ordered_underlying_sequence[i] = h;
    }
    return seq;
}

// Read an output External_Sequence back into a flat vector, in index order.
template<typename T>
std::vector<T> ReadOutput(const External_Sequence &out) {
    std::vector<chunk_header> headers = out.ordered_underlying_sequence;
    std::sort(headers.begin(), headers.end(),
              [](const chunk_header &a, const chunk_header &b) { return a.index < b.index; });

    std::vector<T> result;
    for (const auto &h : headers) {
        const size_t n = h.used / sizeof(T);
        if (n == 0) {
            continue;  // block produced no surviving elements
        }
        std::vector<T> buf(n);
        int fd = open(h.filename.c_str(), O_RDONLY);
        CHECK(fd >= 0) << "could not open output file " << h.filename;
        ssize_t r = pread(fd, buf.data(), h.used, (off_t) h.begin_address);
        CHECK(r == (ssize_t) h.used) << "short read of output " << h.filename;
        close(fd);
        result.insert(result.end(), buf.begin(), buf.end());
    }
    return result;
}

void RemoveFiles(const std::vector<std::string> &files) {
    for (const auto &f : files) {
        unlink(f.c_str());
    }
}

// The deterministic set of output filenames for a given prefix (no side effects).
std::vector<std::string> OutputNames(const std::string &prefix) {
    std::vector<std::string> names;
    for (int i = 0; i < NUM_SSDS; i++) {
        names.push_back(prefix + "_out_" + std::to_string(i));
    }
    return names;
}

std::vector<std::string> MakeOutputNames(const std::string &prefix) {
    std::vector<std::string> names = OutputNames(prefix);
    // Start fresh so stale blocks from a previous run can't be confused for output.
    RemoveFiles(names);
    return names;
}

constexpr size_t kBufferBytes = 4u << 20;  // ExternalFilter's per-block buffer size

// Default per-chunk length: varied and deliberately not block-aligned, but well
// under the 4 MiB block buffer so each input chunk maps to a single output block.
size_t VariedLen(size_t c) { return 300 + (c * 37) % 900; }
// Many tiny chunks, to stress the per-chunk header bookkeeping.
size_t TinyLen(size_t c) { return 1 + (c * 13) % 17; }

// Generate `num_chunks` chunks of T, numbering elements sequentially across all
// chunks via `make(global_index)`. `len(chunk_index)` sets each chunk's length.
// Also produces the in-memory reference: `predicate` applied to the flattened
// sequence, in order -- which is exactly what sorting the output by `index`
// should reproduce.
template<typename T, typename MakeFn, typename PredFn, typename LenFn>
void MakeChunks(size_t num_chunks, MakeFn make, PredFn predicate, LenFn len,
                std::vector<std::vector<T>> &chunks, std::vector<T> &reference) {
    size_t counter = 0;
    for (size_t c = 0; c < num_chunks; c++) {
        const size_t n = len(c);
        std::vector<T> chunk;
        chunk.reserve(n);
        for (size_t k = 0; k < n; k++) {
            const T v = make(counter++);
            chunk.push_back(v);
            if (predicate(v)) {
                reference.push_back(v);
            }
        }
        chunks.push_back(std::move(chunk));
    }
}

// Run the filter over `chunks` with `predicate` and return the output sequence
// (headers), so callers can both read the data back and inspect structure.
template<typename T>
External_Sequence RunFilter(const std::string &prefix,
                            const std::vector<std::vector<T>> &chunks,
                            const std::function<bool(const T)> &predicate,
                            std::vector<std::string> &all_files) {
    External_Sequence in = BuildInput<T>(prefix, chunks, all_files);
    std::vector<std::string> out_names = MakeOutputNames(prefix);
    all_files.insert(all_files.end(), out_names.begin(), out_names.end());

    return ExternalFilter<T>(in, predicate, out_names);
}

// Structural invariants that must hold for any successful filter, independent of
// the predicate: one output header per input chunk, every header's byte count is
// a whole number of T's and fits in a block buffer, output filenames are drawn
// only from the provided output set, and the surviving input `index` values form
// exactly the set {0, ..., num_input_chunks - 1} (each block preserved once).
template<typename T>
void CheckOutputInvariants(const External_Sequence &out, size_t num_input_chunks,
                           const std::vector<std::string> &out_names,
                           const std::string &name) {
    const std::vector<chunk_header> &hs = out.ordered_underlying_sequence;
    std::unordered_set<std::string> allowed(out_names.begin(), out_names.end());

    bool ok = (hs.size() == num_input_chunks);
    std::vector<size_t> indices;
    indices.reserve(hs.size());
    for (const chunk_header &h : hs) {
        if (h.used % sizeof(T) != 0) ok = false;       // whole elements only
        if (h.used > kBufferBytes) ok = false;          // never exceeds a block
        if (allowed.find(h.filename) == allowed.end()) ok = false;  // valid sink
        indices.push_back(h.index);
    }
    std::sort(indices.begin(), indices.end());
    for (size_t i = 0; i < indices.size(); i++) {
        if (indices[i] != i) ok = false;  // a permutation of 0..n-1, no dup/gap
    }
    Check(ok, name + ": output header invariants (count/index/size/filename)");
}

// Run one size_t case end-to-end and assert structure + count + stable order.
template<typename PredFn, typename LenFn>
void RunSizeCase(const std::string &name, const std::string &prefix,
                 size_t num_chunks, PredFn pred, LenFn len) {
    std::cout << name << " (" << num_chunks << " chunks, "
              << (num_chunks + NUM_SSDS - 1) / NUM_SSDS << " batch(es))" << std::endl;
    std::function<bool(const size_t)> predicate = pred;
    std::vector<std::vector<size_t>> chunks;
    std::vector<size_t> reference;
    MakeChunks<size_t>(num_chunks, [](size_t i) { return i; }, predicate, len,
                       chunks, reference);

    std::vector<std::string> files;
    External_Sequence out = RunFilter<size_t>(prefix, chunks, predicate, files);
    std::vector<size_t> got = ReadOutput<size_t>(out);

    CheckOutputInvariants<size_t>(out, num_chunks, OutputNames(prefix), name);
    Check(got.size() == reference.size(), name + ": element count matches reference");
    Check(got == reference, name + ": values match reference in stable index order");
    RemoveFiles(files);
}

// --- Tests -----------------------------------------------------------------

// Batch-boundary coverage: a single chunk, an under-full batch, an exactly full
// batch, one element past a batch, and a multi-batch run with a partial tail.
// The under-full / past-boundary / partial-tail cases all exercise the
// bad_flags path (Poll() returns null for slots with no pending chunk).
void TestBatchBoundaries() {
    RunSizeCase("TestSingleChunk", "eftest_one", 1,
                [](size_t x) { return x % 2 == 0; }, VariedLen);
    RunSizeCase("TestUnderFullBatch", "eftest_under", NUM_SSDS - 1,
                [](size_t x) { return x % 2 == 0; }, VariedLen);
    RunSizeCase("TestExactBatch", "eftest_exact", NUM_SSDS,
                [](size_t x) { return x % 2 == 0; }, VariedLen);
    RunSizeCase("TestOverBatch", "eftest_over", NUM_SSDS + 1,
                [](size_t x) { return x % 2 == 0; }, VariedLen);
    RunSizeCase("TestMultiBatchPartial", "eftest_multi", 2 * NUM_SSDS + 5,
                [](size_t x) { return x % 3 == 0; }, VariedLen);
}

// Predicate-shape coverage: reject all, keep all, keep a sparse subset, and an
// alternating predicate (independent of value parity).
void TestPredicateShapes() {
    // Reject everything: every output block is empty (used == 0), output empty.
    RunSizeCase("TestEmptyResult", "eftest_empty", NUM_SSDS + 3,
                [](size_t) { return false; }, VariedLen);
    // Keep everything: output must equal the full input sequence.
    RunSizeCase("TestKeepAll", "eftest_keep", NUM_SSDS + 7,
                [](size_t) { return true; }, VariedLen);
    // Sparse: only a handful survive per block, many blocks contribute nothing.
    RunSizeCase("TestSparse", "eftest_sparse", 3 * NUM_SSDS + 11,
                [](size_t x) { return x % 97 == 0; }, VariedLen);
    // Position-based predicate (keep odd-indexed elements) to avoid leaning on
    // value parity; still a deterministic function of the global index value.
    RunSizeCase("TestAlternating", "eftest_alt", NUM_SSDS + 13,
                [](size_t x) { return (x & 1u) == 1u; }, VariedLen);
}

// Many tiny chunks across multiple batches: stresses per-chunk header
// bookkeeping and the stable-ordering sort with lots of small blocks.
void TestManyTinyChunks() {
    RunSizeCase("TestManyTinyChunks", "eftest_tiny", 4 * NUM_SSDS + 9,
                [](size_t x) { return x % 4 == 0; }, TinyLen);
}

// Non-size_t element type: a 16-byte record. The predicate looks only at `key`,
// but the reference compares the whole record, so any truncation, misalignment,
// or wrong sizeof(T) handling in the read/filter/write path is caught.
struct Record {
    uint64_t key;
    uint64_t tag;
    bool operator==(const Record &o) const { return key == o.key && tag == o.tag; }
};

void TestRecordType() {
    const std::string name = "TestRecordType";
    const size_t num_chunks = 2 * NUM_SSDS + 3;
    std::cout << name << " (" << num_chunks << " chunks, 16-byte records)" << std::endl;
    std::function<bool(const Record)> pred = [](const Record r) { return r.key % 5 == 0; };
    auto make = [](size_t i) { return Record{(uint64_t) i, (uint64_t) (i * 1000003ull + 7)}; };

    std::vector<std::vector<Record>> chunks;
    std::vector<Record> reference;
    MakeChunks<Record>(num_chunks, make, pred, VariedLen, chunks, reference);

    std::vector<std::string> files;
    External_Sequence out = RunFilter<Record>("eftest_rec", chunks, pred, files);
    std::vector<Record> got = ReadOutput<Record>(out);

    CheckOutputInvariants<Record>(out, num_chunks, OutputNames("eftest_rec"), name);
    Check(got.size() == reference.size(), name + ": record count matches reference");
    Check(got == reference, name + ": full records (key+tag) match reference in order");
    RemoveFiles(files);
}

// Determinism: the same input filtered twice must yield byte-identical output,
// even though writes are spread across SSDs with a random distribution.
void TestDeterminism() {
    const std::string name = "TestDeterminism";
    const size_t num_chunks = 2 * NUM_SSDS + 4;
    std::cout << name << " (" << num_chunks << " chunks, run twice)" << std::endl;
    std::function<bool(const size_t)> pred = [](size_t x) { return x % 7 == 0; };

    std::vector<std::vector<size_t>> chunks;
    std::vector<size_t> reference;
    MakeChunks<size_t>(num_chunks, [](size_t i) { return i; }, pred, VariedLen,
                       chunks, reference);

    std::vector<std::string> files_a, files_b;
    std::vector<size_t> got_a = ReadOutput<size_t>(RunFilter<size_t>("eftest_det_a", chunks, pred, files_a));
    std::vector<size_t> got_b = ReadOutput<size_t>(RunFilter<size_t>("eftest_det_b", chunks, pred, files_b));

    Check(got_a == reference, name + ": first run matches reference");
    Check(got_a == got_b, name + ": repeated run produces identical output");
    RemoveFiles(files_a);
    RemoveFiles(files_b);
}

// Memory test: run several batches and confirm resident memory returns to near
// baseline afterwards (the per-batch 4 MiB output buffers and the reader's
// buffer pool must be freed, not leaked). A regression where buffers leak would
// retain ~NUM_SSDS * 4 MiB per batch.
void TestMemoryBounded() {
    const size_t num_chunks = 6 * NUM_SSDS;  // several full batches
    std::cout << "TestMemoryBounded (" << num_chunks << " chunks, "
              << (num_chunks + NUM_SSDS - 1) / NUM_SSDS << " batches)" << std::endl;
    std::function<bool(const size_t)> pred = [](size_t x) { return x % 2 == 0; };
    std::vector<std::vector<size_t>> chunks;
    std::vector<size_t> reference;
    MakeChunks<size_t>(num_chunks, [](size_t i) { return i; }, pred, VariedLen,
                       chunks, reference);

    const size_t rss_before = CurrentRssKb();
    std::vector<std::string> files;
    External_Sequence out = RunFilter<size_t>("eftest_mem", chunks, pred, files);
    std::vector<size_t> got = ReadOutput<size_t>(out);
    const size_t rss_after = CurrentRssKb();
    const size_t peak = PeakRssKb();

    CheckOutputInvariants<size_t>(out, num_chunks, OutputNames("eftest_mem"), "TestMemoryBounded");
    Check(got == reference, "memory-run output still matches reference");

    // If buffers leaked, retained memory would scale with batch count
    // (~NUM_SSDS * 4 MiB * num_batches). Allow a generous constant slack.
    const size_t retained_kb = rss_after > rss_before ? rss_after - rss_before : 0;
    const size_t leak_threshold_kb = 256 * 1024;  // 256 MiB
    std::cout << "    rss before=" << rss_before / 1024 << " MiB, after="
              << rss_after / 1024 << " MiB, retained=" << retained_kb / 1024
              << " MiB, peak=" << peak / 1024 << " MiB" << std::endl;
    Check(retained_kb < leak_threshold_kb,
          "resident memory returns near baseline after filtering (no per-batch leak)");

    RemoveFiles(files);
}

}  // namespace

int main() {
    std::cout << "=== ExternalFilter tests ===" << std::endl;

    TestBatchBoundaries();
    TestPredicateShapes();
    TestManyTinyChunks();
    TestRecordType();
    TestDeterminism();
    TestMemoryBounded();

    std::cout << "============================" << std::endl;
    if (g_failures == 0) {
        std::cout << "All tests passed." << std::endl;
        return 0;
    }
    std::cout << g_failures << " check(s) failed." << std::endl;
    return 1;
}

// Original randPerm/map/filter/scan demo, kept for reference. Not run by main().
void legacyDemo() {
    externalSeq<size_t> nums = externalSeqOps::randPerm<size_t>("nums", 24);
    std::cout << externalSeqOps::reduce<>(nums, add, (size_t)0) << std::endl;
    externalSeq<size_t> halved = externalSeqOps::map<size_t, size_t>(nums, "halved", [](size_t x) { return x / 2; });
    std::cout << externalSeqOps::reduce<>(halved, add, (size_t)0) << std::endl;
    externalSeq<size_t> modTen = externalSeqOps::filter<>(nums, "modTen", [](size_t a) {return a % 10 == 0;});
    std::cout << externalSeqOps::reduce<>(modTen, add, (size_t)0) << std::endl;
}

void mapThroughput() {
    parlay::internal::timer timer("Map");
    timer.next("Start prep");
    externalSeq<size_t> nums = externalSeqOps::randPerm<size_t>("nums", 24);
    timer.next("Start map");
    auto result = externalSeqOps::reduce<>(nums, add, (size_t)0);
    double time = timer.next_time();
    double throughput = GetThroughput(nums.files, time);
    std::cout << "throughput is " << throughput << " GB per sec" <<  std::endl;
}
