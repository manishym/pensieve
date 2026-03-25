#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace pensieve {

struct SlabEntry {
    uint32_t key_size;
    uint32_t value_size;
    uint8_t  access_bit;
    uint8_t  reserved[7];

    std::string_view key() const {
        auto* data = reinterpret_cast<const char*>(this) + sizeof(SlabEntry);
        return {data, key_size};
    }

    std::string_view value() const {
        auto* data = reinterpret_cast<const char*>(this) + sizeof(SlabEntry) +
                     key_size;
        return {data, value_size};
    }

    static size_t required_size(size_t key_len, size_t val_len) {
        return sizeof(SlabEntry) + key_len + val_len;
    }

    static SlabEntry* init_at(void* ptr, std::string_view key,
                              std::string_view value) {
        auto* entry = static_cast<SlabEntry*>(ptr);
        entry->key_size = static_cast<uint32_t>(key.size());
        entry->value_size = static_cast<uint32_t>(value.size());
        entry->access_bit = 0;
        std::memset(entry->reserved, 0, sizeof(entry->reserved));

        auto* dst = static_cast<char*>(ptr) + sizeof(SlabEntry);
        std::memcpy(dst, key.data(), key.size());
        std::memcpy(dst + key.size(), value.data(), value.size());

        return entry;
    }
};

static_assert(sizeof(SlabEntry) == 16);

}  // namespace pensieve
