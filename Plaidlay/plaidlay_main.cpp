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


// #define num_blocks 10
// #define block_size 512 * 512 * 4 //1 GB
// #define target block_size * num_blocks //10 GB


#define QDEPTH 32



void sum_example(int n);
void primes(int n);
void scan(int n);


//taken from SSD write code
double get_time_diff(struct timeval start, struct timeval end) {
    return (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000.0;
}


bool test_seq_io(naiveSeq<T> sequence, int seqlen, const char* filepath);
int main() {
    sum_example(100);
    scan(100);
    primes(20);
    test_seq_io()
    return 0;
}
void sum_example(int n) {
    // basic example: sum_(i=1)^n i^2 = (n(n+1)(2n+1))/6
    // for now I think it's chill to just switch out the interfaces with namespaces
    using namespace plaidlayNaive;
    auto mySeq = tabulate<int>(n, [](int i){return i + 1;});
    auto squares = map(mySeq, [](int val) {return val * val;});
    // the interface here is a little different
    int res = reduce(squares, [](int a, int b) {return a + b;}, 0);
    int expect = n * (n + 1) * (2 * n + 1) / 6;
    assert(res == expect && "whoops! this sum is wrong!");
}
// print out the primes up to n, naively not with sieve
void primes(int n) {
    using namespace plaidlayNaive;
    auto nums = tabulate<int>(n, [](int i) {return i + 1;});
    auto primes = filter(nums, [](int val){
        for (int i = 2; i <= sqrt(val); i++) {
            if (val % i == 0) return false;
        }
        return true;
    });
    std::cout << primes << std::endl;
}
void scan(int n) {
    using namespace plaidlayNaive;
    auto mySeq = tabulate<int>(n, [](int i){return i + 1;});
    auto [sums, tot] = scan(mySeq, [](int a, int b) {return a + b;}, 0);
    for (int i = 1; i < n; i++) {
        assert(sums[i] == i*(i+1)/2);
    }
    assert(tot == n*(n+1)/2);
}


//found on stackoverflow, credit: Mark Ransom
int roundUp(int numToRound, int multiple)
{
    if (multiple == 0)
        return numToRound;

    int remainder = numToRound % multiple;
    if (remainder == 0)
        return numToRound;

    return numToRound + multiple - remainder;
}





template <typename T>
bool test_seq_io(naiveSeq<T> sequence, int seqlen, const char* filepath){
    using namespace plaidlayNaive;


auto qdepth = QDEPTH;
auto block_size = 512; //512 bytes
auto target_size = roundUp(seqlen * sizeof(sequence[0]), block_size);

plaidlayNaive::naiveSeq<int> check_sequence(seqlen);

for(int i = 0;i < seqlen; i++){
    check_sequence[i] = sequence[i];
}

if(filepath == NULL){
    filepath = "Parlay_Primitives_for_MultiSSD/bogus.txt";
}

int filedes = open(filepath, O_WRONLY | O_DIRECT | O_CREAT | O_TRUNC, 0644);
if(filedes < 0){
    std::cout << "file descriptor did not work";
    goto cleanup_bad;

}

char* buffer;

struct io_uring ring;

struct timeval start, end;


//result == seqlen ? std::cout << "all good on the O_DIRECT open" : std::cout << "Something went wrong with the O_DIRECT open";

// if(result != seqlen){
//     close(filedes);
//     goto cleanup_bad;
// }




if(posix_memalign((void**)(&buffer), 512, target_size)){

    goto cleanup_bad;
}

std::memset(buffer, 0x0, target_size); //it's rounded up to the nearest block_size

// for(int i = 0; i < seqlen; i++){

//     buffer[i] = sequence[i];
// }

std::memcpy(buffer, &sequence[0], seqlen *sizeof(sequence[0]));


if((io_uring_queue_init(qdepth, &ring, 0)) < 0){
std::cout << "io_uring init went wrong";
goto cleanup_bad;
}

long long increment = 0;
unsigned long submitted = 0;
unsigned long completed = 0;
unsigned long needed = target_size / block_size;
unsigned curr_qdepth = 0;

gettimeofday(&start, NULL);



while(completed < needed){

    while(submitted < needed && curr_qdepth < qdepth){//!(qdepth > submitted - completed)){


struct io_uring_sqe* sqe;

sqe = io_uring_get_sqe(&ring);
// int bogus;
// !sqe ? break : bogus = 1; //std::cout << "sqe acquired";
if(!sqe) break;

io_uring_prep_write(sqe, filedes, buffer+increment, block_size, increment);
increment+=block_size;
submitted++;
curr_qdepth++;

    }
    io_uring_submit(&ring);
    struct io_uring_cqe* cqe;

    auto r = io_uring_wait_cqe(&ring, &cqe);
    if(r < 0){
        std::cout << "issue in waiting for a cqe";
        goto cleanup_bad;
    }
    else if(cqe->res < 0){
        std::cout << "issue with asynch write";
        goto cleanup_bad;
    }
    curr_qdepth--;
    
    completed++;
    io_uring_cqe_seen(&ring, cqe);
    while(!io_uring_peek_cqe(&ring, &cqe)){
        if((cqe->res) >0 && cqe->res == block_size){
        io_uring_cqe_seen(&ring, cqe);
        completed++;
        curr_qdepth--;
        }
        else{
            std::cout << "issue with asynch write with peek";
            goto cleanup_bad;
        }

    }








}

gettimeofday(&end, NULL);

std::cout << "writes completed, wrote " << target_size << " elements in " << get_time_diff(start,end) << " time ";

 double mb_written = target_size / (1024.0 * 1024.0);
double bandwidth = mb_written / get_time_diff(start,end) ;

printf("Bandwidth: %f", bandwidth);
printf("mb written: %f", mb_written);


close(filedes);

std::cout << "INITIATING READ BACK FROM DISK";
 
filedes = open(filepath, O_RDONLY | O_DIRECT);
if(filedes < 0){
    std::cout << "file descriptor did not work on read";
    goto cleanup_bad;

}

struct timeval start_read, end_read;

gettimeofday(&start_read, NULL);



std::memset(buffer, 0x0, target_size);
completed = 0;
needed = target_size / block_size;
curr_qdepth = 0;
increment = 0;
submitted = 0;



while(completed < needed){

    while(submitted < needed && curr_qdepth < qdepth){//!(qdepth > submitted - completed)){


struct io_uring_sqe* sqe;

sqe = io_uring_get_sqe(&ring);
// int bogus;
// !sqe ? break : bogus = 1; //std::cout << "sqe acquired";
if(!sqe) break;

io_uring_prep_read(sqe, filedes, buffer+increment, block_size, increment);
increment+=block_size;
submitted++;
curr_qdepth++;

    }
    io_uring_submit(&ring);
    struct io_uring_cqe* cqe;

    auto r = io_uring_wait_cqe(&ring, &cqe);
    if(r < 0){
        std::cout << "issue in waiting for a cqe in read";
        goto cleanup_bad;
    }
    else if(cqe->res < 0){
        std::cout << "issue with asynch read";
        goto cleanup_bad;
    }
    curr_qdepth--;
    
    completed++;
    io_uring_cqe_seen(&ring, cqe);
    while(!io_uring_peek_cqe(&ring, &cqe)){
        if((cqe->res) >0 && cqe->res == block_size){
        io_uring_cqe_seen(&ring, cqe);
        completed++;
        curr_qdepth--;
        }
        else{
            std::cout << "issue with asynch read with peek";
            goto cleanup_bad;
        }

    }








}






gettimeofday(&end_read, NULL);

std::cout << "writes completed, wrote " << target_size << " elements in " << get_time_diff(start_read,end_read) << " time ";

 double mb_read = target_size / (1024.0 * 1024.0);
bandwidth = mb_read/ get_time_diff(start_read,end_read) ;

printf("Bandwidth: %f", bandwidth);
printf("mb read: %f", mb_read);


T* buffered = (T*) buffer;

for(int i =0; i < seqlen; i++){
    assert(buffered[i] == check_sequence[i]);
}

//now we want to read back from the file



std::cout << "finished correctly";






cleanup:

    io_uring_queue_exit(&ring);
    free(buffer);
    close(filedes);
    unlink(filepath); 
    return 1;


cleanup_bad:
    try{
    close(filedes);
    io_uring_queue_exit(&ring);
    free(buffer);
    unlink(filepath); 
    throw 401;
    }
    catch(int e){
        if(e == 401)
            std::cout << "error in handling";
        else{
            std::cout << "UNK";
        }
    }

    return 0;




}


