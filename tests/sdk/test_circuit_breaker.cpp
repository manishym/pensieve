#include "sdk/circuit_breaker.h"

#include <gtest/gtest.h>
#include <thread>

using namespace pensieve;

static NodeEndpoint ep(const std::string& host, uint16_t port) {
    return {host, 0, port};
}

TEST(CircuitBreaker, StartsAllowing) {
    CircuitBreaker cb;
    EXPECT_TRUE(cb.allow(ep("127.0.0.1", 11211)));
    EXPECT_EQ(cb.open_count(), 0u);
}

TEST(CircuitBreaker, OpensAfterThreshold) {
    CircuitBreaker::Config cfg;
    cfg.failure_threshold = 3;
    cfg.open_duration = std::chrono::milliseconds{500};
    CircuitBreaker cb(cfg);

    auto node = ep("10.0.0.1", 11211);
    cb.record_failure(node);
    cb.record_failure(node);
    EXPECT_TRUE(cb.allow(node));

    cb.record_failure(node);
    EXPECT_FALSE(cb.allow(node));
    EXPECT_EQ(cb.open_count(), 1u);
}

TEST(CircuitBreaker, TransitionsToHalfOpenAfterTimeout) {
    CircuitBreaker::Config cfg;
    cfg.failure_threshold = 1;
    cfg.open_duration = std::chrono::milliseconds{50};
    CircuitBreaker cb(cfg);

    auto node = ep("10.0.0.1", 11211);
    cb.record_failure(node);
    EXPECT_FALSE(cb.allow(node));

    std::this_thread::sleep_for(std::chrono::milliseconds{80});
    EXPECT_TRUE(cb.allow(node));
}

TEST(CircuitBreaker, SuccessCloses) {
    CircuitBreaker::Config cfg;
    cfg.failure_threshold = 1;
    cfg.open_duration = std::chrono::milliseconds{10};
    CircuitBreaker cb(cfg);

    auto node = ep("10.0.0.1", 11211);
    cb.record_failure(node);
    std::this_thread::sleep_for(std::chrono::milliseconds{20});

    EXPECT_TRUE(cb.allow(node));
    cb.record_success(node);
    EXPECT_TRUE(cb.allow(node));
    EXPECT_EQ(cb.open_count(), 0u);
}

TEST(CircuitBreaker, IndependentPerNode) {
    CircuitBreaker::Config cfg;
    cfg.failure_threshold = 1;
    CircuitBreaker cb(cfg);

    auto a = ep("10.0.0.1", 11211);
    auto b = ep("10.0.0.2", 11211);

    cb.record_failure(a);
    EXPECT_FALSE(cb.allow(a));
    EXPECT_TRUE(cb.allow(b));
}
