# Parlay Primitives for MultiSSD

Research project implementing Parlay-style parallel primitives (map, reduce, filter, scan, …) for data stored across many SSDs.  Data is too large for DRAM; all I/O goes through `io_uring` with `O_DIRECT`.

## Building

Uses **Make**, not Bazel (Bazel files exist but are not the primary build system).

```bash
# First-time setup: fetch parlaylib and build abseil from source
make deps

# Build all binaries (outputs to bazel-bin/)
make all

# Individual targets
make bazel-bin/speed_test
make bazel-bin/io_uring_test
make bazel-bin/sample_sort
make bazel-bin/permutation
make bazel-bin/sequence
make bazel-bin/plaidlaymain
make bazel-bin/externalSeqMain
make bazel-bin/scanVerify

# Run the ChunkSequence correctness tests (permTest, mapTest, reduceTest, filterTest,
# scanTest, combinedTest, delayedTest — combinedTest chains the eager primitives and
# delayedTest covers the fused delayed pipeline; both sweep edge-case sizes).  Builds
# them if needed, runs each, and exits non-zero if any fails.
make test
make test TEST_ARGS=8000000   # override the per-test element count

# Cleanup
make clean       # remove object files and binaries
make distclean   # also remove deps/ and bazel-bin/
```

**Key compiler flags**: `-std=c++17 -O2`.  Link flags: `-luring -lpthread` plus all abseil static libs.  Include roots: `.` (repo root), `deps/parlaylib`, `deps/abseil-cpp/install/include`.

> **Note**: the Makefile tracks no header dependencies, so editing a header (e.g. anything under `ChunkSequence/`) will *not* trigger a rebuild of a binary whose `.cpp` is unchanged.  Force it with `rm -f bazel-bin/<target>` (or `make clean`) before rebuilding.

### Scaling benchmark — Chunk vs file-based Map/Reduce

`ChunkSequence/bench/` is a self-contained scaling harness comparing the
ChunkSequence primitives against the file-based `sequence_algorithms` ones across
powers of two.  Only the operation is timed — `bw_compare` generates the data
first, outside the timed region.

```bash
make bazel-bin/bwCompare
# run the workload for n = 2^min .. 2^max (reps>1 keeps the per-column min time):
ChunkSequence/bench/bench_chunk_vs_seq.sh [min_exp] [max_exp] [reps]   # defaults 20 26 1
# -> writes ChunkSequence/bench/results/chunk_bw.csv (the driver force-rebuilds bwCompare)

python3 ChunkSequence/bench/plot_chunk_bw.py     # needs matplotlib
# -> ChunkSequence/bench/results/chunk_bw.png : two log-log panels (both axes base-2),
#    left Map vs ChunkMap, right Reduce vs ChunkReduce
```

Each run needs ~`32·n` bytes on the SSD mounts (chunk input + seq input + both
map outputs).  On a dev box where the "SSDs" share one tmpfs, keep `max_exp`
small enough to fit; the driver clears `/mnt/ssd*/` between sizes.

Nix environments are detected automatically; liburing include/lib paths are picked up from `NIX_CFLAGS_COMPILE` / `NIX_LDFLAGS`.

### Scaling harness — in-memory vs chunk-eager vs chunk-delayed

`ChunkSequence/bench/delayed_compare.cpp` (→ `bwDelayed`) times the **same** pipeline
three ways at one size — in-memory `parlay::delayed` (the DRAM "speed-of-light"
line), chunk-eager (round-trips every intermediate through SSD), and chunk-delayed
(fused) — plus a raw-read ceiling (`ChunkReduce`), and checks all substrates agree
(cross-substrate differential correctness; exits non-zero on mismatch).  Pipelines:
`map|reduce`, `map|map|reduce`, and the write-terminated `force(map|map)`.

```bash
make bazel-bin/bwDelayed
./bazel-bin/bwDelayed <n>            # one size: per-substrate rows + CSV line + agree=1
# sweep n = 2^min .. 2^max (intended for a real multi-SSD box that crosses the DRAM boundary):
ChunkSequence/bench/bench_delayed_scale.sh [min_exp] [max_exp] [reps]   # defaults 24 32 1
# -> ChunkSequence/bench/results/delayed_scale.csv
```

