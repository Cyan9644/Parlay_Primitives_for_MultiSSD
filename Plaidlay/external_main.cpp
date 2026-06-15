#include "externalSeq.h"

int main() {
    // later on we could use macros to enforce file name matching identifier?
    externalSeq<int> nums = plaidlay::randPerm<int>("nums", 26);
    return 0;
}
