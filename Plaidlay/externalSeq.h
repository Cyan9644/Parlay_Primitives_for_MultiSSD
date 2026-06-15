#include "utils/file_info.h"
#include "utils/file_utils.h"
#include "utils/unordered_file_writer.h"
#include "parlay/primitives.h"
#include <vector>

template <typename T>
// this is some seq, later possibly will implement delaying, that "owns" some
// number of files on drive with owns in quotes because the file system handles
// memory
//
// currently templated but really the type info doesn't come into play until we
// have some way to consume to an in memory variable
//
// should be able to map, reduce, filter, flatten, scan, and to convert to an in memory seq
// maybe printing would be nice for small seqs? and write out to a file for big ones?
class externalSeq {
    private:
        std::vector<FileInfo> files;
        // the prefix that all of the file names start with
        std::string prefix;
    public:
        externalSeq() {}
        externalSeq(std::vector<FileInfo> files, std::string prefix) : files(files), prefix(prefix) {}
        ~externalSeq() {}

        // TODO: at least for now, we can't support these. depends on how we do delaying I guess
        // if you wanted to you could probably implement these with a single small read or write
        // but I'm not sure how alignment factors into that
        //
        // auto begin() {return data.begin();}
        // auto end() {return data.end();}

        // auto begin() const {return data.cbegin();}
        // auto end() const {return data.cend();}

        // auto size() const {return data.size();}

        // T& operator[](int i) { return data[i]; }
        // const T& operator[](int i) const { return data[i]; }
};
namespace plaidlay {
    void nop(void *ptr) {}
    template<typename T>
    externalSeq<T> randPerm(const std::string &prefix, size_t power_of_two) {
        size_t n = 1UL << power_of_two;
        // TODO: this part right here is copied from sample-sort.cpp for now
        auto nums = (size_t *) aligned_alloc(O_DIRECT_MEMORY_ALIGNMENT, n * sizeof(size_t));
            {
                auto perm = parlay::random_permutation(n, parlay::random(std::random_device()()));
                parlay::parallel_for(0, n, [&](size_t i) {
                    nums[i] = perm[i];
                });
            }
            UnorderedWriterConfig config;
            config.num_threads = 2;
            config.io_uring_size = 64;
            UnorderedFileWriter<size_t> writer(prefix, config);
            size_t step = std::min(1UL << 20, n);
            for (size_t i = 0; i < n; i += step) {
                writer.Push(std::shared_ptr<size_t>(nums + i, nop), std::min(step, n - i));
            }
            writer.Close();
            writer.Wait();
            free(nums);
        // Is this bad? first generating then finding? should be fine right
        auto files = FindFiles(prefix);
        return externalSeq<T>(files, prefix);
    }
    template<typename T, typename Func>
    externalSeq<T> tabulate(int n, Func f) {
        // std::vector<T> data;
        // for (int i = 0; i < n; i++) {
        //     data.push_back(f(i));
        // }
        // return naiveSeq<T>(data);
    }
    template <typename T, typename Func>
    auto map(const externalSeq<T>& seq, Func f) {
        // using U = decltype(f(*seq.begin()));
        // std::vector<U> out;
        // for (const T& elem : seq) {
        //     out.push_back(f(elem));
        // }
        // return naiveSeq<U>(out);
    }
    // here Func should map (U, T) -> U
    template <typename T, typename U, typename Func>
    auto reduce(const externalSeq<T>& seq, Func f, U identity) {
        // U out = identity;
        // for (const T& elem : seq) {
        //     out = f(out, elem);
        // }
        // return out;
    }
    // filter, we take a seq and a boolean predicate
    // Func here maps T -> bool
    template <typename T, typename Func>
    externalSeq<T> filter (const externalSeq<T>& seq, Func f) {
        // std::vector<T> out;
        // for (const T& elem: seq) {
        //     if (f(elem)) {
        //         out.push_back(elem);
        //     }
        // }
        // return naiveSeq<T>(out);
    }
}