The in-memory baseline is skipped once the input exceeds a RAM budget (default ½
physical RAM; override with env `DELAYED_INMEM_BUDGET_BYTES`).  Past that **cliff**
its CSV columns blank out and the plotted line stops — that boundary is the point of
the sweep, so it needs storage ≫ RAM.  On a tmpfs dev box (the "SSDs" are RAM) you
only see the in-DRAM-overhead regime, never the cliff.

```bash
python3 ChunkSequence/bench/plot_delayed_scale.py     # needs matplotlib
# -> results/delayed_scale.png : two throughput panels (y = effective input GB/s,
#    log-log base 2), left map|map|reduce (read-bound), right force(map|map)
#    (write-bound); each shows in-mem (stops at the cliff), chunk-delayed
#    (~raw-read ceiling), and chunk-eager.
```

The in-mem-vs-chunk gap measures the externalization/engineering tax (both fuse — same
technique, different substrate); the eager-vs-delayed gap on the same plot isolates the
fusion win; chunk-delayed-vs-raw-read shows I/O efficiency.

## Repository layout

```
configs.h                   global constants (SSD_COUNT, CHUNK_SIZE, O_DIRECT_MULTIPLE, …)
utils/
  file_info.h               FileInfo struct (filename, file_index, true_size, file_size, before_size)
  file_utils.h/.cpp         AlignUp/AlignDown, FindFiles, GetFileName, MakeFileEndMarker, …
  unordered_file_reader.h   async multi-file reader (io_uring)
  unordered_file_writer.h   async multi-file writer (io_uring)
  simple_queue.h            blocking MPMC queue used between IO threads and workers
  type_allocator.h          aligned memory pool
  logger.h                  SYSCALL() / ASSERT() macros wrapping absl logging
sequence_algorithms/
  map.h                     Map   – file-level map
  reduce.h                  Reduce – file-level reduce
  filter.h                  Filter / FilterFile – file-level filter
ChunkSequence/
  chunk_seq.h               chunk / chunk_seq structs
  chunk_seq_reader.h        async chunk-level reader (io_uring)
  chunk_map.h               ChunkMap   – chunk-level map
  chunk_reduce.h            ChunkReduce – chunk-level reduce
  chunk_filter.h            ChunkFilter – chunk-level filter (tightly packed output)
  chunk_scan.h              ChunkScan  – chunk-level exclusive prefix scan
  chunk_delayed.h           delayed (fused) map/reduce/scan/filter/tabulate (ChunkSequenceOps::delayed)
  tests/                    correctness tests (perm_test, map_test, reduce_test, filter_test, scan_test, combined_test, delayed_test → permTest/…/combinedTest/delayedTest)
  bench/                    scaling benchmarks (bw_compare.cpp → bwCompare; delayed_compare.cpp → bwDelayed; driver + matplotlib plotter)
  examples/                 example programs (raytracer.cpp → raytracer, path_tracer.cpp → pathTracer, make_png.py post-processor)
Plaidlay/                   external-scan / scan-verify experiments
benchmarks/                 throughput benchmarks (speed-test entry point)
deps/                       parlaylib (header-only), abseil-cpp (built as static libs)
```

---

## Core abstractions

### `FileInfo`  (`utils/file_info.h`)

Describes one file in a multi-file dataset.

| field | meaning |
|---|---|
| `file_name` | path on disk |
| `file_index` | position in the file list |
| `true_size` | bytes of actual data (may exclude padding) |
| `file_size` | bytes on disk (rounded up for O_DIRECT) |
| `before_size` | cumulative byte offset of all preceding files |

### `UnorderedFileReader<T>`  (`utils/unordered_file_reader.h`)

Reads a list of `FileInfo` files asynchronously using io_uring, in whatever completion order the SSDs return them.  Multiple worker threads each own a subset of files and a private io_uring ring.

