#include "io/io_uring_context.h"

#include <atomic>
#include <thread>
#include <gtest/gtest.h>

using namespace pensieve;

TEST(IoUringContext, InitAndTeardown) {
    EXPECT_NO_THROW({
        IoUringContext ctx(64);
    });
}

TEST(IoUringContext, SingleNop) {
    IoUringContext ctx(64);

    int32_t captured = -999;
    std::function<void(int32_t)> cb = [&](int32_t res) { captured = res; };
    Completion comp = Completion::from_callback(&cb);

    io_uring_sqe* sqe = ctx.get_sqe();
    ASSERT_NE(sqe, nullptr);
    io_uring_prep_nop(sqe);
    io_uring_sqe_set_data(sqe, &comp);

    ctx.submit_and_wait(1);

    io_uring_cqe* cqe = nullptr;
    ASSERT_EQ(ctx.peek_cqe(&cqe), 0);
    ASSERT_NE(cqe, nullptr);
    EXPECT_EQ(cqe->res, 0);
    ctx.seen_cqe(cqe);
}

TEST(IoUringContext, BatchNops) {
    IoUringContext ctx(64);

    constexpr int kCount = 16;
    int32_t results[kCount];
    std::function<void(int32_t)> callbacks[kCount];
    Completion completions[kCount];

    for (int i = 0; i < kCount; ++i) {
        results[i] = -999;
        callbacks[i] = [&results, i](int32_t res) { results[i] = res; };
        completions[i] = Completion::from_callback(&callbacks[i]);

        io_uring_sqe* sqe = ctx.get_sqe();
        ASSERT_NE(sqe, nullptr);
        io_uring_prep_nop(sqe);
        io_uring_sqe_set_data(sqe, &completions[i]);
    }

    ctx.submit_and_wait(kCount);

    io_uring_cqe* cqe = nullptr;
    int drained = 0;
    while (ctx.peek_cqe(&cqe) == 0) {
        EXPECT_EQ(cqe->res, 0);
        ctx.seen_cqe(cqe);
        ++drained;
    }
    EXPECT_EQ(drained, kCount);
}

TEST(IoUringContext, EventLoopProcessesNops) {
    IoUringContext ctx(64);

    std::atomic<int> completed{0};
    constexpr int kCount = 8;
    std::function<void(int32_t)> callbacks[kCount];
    Completion completions[kCount];

    for (int i = 0; i < kCount; ++i) {
        callbacks[i] = [&completed, &ctx, kCount](int32_t) {
            if (++completed >= kCount) {
                ctx.stop();
            }
        };
        completions[i] = Completion::from_callback(&callbacks[i]);

        io_uring_sqe* sqe = ctx.get_sqe();
        ASSERT_NE(sqe, nullptr);
        io_uring_prep_nop(sqe);
        io_uring_sqe_set_data(sqe, &completions[i]);
    }

    ctx.run();
    EXPECT_EQ(completed.load(), kCount);
}

TEST(IoUringContext, GetSqeFlushesWhenFull) {
    constexpr uint32_t kSmallDepth = 4;
    IoUringContext ctx(kSmallDepth);

    for (uint32_t i = 0; i < kSmallDepth + 1; ++i) {
        io_uring_sqe* sqe = ctx.get_sqe();
        ASSERT_NE(sqe, nullptr) << "Failed to get SQE at iteration " << i;
        io_uring_prep_nop(sqe);
        io_uring_sqe_set_data(sqe, nullptr);
    }

    ctx.submit_and_wait(1);

    int drained = 0;
    io_uring_cqe* cqe = nullptr;
    while (ctx.peek_cqe(&cqe) == 0) {
        ctx.seen_cqe(cqe);
        ++drained;
    }
    EXPECT_GE(drained, 1);
}
