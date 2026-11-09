#pragma once

#include <cstdint>

namespace vnet::blackbox {

    /**
     * A token bucket: rate tokens arrive per second, at most burst are
     * held. take() succeeds when enough tokens are present. A rate of 0
     * means no limit. Time is passed in by the caller in nanoseconds, so
     * the bucket itself has no clock.
     */
    class TokenBucket {
    public:
        TokenBucket() = default;
        TokenBucket(uint64_t rate_per_sec, uint64_t burst);

        /**
         * Take n tokens at time now_ns. False when they are not there.
         * rate times 10 s and burst must stay below 1.8e10 (2^64 / 1e9).
         */
        bool take(uint64_t n, uint64_t now_ns);

        bool unlimited() const { return rate_ == 0; }
        uint64_t rate() const { return rate_; }

    private:
        uint64_t rate_    = 0;
        uint64_t burst_   = 0;
        uint64_t tokens_  = 0;
        uint64_t last_ns_ = 0;
        bool     started_ = false;
    };

};