```cpp
UnorderedFileReader<uint64_t> reader;
reader.PrepFiles(files);          // or PrepFiles("prefix") to auto-discover
reader.Start(UnorderedReaderConfig(/*num_threads=*/5, /*max_requests=*/16, /*queue_depth=*/8));

// consumer loop (typically inside parlay::parallel_for):
auto [ptr, n, file_index, element_index] = reader.Poll(); // blocks until data or EOF
// …process ptr[0..n)…
reader.allocator.Free(ptr);
```

`Poll()` returns `(nullptr, 0, 0, 0)` when all files are exhausted.  The `active_chunks_per_file` bitmap in `UnorderedReaderConfig` lets callers skip specific 512 KB chunks within a file.

### `UnorderedFileWriter<T>`  (`utils/unordered_file_writer.h`)

Writes data buffers to a set of output files asynchronously.

```cpp
UnorderedWriterConfig cfg;
cfg.num_files = files.size();
cfg.num_threads = 5;
UnorderedFileWriter<uint64_t> writer("output_prefix", cfg);
writer.Push(shared_ptr, n_elements, file_index, byte_offset);
writer.Wait();   // flush and close
```

`Push()` takes a `shared_ptr` so buffers can be freed once the write completes.  `allow_expand = true` lets the writer create new files on demand if `file_index` exceeds the initial count.

---

## Sequence primitives  (`sequence_algorithms/`)

All three operate on `std::vector<FileInfo>` and use `UnorderedFileReader` internally.

### `Map`  (`map.h`)

```cpp
template <typename T, typename R = T, bool in_place = sizeof(T) == sizeof(R)>
void Map(std::vector<FileInfo> files, std::string result_prefix, std::function<R(T)> f);
```

Reads every element, applies `f`, writes results to `result_prefix_0`, `result_prefix_1`, …  When `in_place` is true (same element size) the reader buffer is transformed and reused directly.

### `Reduce`  (`reduce.h`)

```cpp
template <typename T, typename R = T, typename Monoid>
R Reduce(std::vector<FileInfo> files, Monoid monoid);
```

Each parlay worker accumulates a local partial result; `parlay::reduce` combines them.  Uses 10 reader threads because IO bandwidth, not CPU, is the bottleneck.

### `Filter` / `FilterFile`  (`filter.h`)

```cpp
// Single file:
FileInfo FilterFile(const FileInfo& in, const std::string& out, std::function<bool(T)> pred);

// All files in parallel:
std::vector<FileInfo> Filter(const std::vector<FileInfo>& files, const std::string& prefix,
                             std::function<bool(T)> pred);
```

Preserves element order within each file using a priority queue keyed on `element_index`.  Packs survivors into 4 MB write buffers; the last buffer carries a file-end marker (`MakeFileEndMarker`) so readers can determine the true data size.

---

## ChunkSequence  (`ChunkSequence/`)

An alternative data layout and set of primitives that address data at **chunk** granularity instead of whole-file granularity.

The chunk primitives live in the `ChunkSequenceOps` namespace (declared in `chunk_seq.h`): `tabulate`, `perm`, `ChunkMap` (`chunk_map.h`), `ChunkReduce` (`chunk_reduce.h`), `ChunkFilter` (`chunk_filter.h`), and `ChunkScan` (`chunk_scan.h`).  Call them qualified, e.g. `ChunkSequenceOps::ChunkMap(...)`.  These are all **eager** (every op reads from and writes back to SSD).  A **delayed/fused** variant lives in the nested `ChunkSequenceOps::delayed` namespace (`chunk_delayed.h`) — see *Delayed (fused) sequences* below.

### `chunk` / `chunk_seq`  (`chunk_seq.h`)

```cpp
const size_t CHUNK_SIZE      = 4 << 20;                          // 4 MB
const size_t ELEMS_PER_CHUNK = CHUNK_SIZE / sizeof(uint64_t);   // 524,288

struct chunk {
    std::string filename;   // file containing this chunk
    size_t begin_addr;      // byte offset of the chunk within that file
    size_t used;            // bytes of actual data (≤ CHUNK_SIZE)
    size_t index;           // position of this chunk in the chunk_seq
};

struct chunk_seq { std::vector<chunk> chunks; };
```

