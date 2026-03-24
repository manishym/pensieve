#include "storage/local_store.h"

#include <string>
#include <thread>
#include <vector>
#include <gtest/gtest.h>

using namespace pensieve;

static constexpr size_t k4MB = 4 * 1024 * 1024;

TEST(LocalStore, PutAndGet) {
    LocalStore store(k4MB);
    EXPECT_TRUE(store.put("hello", "world"));

    auto val = store.get("hello");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, "world");
}

TEST(LocalStore, GetMissReturnsNullopt) {
    LocalStore store(k4MB);
    EXPECT_FALSE(store.get("nonexistent").has_value());
}

TEST(LocalStore, OverwriteExistingKey) {
    LocalStore store(k4MB);
    EXPECT_TRUE(store.put("key", "v1"));
    EXPECT_TRUE(store.put("key", "v2"));

    auto val = store.get("key");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, "v2");
    EXPECT_EQ(store.size(), 1u);
}

TEST(LocalStore, DeleteExisting) {
    LocalStore store(k4MB);
    store.put("key", "val");
    EXPECT_TRUE(store.del("key"));
    EXPECT_FALSE(store.get("key").has_value());
    EXPECT_EQ(store.size(), 0u);
}

TEST(LocalStore, DeleteMissReturnsFalse) {
    LocalStore store(k4MB);
    EXPECT_FALSE(store.del("nonexistent"));
}

TEST(LocalStore, ManyKeys) {
    LocalStore store(k4MB);
    constexpr int kCount = 500;

    for (int i = 0; i < kCount; ++i) {
        std::string k = "key-" + std::to_string(i);
        std::string v = "val-" + std::to_string(i);
        EXPECT_TRUE(store.put(k, v));
    }

    EXPECT_EQ(store.size(), static_cast<size_t>(kCount));

    for (int i = 0; i < kCount; ++i) {
        std::string k = "key-" + std::to_string(i);
        auto val = store.get(k);
        ASSERT_TRUE(val.has_value()) << "Missing key: " << k;
        EXPECT_EQ(*val, "val-" + std::to_string(i));
    }
}

TEST(LocalStore, LargeValue) {
    LocalStore store(k4MB);
    std::string big_val(4096, 'x');
    EXPECT_TRUE(store.put("big", big_val));

    auto val = store.get("big");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, big_val);
}

TEST(LocalStore, SizeTracking) {
    LocalStore store(k4MB);
    EXPECT_EQ(store.size(), 0u);

    store.put("a", "1");
    EXPECT_EQ(store.size(), 1u);

    store.put("b", "2");
    EXPECT_EQ(store.size(), 2u);

    store.del("a");
    EXPECT_EQ(store.size(), 1u);
}

TEST(LocalStore, ConcurrentReadWrite) {
    LocalStore store(16 * 1024 * 1024, 16);
    constexpr int kWriters = 4;
    constexpr int kKeysPerWriter = 200;

    auto writer = [&](int id) {
        for (int i = 0; i < kKeysPerWriter; ++i) {
            std::string k = "w" + std::to_string(id) + "-" + std::to_string(i);
            store.put(k, "value");
        }
    };

    auto reader = [&](int id) {
        for (int i = 0; i < kKeysPerWriter; ++i) {
            std::string k = "w" + std::to_string(id) + "-" + std::to_string(i);
            store.get(k);  // may or may not find it
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < kWriters; ++i) {
        threads.emplace_back(writer, i);
    }
    for (int i = 0; i < kWriters; ++i) {
        threads.emplace_back(reader, i);
    }
    for (auto& t : threads) {
        t.join();
    }

    // All written keys should be readable.
    for (int id = 0; id < kWriters; ++id) {
        for (int i = 0; i < kKeysPerWriter; ++i) {
            std::string k =
                "w" + std::to_string(id) + "-" + std::to_string(i);
            auto val = store.get(k);
            EXPECT_TRUE(val.has_value()) << "Missing: " << k;
        }
    }
}

TEST(LocalStore, EvictionOnFullSlab) {
    // Small store: 2 MB, 2 shards.
    LocalStore store(2 * 1024 * 1024, 2);

    // Fill with entries until we approach capacity.
    // Each entry: 16 header + ~10 key + ~100 value = ~126 bytes -> 128B slot.
    // 2 MB / 128B = ~16K slots, but spread across shards, so use fewer.
    int stored = 0;
    for (int i = 0; i < 20000; ++i) {
        std::string k = "k" + std::to_string(i);
        std::string v(100, 'a');
        if (store.put(k, v)) {
            ++stored;
        } else {
            break;
        }
    }

    // Should have stored a significant number.
    EXPECT_GT(stored, 100);

    // After filling up, new puts should still succeed (via eviction).
    EXPECT_TRUE(store.put("fresh-key", std::string(100, 'z')));
    auto val = store.get("fresh-key");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, std::string(100, 'z'));
}
