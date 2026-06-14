#ifndef FILTER_H
#define FILTER_H

#include "plaidlay.h"
#include <cassert>
#include <math.h>
#include <iostream>
#include <fcntl.h>
#include <sys/time.h>
#include <unistd.h>
#include <stdlib.h>
#include <liburing.h>
#include <cstring>
#include <parlay/parallel.h>
#include <parlay/primitives.h>

#define NUMBLOCKS 1
#define BLOCKSIZE_FILTER 512
//copied from plaidlay.h
template <typename T, typename Func>
naiveSeq<T> filter_range(const naiveSeq<T>& seq, Func f, unsigned long start, unsigned long end){

        auto seq2 = plaidlayNaive::cut(seq, start, end);
        std::vector<T> out;

        
        for (const T& elem: seq2) {
            if (f(elem)) {
                out.push_back(elem);
            }
        }
        return naiveSeq<T>(out);


    }


//don't use this
template <typename T>

naiveSeq<T> flatten_range(const naiveSeq<naiveSeq<T>>& seq, unsigned long start, unsigned long end) {
        // naiveSeq<T> res;
        std::vector<T> res;
        for(; start < end; start++){
            for(const auto& in: seq[start]){
                
                res.push_back(in);
            }
        }
        return naiveSeq<T>(res);


    }




//filters in parallel (must be in DRAM)
//this can be improved on by removing the lock -- use out.resize to write in parallel without synchronization overhead
//this also doesn't preserve ordering, whoops
template <typename T, typename Func>
naiveSeq<T> naive_parallel_dram_filter_lock(naiveSeq<T> sequence, Func f){

std::vector<naiveSeq<T>> out;
std::atomic<bool> lock = true;
auto num_blocks = (int)(sequence.size() / BLOCKSIZE_FILTER);
auto remainder = (int)(sequence.size() % BLOCKSIZE_FILTER);
parlay::parallel_for(0, num_blocks, [&](long i){


// auto filtered = filter_range(sequence,f BLOCKSIZE_FILTER * i, BLOCKSIZE_FILTER * (i+1));
naiveSeq<T> filtered;

i != num_blocks-1 ? filtered = plaidlayNaive::filter(plaidlayNaive::cut(sequence, BLOCKSIZE_FILTER * i, BLOCKSIZE_FILTER * (i+1)), f) : filtered = plaidlayNaive::filter(plaidlayNaive::cut(sequence, BLOCKSIZE_FILTER * i, BLOCKSIZE_FILTER * (i+1) + remainder));

bool check_yes = 1;
bool check_no = 0;
while(!(lock.compare_exchange_strong(check_yes, check_no))){

    check_yes = true;
    check_no = false;

}

out.push_back(filtered);
lock = 1;



});



auto outer = naiveSeq<naiveSeq<T>>(out);
return flatten(outer);


}



//this method uses the cheating approach, which is to allocate for the maximal case of filter where no elements are removed
template <typename T, typename Func>
naiveSeq<T> naive_parallel_dram_filter(naiveSeq<T> sequence, Func f){

std::vector<naiveSeq<T>> out;
std::atomic<bool> lock = true;
auto num_blocks = (int)(sequence.size() / BLOCKSIZE_FILTER);
auto remainder = (int)(sequence.size() % BLOCKSIZE_FILTER);
out.resize(num_blocks);
parlay::parallel_for(0, num_blocks, [&](long i){


out[i] = plaidlayNaive::filter(plaidlayNaive::cut(sequence, BLOCKSIZE_FILTER * i, BLOCKSIZE_FILTER * (i+1)), f);


});

auto remain = plaidlayNaive::filter(plaidlayNaive::cut(sequence, BLOCKSIZE_FILTER * num_blocks, BLOCKSIZE_FILTER * num_blocks + remainder), f);

out.push_back(remain);

auto outer = naiveSeq<naiveSeq<T>>(out);


return flatten(outer);


}




#endif