**Convention**: a `chunk_seq` corresponds to exactly **one file per drive** (i.e. `SSD_COUNT` files total, one on each SSD mount point).  All chunks point into those files at offsets that are multiples of `CHUNK_SIZE`, which is itself a multiple of `O_DIRECT_MULTIPLE`, so every read is naturally aligned for O_DIRECT io_uring without extra padding logic. On creating a sequence via tabulate, the chunks are randomly assigned to the drives, balls in bins style. This saturates all drives in parallel.  

**Index-ordered invariant**: `chunk_seq.chunks` is always stored in index order, i.e. `chunks[i].index == i`.  `tabulate` establishes this, and every primitive that returns a `chunk_seq` (`ChunkMap`, `ChunkFilter`, …) must preserve it so callers can index by position without a lookup.  
The `chunk_seq_reader` opens each per-drive file once per worker thread (via its fd cache) and issues individual `CHUNK_SIZE`-aligned reads for each chunk, letting io_uring pipeline them across drives.

### `ChunkSequenceReader<T>`  (`chunk_seq_reader.h`)

Drop-in replacement for `UnorderedFileReader` that works on `chunk_seq` instead of `vector<FileInfo>`.

```cpp
ChunkSequenceReader<uint64_t> reader;
reader.PrepChunks(seq);
reader.Start(/*num_threads=*/5, /*queue_depth=*/32, /*max_requests=*/16);

// consumer loop:
auto [ptr, n, chunk_index] = reader.Poll();  // (nullptr,0,0) when done
// …process ptr[0..n)…
reader.allocator.Free(ptr);
```

Key differences from `UnorderedFileReader`:
- Issues one io_uring read per chunk at `chunk.begin_addr` for `AlignUp(chunk.used)` bytes; reports only `chunk.used / sizeof(T)` elements.
- Caches open file descriptors per worker thread so files referenced by multiple chunks are opened once.
- `BufferData` is a 3-tuple `(ptr, n_elements, chunk_index)` — no `file_index` since the chunk already encodes its file.

### `ChunkSequenceOps::tabulate`  (`chunk_seq.h`)

```cpp
chunk_seq ChunkSequenceOps::tabulate(size_t n, const std::string& result_prefix,
                                     std::function<uint64_t(size_t)> f);
```

General constructor: fills position `i` with `f(i)` for `i` in `[0, n)`, writing `ELEMS_PER_CHUNK` elements per chunk.

- Divides `[0, n)` into chunks of `ELEMS_PER_CHUNK` = 524 288 elements each.
- Randomly assigns each chunk to one of the `GetSSDList().size()` drives (uniform, `mt19937_64`) so writes are balanced across SSDs.
- Pre-fallocates one file per drive at `GetFileName(result_prefix, drive)`.
- Writes via `UnorderedFileWriter` (io_uring); back-pressure queue caps DRAM to 256 MB in-flight.
- `begin_addr` for slot `k` of a drive is `k * CHUNK_SIZE`, always O_DIRECT-aligned.  The last chunk is zero-padded; its `used` field holds the true byte count.

### `ChunkSequenceOps::perm`  (`chunk_seq.h`)

```cpp
chunk_seq ChunkSequenceOps::perm(size_t n);
```

Thin wrapper: `tabulate(n, "perm", [](size_t i) { return (uint64_t)i; })`.

### `ChunkMap`  (`chunk_map.h`)

```cpp
template <typename T, typename R = T>
chunk_seq ChunkSequenceOps::ChunkMap(const chunk_seq& seq, const std::string& result_prefix,
                                     std::function<R(T)> f);
```

Maps `f` over every element, writing the results out with the same **one-file-per-drive** layout as `tabulate`: output chunks are randomly assigned to the `GetSSDList()` drives (balls-in-bins) and packed at `CHUNK_SIZE`-aligned offsets within each drive's file (`result_prefix + drive_index`), so a single file grows to hold many more chunks than there are drives. Writes go through `UnorderedFileWriter` (io_uring).  Returns an index-ordered `chunk_seq` describing the outputs so results are directly chainable.  In-place optimization applies when `T == R` (the reader buffer is transformed and handed straight to the writer).

