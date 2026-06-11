#include "plaidlay.h"
#include <cassert>
#include <math.h>

void sum_example(int n);
void primes(int n);
void scan(int n);

int main() {
    sum_example(100);
    scan(100);
    primes(20);
    return 0;
}
void sum_example(int n) {
    // basic example: sum_(i=1)^n i^2 = (n(n+1)(2n+1))/6
    // for now I think it's chill to just switch out the interfaces with namespaces
    using namespace plaidlayNaive;
    auto mySeq = tabulate<int>(n, [](int i){return i + 1;});
    auto squares = map(mySeq, [](int val) {return val * val;});
    // the interface here is a little different
    int res = reduce(squares, 0, [](int a, int b) {return a + b;});
    int expect = n * (n + 1) * (2 * n + 1) / 6;
    assert(res == expect && "whoops! this sum is wrong!");
}
// print out the primes up to n, naively not with seive
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
    auto [sums, tot] = scan(mySeq, 0, [](int a, int b) {return a + b;});
    for (int i = 1; i < n; i++) {
        assert(sums[i] == i*(i+1)/2);
    }
    assert(tot == n*(n+1)/2);
}
