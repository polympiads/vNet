#include <gtest/gtest.h>
#include "vnet/blackbox/ratelimit.hpp"

using namespace vnet::blackbox;

static const uint64_t SEC = 1000000000ULL;

TEST(TokenBucket, UnlimitedTakesEverything) {
    TokenBucket b(0, 0);
    EXPECT_TRUE(b.unlimited());
    for (int i = 0; i < 1000; i++) EXPECT_TRUE(b.take(1000000, 0));
}

TEST(TokenBucket, BurstThenRefill) {
    TokenBucket b(10, 10);          // 10 per second, burst 10
    uint64_t t = 5 * SEC;
    for (int i = 0; i < 10; i++) EXPECT_TRUE(b.take(1, t));
    EXPECT_FALSE(b.take(1, t));     // burst spent

    EXPECT_FALSE(b.take(1, t + SEC / 20));   // 50 ms: 0.5 token, not yet
    EXPECT_TRUE(b.take(1, t + SEC / 10));    // 100 ms: 1 token
    EXPECT_FALSE(b.take(1, t + SEC / 10));

    // After a long pause only burst tokens are available.
    EXPECT_TRUE(b.take(10, t + 100 * SEC));
    EXPECT_FALSE(b.take(1, t + 100 * SEC));
}

TEST(TokenBucket, BytesAtHighRate) {
    // 25 MB/s with a 25 MB burst, as the switch sets for 200 Mbit/s.
    uint64_t rate = 25000000;
    TokenBucket b(rate, rate);
    uint64_t t = 0;
    uint64_t taken = 0;
    while (b.take(1500, t)) taken += 1500;
    EXPECT_GE(taken, rate - 1500);
    EXPECT_LE(taken, rate);

    // One millisecond later exactly 25000 more bytes have arrived.
    uint64_t remaining = rate - taken;
    EXPECT_TRUE(b.take(remaining + 25000, t + SEC / 1000));
    EXPECT_FALSE(b.take(1, t + SEC / 1000));
}

TEST(TokenBucket, TimeGoingBackwardsIsIgnored) {
    TokenBucket b(10, 10);
    EXPECT_TRUE(b.take(10, 5 * SEC));
    EXPECT_FALSE(b.take(1, 4 * SEC));
    EXPECT_FALSE(b.take(1, 5 * SEC));
    EXPECT_TRUE(b.take(1, 5 * SEC + SEC / 10));
}