### `ChunkReduce`  (`chunk_reduce.h`)

```cpp
template <typename T, typename R = T, typename Monoid>
R ChunkSequenceOps::ChunkReduce(const chunk_seq& seq, Monoid monoid);
```

Same monoid protocol as `Reduce`.

### `ChunkFilter`  (`chunk_filter.h`)

```cpp
template <typename T>
chunk_seq ChunkSequenceOps::ChunkFilter(const chunk_seq& seq, const std::string& result_prefix,
                                        std::function<bool(T)> pred);
```

Filters elements by `pred`, writing survivors as a **tightly packed** chunk_seq.  Unlike `ChunkMap`, the output size is unknown upfront, so output files are not pre-fallocated — they grow via `pwrite` at `CHUNK_SIZE`-aligned offsets.  All output chunks except the final one have `used == CHUNK_SIZE` (dense packing).

Element order is preserved.  The input is processed in **index-contiguous** batches of `FILTER_BATCH_SIZE = 128` chunks: batch `k` reads exactly the slice `seq.chunks[k*128 : (k+1)*128)` with its own `ChunkSequenceReader`, so even though the reader delivers chunks in arbitrary completion order, every chunk in a batch belongs to that contiguous index range.  Sorting within the batch then yields true global order before packing.  (A single whole-sequence reader would let a later chunk land in an earlier batch and scramble the survivor stream — see the `order_cross_batch` case in `filter_test.cpp`.)

Algorithm (per batch of `FILTER_BATCH_SIZE = 128` input chunks):
1. Read the batch's contiguous slice and sort by `chunk_index` (reader returns in completion order, but only chunks from this slice).
2. Filter in-place in parallel (`parlay::parallel_for`): compact survivors to front of each buffer.
3. Compute prefix sums over survivor counts to determine output positions.
4. Parallel scatter: each input chunk writes its survivors to the correct offset(s) in pre-allocated output buffers — no races because prefix sums guarantee non-overlapping destination ranges.
5. Flush full output chunks to the writer (balls-in-bins drive assignment); carry leftover survivors to the next batch.

Returns an index-ordered `chunk_seq`.  Final chunk's `used` field holds the true survivor byte count; the rest are zero-padded to `CHUNK_SIZE` for O_DIRECT alignment.

### `ChunkScan`  (`chunk_scan.h`)

```cpp
template <typename T, typename R = T, typename Monoid>
std::pair<chunk_seq, R> ChunkSequenceOps::ChunkScan(const chunk_seq& seq,
                                                    const std::string& result_prefix,
                                                    Monoid monoid);
```

**Exclusive** prefix scan: `out[i] = monoid(in[0], …, in[i-1])`, with `out[0] = monoid.identity`.  Same monoid protocol as `ChunkReduce` (`monoid.identity`, `monoid(a, b)`).  Returns `{result_seq, total}` where `total = monoid(in[0], …, in[n-1])` is the grand reduction (the parlay scan convention).

Out-of-core, two-level (block) scan.  Data is too large for DRAM, but there is one accumulator per chunk, so the `O(c)` block-sum array fits in memory:
1. **Pass 1** (one read pass): reduce each chunk independently in parallel, storing the per-chunk total into `chunk_sums[chunk_index]`.
2. **Block prefix** (sequential, `O(c)` in RAM): exclusive prefix over `chunk_sums` → `offset[i]` = reduction of every element in chunks strictly before `i` (the seed for chunk `i`); the running accumulator after the last chunk is the returned `total`.  `c` is small, so a sequential loop beats invoking `parlay::scan`.
3. **Pass 2** (second read pass + one write pass): re-read each chunk and run a sequential exclusive scan seeded with `offset[chunk_index]`, writing the result with the same one-file-per-drive layout as `ChunkMap`/`tabulate` (balls-in-bins drive assignment, `CHUNK_SIZE`-aligned slots, pre-fallocated files).

Returns an index-ordered `chunk_seq` (same length as the input) plus the total.  In-place optimization applies when `T == R` (the reader buffer is scanned and handed straight to the writer; the input element is read before the exclusive prefix overwrites it).

