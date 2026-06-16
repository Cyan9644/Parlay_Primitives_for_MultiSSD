#ifndef SCAN_H
#define SCAN_H

// #include "utils/file_info.h"
// #include "utils/unordered_file_reader.h"
// #include "utils/unordered_file_writer.h"
#include "filter.h"
#include "parlay/primitives.h"
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


namespace plaidlayNaive{
    //simple scan for the flatten method
    template <typename T>
    naiveSeq<T> scan_inclusive(naiveSeq<T> input){
        naiveSeq<T> ret(input.size());
        ret[0] = input[0];
        if(input.size() == 1){
            return ret;
        }
        int i = 1;
        for(; i < input.size(); i++){
            ret[i] = input[i] + ret[i-1];
        }
        return ret;
    }


    template <typename T>
    naiveSeq<T> scan(naiveSeq<T> input){
        naiveSeq<T> ret(input.size());
        if(input.size() == 1){
            ret[0]=0;
            return ret;
        }
        else if(input.size()==2){
            ret[0]=0;
            ret[1]=input[0];
            return ret;
        }
        int i = 2;
    
        ret[0]= 0;
        ret[1] = input[0];
        for(; i < input.size(); i++){

            ret[i] = input[i-1] + ret[i-1];
        }
        return ret;
    }

    //parallel scan method based on the specifications from Parallel Block Delayed Sequences
    //do a scan on each block, do a scan on the offsets, and recalculate the block scans
    template <typename T>
    naiveSeq<T> block_scan(naiveSeq<T>& input){

        naiveSeq<T> output(input.size());

        bool remainder = input.size() % BLOCKSIZE;

        long num_blocks = input.size()/BLOCKSIZE;

        long size;
        
        if(remainder){
            size = num_blocks+1;
        }
        else{
            size = num_blocks;
        }
        std::vector<long> offsets(size);
        parlay::parallel_for(0, size, [&](long i){

            long block_start = i * BLOCKSIZE;

            long block_end = std::min((i+1) * BLOCKSIZE,(long)input.size()) ;

            output[block_start]=0;

            long r = block_start+1; //don't want to take information from beyond the block boundaries

            for(; r <block_end; r++){

            output[r] = output[r-1] + input[r-1];
            }

            offsets[i] = output[block_end-1] + input[block_end-1];

        });

        //get offsets scan
        std::exclusive_scan(offsets.begin(), offsets.end(), offsets.begin(), 0L);

        parlay::parallel_for(0, size, [&](long i){

            long block_start = i * BLOCKSIZE;

            long block_end = std::min((i+1) * BLOCKSIZE,(long)input.size());

            long r = block_start;

            for(; r< block_end; r++){

                output[r]+=offsets[i];

            }


        });

        return output;


    }

}

#endif
