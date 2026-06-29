//this is a backup file in case my primes implementation doesn't work
//i.e. we'll still have something to benchmark on Monday bc the machine is up

#ifndef AI_PRIMES_H
#define AI_PRIMES_H

#include <algorithm>
#include <cmath>
#include <array>
#include <atomic>
#include <random>
#include <string>
#include <vector>

#include <parlay/parallel.h>
#include <parlay/primitives.h>
#include <parlay/sequence.h>

#include "chunk_header.h"
#include "ExternalBoolean.h"
#include "utils/unordered_file_reader_modified.h"
#include "utils/unordered_file_writer_modified.h"

// **************************************************************
// External-memory parallel primes (sieve of Eratosthenes).
//
// External version of examples/in_memory/in_memory_primes.h. The flag array
// (n+1 booleans, "true = still possibly prime") is far too big to hold in DRAM
// for large n, so it lives on disk as an External_Sequence and is streamed back
// one 4 MiB block at a time through the io_uring UnorderedChunkReader -- we never
// touch the flags element-by-element across the device.
//
// The in-memory blocked sieve is what makes this efficient on disk: it sieves
// over blocks of size sqrt(n), and here each on-disk chunk *is* one block. Every
// composite m <= n has a prime factor <= sqrt(n), and all primes <= sqrt(n) live
// in `base_primes`, so applying every base prime to a block fully settles it in a
// single pass. That means once we have read a block we can immediately emit the
// surviving indices (the primes in that block's range) -- there is no need to
// write the sieved flags back to disk and read them a second time.
//
// ASSUMPTION: the base primes up to sqrt(n) fit comfortably in DRAM. This is the
// standard assumption for external-memory sieves (the "small primes" wheel); for
// n = 10^12 that is only ~78498 primes <= 10^6.
// **************************************************************

namespace external_primes_detail {

// In-memory sieve for the base primes up to m (m ~ sqrt of the external n, small
// by assumption). Returned in increasing order.
inline parlay::sequence<long> base_sieve(long m) {
    if (m < 2) return parlay::sequence<long>();
    std::vector<char> is_prime(m + 1, 1);
    is_prime[0] = is_prime[1] = 0;
    for (long p = 2; p * p <= m; p++) {
        if (is_prime[p]) {
            for (long k = p * p; k <= m; k += p) is_prime[k] = 0;
        }
    }
    parlay::sequence<long> out;
    for (long i = 2; i <= m; i++) {
        if (is_prime[i]) out.push_back(i);
    }
    return out;
}

}  // namespace external_primes_detail

