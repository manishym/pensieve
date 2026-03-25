#pragma once

#include <cstddef>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/types.h"
#include "storage/clock_evictor.h"
#include "storage/slab_allocator.h"
#include "storage/slab_entry.h"

namespace pensieve {

struct StringHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view sv) const {
        return std::hash<std::string_view>{}(sv);
    }
};

struct alignas(kCacheLineSize) Shard {
    std::mutex mu;
    std::unordered_map<std::string, SlabEntry*, StringHash, std::equal_to<>>
        map;
    ClockEvictor evictor;
};

class LocalStore {
    SlabAllocator allocator_;
    std::vector<Shard> shards_;

    static constexpr size_t kMaxEvictAttempts = 8;

    size_t shard_for(std::string_view key) const;

public:
    explicit LocalStore(size_t total_memory, size_t num_shards = 16);

    std::optional<std::string> get(std::string_view key);
    bool put(std::string_view key, std::string_view value);
    bool del(std::string_view key);

    size_t size() const;
    size_t memory_used() const;
};

}  // namespace pensieve
