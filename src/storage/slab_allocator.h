#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace pensieve {

class Arena {
    void* base_ = nullptr;
    size_t capacity_ = 0;
    bool hugepage_ = false;

public:
    explicit Arena(size_t capacity);
    ~Arena();

    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;
    Arena(Arena&&) = delete;
    Arena& operator=(Arena&&) = delete;

    void* base() const { return base_; }
    size_t capacity() const { return capacity_; }
    bool is_hugepage() const { return hugepage_; }
};

class SlabAllocator {
public:
    static constexpr size_t kPageSize = 1 << 20;  // 1 MB
    static constexpr size_t kMinSlotSize = 64;
    static constexpr size_t kMaxSlotSize = 1 << 20;  // 1 MB
    static constexpr size_t kNumClasses = 15;  // 64, 128, ... , 1MB

    explicit SlabAllocator(size_t total_size);

    void* allocate(size_t size);
    void deallocate(void* ptr, size_t size);

    size_t capacity() const { return arena_.capacity(); }
    size_t pages_total() const;
    size_t pages_used() const { return next_page_; }

    static size_t class_for_size(size_t size);
    static size_t slot_size_for_class(size_t cls);

private:
    void assign_page(size_t cls);

    Arena arena_;

    struct SizeClass {
        size_t slot_size;
        std::vector<void*> free_slots;
    };

    std::vector<SizeClass> classes_;
    size_t next_page_ = 0;
    std::mutex mu_;
};

}  // namespace pensieve