// Returns an External_Sequence whose logical contents are the primes <= n, in
// increasing order (one chunk of `long` per input flag block).
//
//  flag_files : NUM_SSDS filenames that back the on-disk flag array (scratch).
//  out_files  : NUM_SSDS filenames that back the returned prime list.
inline External_Sequence primes(size_t n,
                                const std::vector<std::string> &flag_files,
                                const std::vector<std::string> &out_files) {
    // base case: there are no primes <= 1
    if (n < 2) {
        return External_Sequence(0);
    }

    // Base primes up to sqrt(n), held in DRAM. Nudge sqrt up to defend against
    // floating-point rounding so we never miss a base prime near the boundary.
    long sqrt_n = (long) std::sqrt((double) n);
    while ((size_t)(sqrt_n + 1) * (size_t)(sqrt_n + 1) <= n) sqrt_n++;
    while (sqrt_n > 0 && (size_t) sqrt_n * (size_t) sqrt_n > n) sqrt_n--;
    parlay::sequence<long> base_primes = external_primes_detail::base_sieve(sqrt_n);

    // On-disk flag array: n+1 booleans, all true. BoolCreate lays them out across
    // flag_files as 4 MiB blocks and records one chunk_header per block.
    constexpr size_t kBlockBytes = 4 << 20;
    constexpr size_t kFlagsPerChunk = kBlockBytes / sizeof(bool);   // elems per flag block
    constexpr size_t kPrimesPerChunk = kBlockBytes / sizeof(long);  // capacity of an output block
    const size_t num_chunks = (n + 1 + kFlagsPerChunk - 1) / kFlagsPerChunk;

    External_Sequence flags(num_chunks);
    BoolCreate<bool>(n + 1, flags, flag_files, true);

    // Stream the flag blocks back through io_uring, NUM_SSDS at a time.
    auto &flag_headers = flags.ordered_underlying_sequence;
    UnorderedChunkReader<bool, kBlockBytes> reader;
    reader.PrepFiles(flag_headers);
    reader.Start();

    const size_t expected_reads = (flag_headers.size() + NUM_SSDS - 1) / NUM_SSDS;

    // Output: one block of primes per input flag block, spread across out_files.
    External_Sequence result(flag_headers.size());
    auto *out_headers = &result.ordered_underlying_sequence;

    UnorderedChunkWriter<long> writer;
    UnorderedChunkWriterConfig wconfig;
    wconfig.num_threads = WRITER_THREADS;
    writer.Start(out_files, wconfig);

    std::vector<long *> buffer(NUM_SSDS);
    std::array<std::atomic<size_t>, NUM_SSDS> file_offsets{};

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> distrib(0, NUM_SSDS - 1);

    size_t batch = 0;
    while (batch < expected_reads) {
        for (int i = 0; i < NUM_SSDS; i++) {
            buffer[i] = (long *) aligned_alloc(O_DIRECT_MEMORY_ALIGNMENT, kBlockBytes);
        }

        std::atomic<int> counter(0);
        std::vector<unsigned int> random_holder(NUM_SSDS);
        std::atomic<bool> bad_flags[NUM_SSDS];
        std::vector<int> slot_for(NUM_SSDS, -1);
        for (int k = 0; k < NUM_SSDS; k++) {
            random_holder[k] = distrib(gen);
            bad_flags[k] = false;
        }

        parlay::parallel_for(0, NUM_SSDS, [&](size_t i) {
            // Pull one flag block off the io_uring reader. Poll() blocks while
            // chunks remain and only returns null once the reader has drained, so
            // bad_flags is only set for surplus slots in the final partial batch.
            auto polled = reader.Poll();
            bool *ptr = std::get<0>(polled);
            size_t count = std::get<1>(polled);   // valid flags in this block
            size_t index = std::get<3>(polled);   // global block number
            if (ptr == nullptr) {
                bad_flags[i] = true;
                return;
            }

            // This block covers the logical index range [chunk_start, chunk_end).
            const size_t chunk_start = index * kFlagsPerChunk;
            const size_t chunk_end = chunk_start + count;

            // Sieve: cross off every multiple of every base prime that lands in
            // this block, starting at p*p (smaller multiples are crossed off by
            // smaller primes).
            for (long pl : base_primes) {
                const size_t p = (size_t) pl;
                const size_t first =
                    std::max<size_t>(p * p, ((chunk_start + p - 1) / p) * p);
                for (size_t m = first; m < chunk_end; m += p) {
                    ptr[m - chunk_start] = false;
                }
            }
            // 0 and 1 are not prime (only affects the very first block).
            if (chunk_start == 0) {
                if (count > 0) ptr[0] = false;
                if (count > 1) ptr[1] = false;
            }

            // The block is now fully settled: surviving indices are the primes.
            long *out = buffer[i];
            size_t produced = 0;
            for (size_t k = 0; k < count; k++) {
                if (ptr[k]) out[produced++] = (long) (chunk_start + k);
            }
            reader.allocator.Free(ptr);  // recycle the flag block

            chunk_header chunked;
            chunked.index = index;                       // preserve block order
            chunked.filename = out_files[random_holder[i]];
            chunked.used = produced * sizeof(long);
            chunked.begin_address = 0;                   // assigned in the push loop below
            const int slot = counter.fetch_add(1);
            slot_for[i] = slot;
            (*out_headers)[batch * NUM_SSDS + slot] = chunked;
        });

        // Issue this batch's writes outside the parallel region so per-SSD offsets
        // can be assigned without another atomic in the hot loop.
        for (int r = 0; r < NUM_SSDS; r++) {
            if (!bad_flags[r]) {
                const size_t base_offset = file_offsets[random_holder[r]].fetch_add(kBlockBytes);
                (*out_headers)[batch * NUM_SSDS + slot_for[r]].begin_address = base_offset;
                writer.Push(std::shared_ptr<long>(buffer[r], free), kPrimesPerChunk,
                            random_holder[r], base_offset);
            } else {
                free(buffer[r]);  // surplus slot in the final batch: no chunk to write
            }
        }
        batch++;
    }

    writer.Wait();

    std::sort(result.begin(), result.end(),
              [](const chunk_header &a, const chunk_header &b) { return a.index < b.index; });
    return result;
}

// Convenience overload: derive the NUM_SSDS flag/output filenames from a prefix.
inline External_Sequence primes(size_t n, const std::string &prefix) {
    std::vector<std::string> flag_files, out_files;
    flag_files.reserve(NUM_SSDS);
    out_files.reserve(NUM_SSDS);
    for (int i = 0; i < NUM_SSDS; i++) {
        flag_files.push_back(prefix + "_flag_" + std::to_string(i));
        out_files.push_back(prefix + "_prime_" + std::to_string(i));
    }
    return primes(n, flag_files, out_files);
}

#endif  // EXTERNAL_PRIMES_H