### Delayed (fused) sequences  (`chunk_delayed.h`)

A port of parlaylib's *block-iterable-delayed* (BID) design to the out-of-core
setting, in the nested `ChunkSequenceOps::delayed` namespace.  The eager primitives
above round-trip every intermediate through the SSDs; a delayed sequence **fuses** an
operation chain so intermediates never touch disk.  `map` is lazy, `reduce`/`force`
consume in a single read pass, and `scan` is *partially delayed*.  Call qualified,
e.g. `ChunkSequenceOps::delayed::reduce(...)`.

A "block" is one 4 MiB chunk: parlay workers run in parallel across chunks and
sequentially within a chunk (the same shape as the eager consumer loops).  A delayed
sequence is a templated *(source + per-chunk iterator factory)*; everything is
templated (not `std::function`) so the fused chain inlines.  Two source kinds:
`delay(seq)` over an on-SSD `chunk_seq`, and `tabulate(n, f)` over a generated index
range (no source files — `reduce`/`scan` over it do zero source I/O).

```cpp
namespace d = ChunkSequenceOps::delayed;
auto s  = d::delay(seq);                          // wrap an on-SSD chunk_seq (lazy)
auto t  = d::tabulate(n, [](size_t i){ ... });    // generated source, no files
auto m  = d::map(s, f);                           // lazy; composes with no I/O
uint64_t r       = d::reduce(m, monoid);          // forces: one read pass, zero writes
auto [sd, total] = d::scan(m, monoid);            // partially delayed (offsets, then lazy)
chunk_seq out    = d::force(m, "out_prefix");     // materialize to SSD (index-ordered)
chunk_seq flt    = d::filter(m, "flt_prefix", pred);  // terminal, ChunkFilter-style packing
```

- `map` returns a delayed sequence of the same kind, composing element-wise with no temp buffer and no I/O — chain arbitrarily.
- `reduce` folds under a `ChunkReduce`-style monoid (operating on the *post-map* element type, `decltype` of the fused chain) in one read pass.
- `scan` (exclusive, parlay convention) does one read pass for the block offsets, then returns `{delayed_seq, total}` whose second pass stays lazy; further maps fuse on top, and forcing/reducing the result triggers the second read pass.
- `force` materializes any delayed sequence to a real `chunk_seq` with the same balls-in-bins one-file-per-drive layout as `ChunkMap`; it allocates a fresh output buffer per chunk (no in-place reuse, so scan chains stay correct).
- `filter` is a terminal modeled on `ChunkFilter` (index-contiguous 128-chunk batches, dense packing) that reads through the fused delayed iterator, so preceding maps cost no extra I/O.
- For `reduce(map(map(seq,f),g),m)` the eager path moves 3 reads + 2 writes; the delayed path moves 1 read — quantified by `bench/delayed_compare.cpp` (→ `bwDelayed`).
- **Lifetime**: a delayed sequence (and any `scan` result derived from it) holds a pointer to its source `chunk_seq`; the source must outlive every terminal call.  `flatten` is omitted — chunks hold plain POD, so there is nothing nested to flatten.

---

## Configuration constants  (`configs.h`)

| constant | default | meaning |
|---|---|---|
| `SSD_COUNT` | 30 | number of SSD mount points |
| `READER_READ_SIZE` | 512 KB | io_uring read granularity (override with `-DREADER_READ_SIZE_BYTES=N`) |
| `O_DIRECT_MULTIPLE` | 4096 | alignment requirement for O_DIRECT buffers and offsets |
| `IO_URING_BUFFER_SIZE` | 64 | default io_uring ring depth |
| `CHUNK_SIZE` | 4 MB | size of one chunk in a `chunk_seq` (in `chunk_seq.h`) |
| `ELEMS_PER_CHUNK` | 524 288 | `CHUNK_SIZE / sizeof(uint64_t)`; elements per chunk (in `chunk_seq.h`) |
| `MAIN_MEMORY_SIZE` | 400 GB | assumed DRAM budget |
