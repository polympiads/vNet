#include "vnet/blackbox/ratelimit.hpp"

namespace vnet::blackbox {

    /* Tokens are kept scaled by one second in nanoseconds, so a refill is
     * rate * elapsed_ns with no rounding, and fractions carry over. */
    static const uint64_t SCALE = 1000000000ULL;
    /* Longer gaps add nothing beyond the burst and could overflow. */
    static const uint64_t MAX_GAP_NS = 10 * SCALE;

    TokenBucket::TokenBucket(uint64_t rate_per_sec, uint64_t burst)
        : rate_(rate_per_sec), burst_(burst * SCALE), tokens_(burst * SCALE) {}

    bool TokenBucket::take(uint64_t n, uint64_t now_ns) {
        if (rate_ == 0) return true;

        if (!started_) {
            started_ = true;
            last_ns_ = now_ns;
        } else if (now_ns > last_ns_) {
            uint64_t elapsed = now_ns - last_ns_;
            if (elapsed > MAX_GAP_NS) elapsed = MAX_GAP_NS;
            uint64_t refill = rate_ * elapsed;
            tokens_ = tokens_ + refill > burst_ ? burst_ : tokens_ + refill;
            last_ns_ = now_ns;
        }

        uint64_t cost = n * SCALE;
        if (tokens_ < cost) return false;
        tokens_ -= cost;
        return true;
    }

};
