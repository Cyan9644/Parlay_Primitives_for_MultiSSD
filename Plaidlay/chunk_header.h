#ifndef CHUNK_HEADER_H
#define CHUNK_HEADER_H

#include <pthread.h>
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
#include <string>
#include <vector>


struct chunk_header{

std::string filename;
size_t begin_address;
size_t used;
size_t index;


};

std::string get_filename(chunk_header* chunk);

size_t get_begin_address(chunk_header* chunk);
size_t get_bytes_used(chunk_header* chunk);
size_t get_index(chunk_header* chunk);


struct External_Sequence{

    std::vector<std::vector<chunk_header>> underlying_sequence;
};



std::vector<std::vector<chunk_header>>& getSeq(External_Sequence* seq);
y


#endif