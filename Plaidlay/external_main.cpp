#include "externalSeq.h"

int main() {
    // later on we could use macros to enforce file name matching identifier?
    externalSeq<long long> nums = externalSeqOps::randPerm<long long>("nums", 24);
    // externalSeq<int> mapped = externalSeqOps::map<int, int>(nums, "mapped", [](int x) { return x * 2; });
    std::cout << externalSeqOps::reduce<long long, long long>(nums, [](long long a, long long b) { return a + b; }, 0) << std::endl;
    // externalSeq<long long> doubled = externalSeqOps::map<long long, long long>(nums, "doubled", [](long long x) { return x * 2; });
    // std::cout << externalSeqOps::reduce<long long>(doubled, [](long long a, long long b) { return a + b; }, 0) << std::endl;
    return 0;
}
