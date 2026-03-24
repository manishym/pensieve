#include "io/buffer_pool.h"
#include "io/io_uring_context.h"

#include <cstring>
#include <gtest/gtest.h>

using namespace pensieve;

TEST(BufferPool, AcquireAll) {
    IoUringContext ctx(64);
    constexpr size_t kSize = 4096;
    constexpr size_t kCount = 8;
    BufferPool pool(ctx, kSize, kCount);

    EXPECT_EQ(pool.count(), kCount);
    EXPECT_EQ(pool.available(), kCount);

    std::vector<BufferPool::BufferHandle> handles;
    for (size_t i = 0; i < kCount; ++i) {
        auto h = pool.acquire();
        ASSERT_TRUE(h.has_value()) << "acquire failed at i=" << i;
        EXPECT_EQ(h->size, kSize);
        EXPECT_NE(h->data, nullptr);
        handles.push_back(*h);
    }

    EXPECT_EQ(pool.available(), 0u);
    EXPECT_FALSE(pool.acquire().has_value());
}

TEST(BufferPool, ReleaseAndReacquire) {
    IoUringContext ctx(64);
    BufferPool pool(ctx, 4096, 4);

    auto h1 = pool.acquire();
    ASSERT_TRUE(h1.has_value());
    uint16_t idx = h1->index;

    pool.release(idx);
    EXPECT_EQ(pool.available(), 4u);

    auto h2 = pool.acquire();
    ASSERT_TRUE(h2.has_value());
    EXPECT_EQ(h2->index, idx);
}

TEST(BufferPool, BuffersAreDistinct) {
    IoUringContext ctx(64);
    constexpr size_t kSize = 1024;
    constexpr size_t kCount = 4;
    BufferPool pool(ctx, kSize, kCount);

    std::vector<BufferPool::BufferHandle> handles;
    for (size_t i = 0; i < kCount; ++i) {
        handles.push_back(*pool.acquire());
    }

    for (size_t i = 0; i < kCount; ++i) {
        for (size_t j = i + 1; j < kCount; ++j) {
            EXPECT_NE(handles[i].data, handles[j].data);
            EXPECT_NE(handles[i].index, handles[j].index);
        }
    }
}

TEST(BufferPool, DataIntegrity) {
    IoUringContext ctx(64);
    constexpr size_t kSize = 256;
    BufferPool pool(ctx, kSize, 2);

    auto h = pool.acquire();
    ASSERT_TRUE(h.has_value());

    const char msg[] = "pensieve buffer test";
    std::memcpy(h->data, msg, sizeof(msg));
    EXPECT_STREQ(reinterpret_cast<char*>(h->data), msg);

    pool.release(h->index);

    auto h2 = pool.acquire();
    ASSERT_TRUE(h2.has_value());
    EXPECT_EQ(h2->index, h->index);
    EXPECT_STREQ(reinterpret_cast<char*>(h2->data), msg);
}
