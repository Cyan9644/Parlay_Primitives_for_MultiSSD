#ifndef EXTERNAL_FILTER_H
#define EXTERNAL_FILTER_H
#include <pthread.h>
#include "plaidlay.h"
#include <cassert>
#include <math.h>
#include <iostream>
#include <fcntl.h>
#include <sys/time.h>
#include <unistd.h>
#include <stdlib.h>
#include "chunk_header.h"
#include <liburing.h>
#include <cstring>
#include <parlay/parallel.h>
#include <parlay/primitives.h>


// #define NUM_READ_THREADS 30
#define NUM_SSDS 30


template<typename T>
External_Sequence ExternalFilter(External_Sequence &seq, const std::function<bool(const T)>& predicate, const std::string &new_filename) {
    std::vector<chunk_header> chunk_headers = getSeq(seq);
    UnorderedFileReader<T> reader;
    reader.PrepFiles(chunk_headers); //prepfiles needs to be changed to accomodate chunk headers
    reader.Start();

    //THIS EVENTUALLY NEEDS TO BE CHANGED
    //we are currently cheating a bit by not merging non-full blocks; this allows us to allocate the exact amount of space needed

    External_Sequence sequence = External_Sequence(seq.size());
    // std::vector<chunk_header>* chunk_header_arr = &((std::vector<chunk_header>*)sequence.ordered_underlying_sequence);
    std::vector<chunk_header>* chunk_header_arr = &sequence.ordered_underlying_sequence;
    constexpr size_t buffer_size_bytes = 4 << 20, buffer_size = buffer_size_bytes / sizeof(T);
    UnorderedFileWriter<T> writer;
    UnorderedWriterConfig wconfig;
    wconfig.num_threads = 1; //should perhaps change this, maybe not necessary because the I/O for filter needs to be sequential by chunk anyway
    writer.Start(std::vector<std::string>{new_filename}, wconfig);

    T* buffer;
    
    // unsigned long block_index = 0;
    size_t write_count = 0;
    while(reader.top() != nullptr){

    buffer = (T*)aligned_alloc(O_DIRECT_MEMORY_ALIGNMENT, buffer_size_bytes * NUM_SSDS);
    // std::vector<UnorderedFileReader<T>> readers;
    // parlay::parallel_for(0, readers.size(), [&](long i){
    //     readers[i].prepFiles()
    // });
    // parlay::parallel_for(block_index *NUM_SSDS, min((1 +block_index) * NUM_SSDS, chunk_headers.size()), [&]{
    
    // std::priority_queue<QueueData, std::vector<QueueData>, decltype(cmp)> queue(cmp);
    
    parlay::parallel_for(0, NUM_SSDS, [&](size_t i){
    
    size_t buffer_index = i * buffer_size;
    size_t next_index = 0;
    auto [ptr, size, _, index, which_chunk, filename] = reader.Poll(); //this poll can return an arbitrary chunk, so we're going to need to 
        //check the metadata to figure out what chunk it is
        //but the metadata isn't embedded in the data, so we can't read it.
        //the existing call uses things that aren't actually returned by reader.poll() like which_chunk 
        //because we expect that this will be implemented later.
        //if we knew what file we were reading from, we could check where we are to determine which chunk we must be in
        //but as far as I know the unordered 
    if (ptr == nullptr) {
        std::cout << "something went wrong or maybe not, null ptr";
        
        }
    else{
    
        size_t j = 0;
        size_t store = buffer_index;
        while (j < size) {
                if (predicate(ptr[j])) {
                    buffer[buffer_index] = ptr[j];
                    buffer_index++;
                }
                // if (buffer_size == buffer_index) {
                //     // writer.Push(std::shared_ptr<T>(buffer, free), buffer_size);
                //     // write_count++;
                //     buffer = (T*)aligned_alloc(O_DIRECT_MEMORY_ALIGNMENT, buffer_size_bytes);
                //     buffer_index = 0;
                // }
                j++;
            }
        chunk_header chunked; //this is intended to be the chunk header that will go into the final vector of chunk headers
        chunked.index = index;
        chunked.filename =new_filename; //we need to figure out what filename to write back to -- this should be given by the caller
        chunked.used = buffer_index-store;
        chunked.begin_address = chunk_headers[write_count * NUM_SSDS + i].get_begin_address(); //begin_address should be the offset into the file, which we can take from
        //the previous chunk

        // chunk_header_arr[write_count * NUM_SSDS + j]  = chunked;//get next position in the chunk header array
        (*chunk_header_arr)[write_count * NUM_SSDS + i] = chunked;
            // reader.allocator.Free(top.ptr);
            
        //now we assume that everything here has been processed into the final buffer
    }
        });

        //at this point, all threads have completed the parallel work and are ready to be written
        //we need to ensure a stable ordering, though, so we should sort them

        writer.Push(std::shared_ptr<T>(buffer, free), buffer_size * NUM_SSDS);
        write_count++;
        //the buffer will be reallocated at the top of the while loop
        // buffer = (T*)aligned_alloc(O_DIRECT_MEMORY_ALIGNMENT, buffer_size_bytes);
        // buffer_index = 0;
    }
    
    
    // size_t end_size = AlignUp(buffer_size * NUM_SSDS * sizeof(T) + METADATA_SIZE);
    // if (end_size > buffer_size_bytes * NUM_SSDS) {
    //     // rare situation where the size of the metadata exceeds sizeof(T), resulting
    //     // in insufficient buffer size
    //     buffer = (T*)realloc(buffer, end_size);
    // }
    MakeFileEndMarker((unsigned char *) buffer,
                      end_size,
                      buffer_size * NUM_SSDS * sizeof(T));
    writer.Push(std::shared_ptr<T>(buffer, free), end_size / sizeof(T));
    writer.Wait();
    // size_t file_size = (write_count + 1) * buffer_size_bytes * NUM_SSDS;
    // size_t true_size = write_count * buffer_size_bytes + buffer_index * NUM_SSDS * sizeof(T);
    std::sort(sequence.begin(), sequence.end(), [&](chunk_header i, chunk_header j){
        return i.index < j.index;
    });
    return sequence;
}



#endif
