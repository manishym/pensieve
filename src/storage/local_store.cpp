#include "storage/local_store.h"

#include "hash/hasher.h"

namespace pensieve {

LocalStore::LocalStore(size_t total_memory, size_t num_shards)
    : allocator_(total_memory),
      shards_(num_shards) {}

size_t LocalStore::shard_for(std::string_view key) const {
    return hash_key(key) % shards_.size();
}

std::optional<std::string> LocalStore::get(std::string_view key) {
    auto& shard = shards_[shard_for(key)];
    std::lock_guard lock(shard.mu);

    auto it = shard.map.find(key);
    if (it == shard.map.end()) return std::nullopt;

    SlabEntry* entry = it->second;
    entry->access_bit = 1;
    return std::string(entry->value());
}

bool LocalStore::put(std::string_view key, std::string_view value) {
    size_t alloc_size = SlabEntry::required_size(key.size(), value.size());
    auto& shard = shards_[shard_for(key)];
    std::lock_guard lock(shard.mu);

    auto it = shard.map.find(key);
    if (it != shard.map.end()) {
        SlabEntry* old = it->second;
        size_t old_size =
            SlabEntry::required_size(old->key_size, old->value_size);
        shard.evictor.untrack(old);
        allocator_.deallocate(old, old_size);
        shard.map.erase(it);
    }

    void* ptr = nullptr;
    for (size_t attempt = 0; attempt <= kMaxEvictAttempts; ++attempt) {
        ptr = allocator_.allocate(alloc_size);
        if (ptr) break;

        SlabEntry* victim = shard.evictor.evict_one();
        if (!victim) break;

        size_t victim_size =
            SlabEntry::required_size(victim->key_size, victim->value_size);
        auto vit = shard.map.find(victim->key());
        if (vit != shard.map.end()) shard.map.erase(vit);
        allocator_.deallocate(victim, victim_size);
    }

    if (!ptr) return false;

    SlabEntry* entry = SlabEntry::init_at(ptr, key, value);
    shard.map.emplace(std::string(key), entry);
    shard.evictor.track(entry);
    return true;
}

bool LocalStore::del(std::string_view key) {
    auto& shard = shards_[shard_for(key)];
    std::lock_guard lock(shard.mu);

    auto it = shard.map.find(key);
    if (it == shard.map.end()) return false;

    SlabEntry* entry = it->second;
    size_t entry_size =
        SlabEntry::required_size(entry->key_size, entry->value_size);
    shard.evictor.untrack(entry);
    allocator_.deallocate(entry, entry_size);
    shard.map.erase(it);
    return true;
}

size_t LocalStore::size() const {
    size_t total = 0;
    for (auto& shard : shards_) {
        std::lock_guard lock(
            const_cast<std::mutex&>(shard.mu));
        total += shard.map.size();
    }
    return total;
}

size_t LocalStore::memory_used() const {
    return allocator_.pages_used() * SlabAllocator::kPageSize;
}

}  // namespace pensieve
