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
    auto [is_init, handle] = wg.try_join("key1");
    EXPECT_TRUE(is_init);
    EXPECT_EQ(wg.pending_count(), 1u);
}

TEST(WaitGroup, SecondJoinReturnsFalse) {
    WaitGroup wg;
    auto [init1, h1] = wg.try_join("key1");
    auto [init2, h2] = wg.try_join("key1");
    EXPECT_TRUE(init1);
    EXPECT_FALSE(init2);
    EXPECT_EQ(wg.pending_count(), 1u);
    EXPECT_EQ(h1, h2);
}

TEST(WaitGroup, DifferentKeysAreIndependent) {
    WaitGroup wg;
    auto [a_init, ha] = wg.try_join("a");
    auto [b_init, hb] = wg.try_join("b");
    EXPECT_TRUE(a_init);
    EXPECT_TRUE(b_init);
    EXPECT_NE(ha, hb);
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

    auto [init1, h1] = wg.try_join("key1");
    ASSERT_TRUE(init1);
    auto [init2, h2] = wg.try_join("key1");
    ASSERT_FALSE(init2);

    auto waiter_coro = [&]() -> Task<> {
        waiter_result = co_await wg.wait(h2);
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

    auto [init_flag, init_handle] = wg.try_join("key1");
    ASSERT_TRUE(init_flag);

    std::vector<WaitGroup::FetchHandle> handles;
    for (int i = 0; i < kWaiters; ++i) {
        auto [is_init, h] = wg.try_join("key1");
        ASSERT_FALSE(is_init);
        handles.push_back(h);
    }

    auto make_waiter = [&](int i, WaitGroup::FetchHandle h) -> Task<> {
        results[i] = co_await wg.wait(std::move(h));
        if (++received >= kWaiters) ctx.stop();
    };

    std::vector<Task<>> tasks;
    for (int i = 0; i < kWaiters; ++i) {
        tasks.push_back(make_waiter(i, handles[i]));
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

    auto [init1, h1] = wg.try_join("key1");
    auto [init2, h2] = wg.try_join("key1");

    auto waiter_coro = [&]() -> Task<> {
        waiter_result = co_await wg.wait(h2);
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
    auto [init1, h1] = wg.try_join("key1");
    EXPECT_TRUE(init1);
    wg.complete("key1", "v1");
    auto [init2, h2] = wg.try_join("key1");
    EXPECT_TRUE(init2);
    EXPECT_EQ(wg.pending_count(), 1u);
}

TEST(WaitGroup, HandleSurvivesMapErasure) {
    // Verify the race condition fix: the handle remains valid even after
    // complete() erases the entry from the pending map.
    WaitGroup wg;

    auto [init1, h1] = wg.try_join("key1");
    auto [init2, h2] = wg.try_join("key1");
    ASSERT_FALSE(init2);

    // Simulate the race: complete before the waiter co_awaits.
    wg.complete("key1", "raced_value");
    EXPECT_EQ(wg.pending_count(), 0u);

    // The handle should still carry the result even though
    // complete() erased the map entry.
    auto awaitable = wg.wait(h2);
    EXPECT_TRUE(awaitable.await_ready());
    auto result = awaitable.await_resume();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "raced_value");
}
