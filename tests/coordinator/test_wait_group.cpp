#include "coordinator/wait_group.h"

#include <atomic>
#include <thread>

#include "io/awaitable.h"
#include "io/io_uring_context.h"
#include "io/task.h"

#include <gtest/gtest.h>

using namespace pensieve;

TEST(WaitGroup, InitiatorJoinsFirst) {
    WaitGroup wg;
    EXPECT_TRUE(wg.try_join("key1"));
    EXPECT_EQ(wg.pending_count(), 1u);
}

TEST(WaitGroup, SecondJoinReturnsFalse) {
    WaitGroup wg;
    EXPECT_TRUE(wg.try_join("key1"));
    EXPECT_FALSE(wg.try_join("key1"));
    EXPECT_EQ(wg.pending_count(), 1u);
}

TEST(WaitGroup, DifferentKeysAreIndependent) {
    WaitGroup wg;
    EXPECT_TRUE(wg.try_join("a"));
    EXPECT_TRUE(wg.try_join("b"));
    EXPECT_EQ(wg.pending_count(), 2u);
}

TEST(WaitGroup, CompleteRemovesPending) {
    WaitGroup wg;
    wg.try_join("key1");
    wg.complete("key1", "value1");
    EXPECT_EQ(wg.pending_count(), 0u);
}

TEST(WaitGroup, CompleteUnknownKeyIsNoOp) {
    WaitGroup wg;
    wg.complete("nonexistent", std::nullopt);
    EXPECT_EQ(wg.pending_count(), 0u);
}

TEST(WaitGroup, WaiterGetsResult) {
    IoUringContext ctx(64);
    WaitGroup wg;
    std::optional<std::string> waiter_result;

    wg.try_join("key1");

    auto waiter_coro = [&]() -> Task<> {
        waiter_result = co_await wg.wait("key1");
        ctx.stop();
    };

    auto initiator_coro = [&]() -> Task<> {
        co_await async_nop(ctx);
        wg.complete("key1", "result_value");
    };

    auto waiter_task = waiter_coro();
    auto init_task = initiator_coro();

    waiter_task.start();
    init_task.start();
    ctx.run();

    ASSERT_TRUE(waiter_result.has_value());
    EXPECT_EQ(*waiter_result, "result_value");
}

TEST(WaitGroup, MultipleWaitersGetResult) {
    IoUringContext ctx(64);
    WaitGroup wg;
    constexpr int kWaiters = 5;
    std::atomic<int> received{0};
    std::vector<std::optional<std::string>> results(kWaiters);

    wg.try_join("key1");

    // Lambda must outlive all coroutines created from it because the
    // coroutine frame stores a this-pointer into the lambda object.
    auto make_waiter = [&](int i) -> Task<> {
        results[i] = co_await wg.wait("key1");
        if (++received >= kWaiters) ctx.stop();
    };

    std::vector<Task<>> tasks;
    for (int i = 0; i < kWaiters; ++i) {
        tasks.push_back(make_waiter(i));
    }

    auto initiator_coro = [&]() -> Task<> {
        co_await async_nop(ctx);
        wg.complete("key1", "shared_value");
    };

    for (auto& t : tasks) t.start();
    auto init_task = initiator_coro();
    init_task.start();
    ctx.run();

    EXPECT_EQ(received.load(), kWaiters);
    for (int i = 0; i < kWaiters; ++i) {
        ASSERT_TRUE(results[i].has_value()) << "waiter " << i;
        EXPECT_EQ(*results[i], "shared_value") << "waiter " << i;
    }
}

TEST(WaitGroup, WaiterGetsNulloptOnMiss) {
    IoUringContext ctx(64);
    WaitGroup wg;
    std::optional<std::string> waiter_result;
    bool waiter_ran = false;

    wg.try_join("key1");

    auto waiter_coro = [&]() -> Task<> {
        waiter_result = co_await wg.wait("key1");
        waiter_ran = true;
        ctx.stop();
    };

    auto initiator_coro = [&]() -> Task<> {
        co_await async_nop(ctx);
        wg.complete("key1", std::nullopt);
    };

    auto waiter_task = waiter_coro();
    auto init_task = initiator_coro();
    waiter_task.start();
    init_task.start();
    ctx.run();

    EXPECT_TRUE(waiter_ran);
    EXPECT_FALSE(waiter_result.has_value());
}

TEST(WaitGroup, RejoinAfterComplete) {
    WaitGroup wg;
    EXPECT_TRUE(wg.try_join("key1"));
    wg.complete("key1", "v1");
    EXPECT_TRUE(wg.try_join("key1"));
    EXPECT_EQ(wg.pending_count(), 1u);
}
