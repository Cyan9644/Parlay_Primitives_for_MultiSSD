#ifndef SCAN_H
#define SCAN_H

#include "utils/file_info.h"
#include "utils/unordered_file_reader.h"
#include "utils/unordered_file_writer.h"

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



//simple scan for the flatten method
naiveSeq<T> scan_inclsuive(naiveSeq<T> input){
    int i = 1;
    naiveSeq<T> ret(input.size());
    ret[0] = input[0];
    for(; i < input.size(); i++){

        ret[i] = input[i] + ret[i-1];
    }
    return ret;
}



naiveSeq<T> scan(naiveSeq<T> input){
    int i = 2;
    naiveSeq<T> ret(input.size());
  
    ret[0]= 0;
    ret[1] = input[0];
    for(; i < input.size(); i++){

        ret[i] = input[i-1] + ret[i-1];
    }
    return ret;
}






#endif
