#include <parlay/parallel.h>
#include <parlay/primitives.h>
#include <string>
#include <vector>

std::string get_filename(chunk_header* chunk){
    return chunk->filename;
}
size_t get_begin_address(chunk_header* chunk){
    return chunk->begin_address;
}
size_t get_bytes_used(chunk_header* chunk){
    return chunk->used;
}
size_t get_index(chunk_header* chunk){
    return chunk->index;
}



std::vector<std::vector<chunk_header>>& getSeq(External_Sequence* seq){
    return seq->underlying_sequence;
}


