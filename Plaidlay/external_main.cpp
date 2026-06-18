#include "externalSeq.h"

int main() {
    auto add = [](size_t a, size_t b) {return a + b;};
    // later on we could use macros to enforce file name matching identifier?
    externalSeq<size_t> nums = externalSeqOps::randPerm<size_t>("nums", 9);
    // TODO: for some reason I need to provide that both are long long for the generic here otherwise I get negatives, probably overflow
    // okay size_t just works for some reason here
    std::cout << externalSeqOps::reduce<>(nums, add, 0) << std::endl;
    externalSeq<size_t> halved = externalSeqOps::map<size_t, size_t>(nums, "halved", [](size_t x) { return x / 2; });
    std::cout << externalSeqOps::reduce<>(halved, add, 0) << std::endl;
    externalSeq<size_t> modTen = externalSeqOps::filter<>(nums, "modTen", [](size_t a) {return a % 10 == 0;});
    std::cout << externalSeqOps::reduce<>(modTen, add, 0) << std::endl;
    return 0;
}
