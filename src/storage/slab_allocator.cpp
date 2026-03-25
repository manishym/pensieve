#include "storage/slab_allocator.h"

#include <bit>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <sys/mman.h>

namespace pensieve {

// ---------------------------------------------------------------------------
// Arena
// ---------------------------------------------------------------------------

Arena::Arena(size_t capacity) : capacity_(capacity) {
    if (capacity == 0) {
        throw std::invalid_argument("Arena capacity must be > 0");
    }

    int flags = MAP_PRIVATE | MAP_ANONYMOUS;

    // Try hugepages first.
    base_ = ::mmap(nullptr, capacity, PROT_READ | PROT_WRITE,
                   flags | MAP_HUGETLB, -1, 0);
    if (base_ != MAP_FAILED) {
        hugepage_ = true;
        return;
    }

    // Fallback to regular pages.
    base_ = ::mmap(nullptr, capacity, PROT_READ | PROT_WRITE, flags, -1, 0);
    if (base_ == MAP_FAILED) {
        throw std::runtime_error(
            std::string("mmap failed: ") + std::strerror(errno));
    }
    hugepage_ = false;
}

Arena::~Arena() {
    if (base_ && base_ != MAP_FAILED) {
        ::munmap(base_, capacity_);
    }
}

// ---------------------------------------------------------------------------
// SlabAllocator
// ---------------------------------------------------------------------------

SlabAllocator::SlabAllocator(size_t total_size) : arena_(total_size) {
    classes_.resize(kNumClasses);
    for (size_t i = 0; i < kNumClasses; ++i) {
        classes_[i].slot_size = kMinSlotSize << i;
    }
}

size_t SlabAllocator::pages_total() const {
    return arena_.capacity() / kPageSize;
}

size_t SlabAllocator::class_for_size(size_t size) {
    if (size <= kMinSlotSize) return 0;
    if (size > kMaxSlotSize) return kNumClasses;  // signals "too large"

    size_t power_of_2 = std::bit_ceil(size);
    return std::countr_zero(power_of_2) - std::countr_zero(kMinSlotSize);
}

size_t SlabAllocator::slot_size_for_class(size_t cls) {
    return kMinSlotSize << cls;
}

void SlabAllocator::assign_page(size_t cls) {
    if (next_page_ >= pages_total()) return;

    auto* page_base = static_cast<uint8_t*>(arena_.base()) +
                      next_page_ * kPageSize;
    ++next_page_;

    size_t slot_size = classes_[cls].slot_size;
    size_t slots_per_page = kPageSize / slot_size;

    auto& free_list = classes_[cls].free_slots;
    free_list.reserve(free_list.size() + slots_per_page);

    for (size_t i = 0; i < slots_per_page; ++i) {
        free_list.push_back(page_base + i * slot_size);
    }
}

void* SlabAllocator::allocate(size_t size) {
    size_t cls = class_for_size(size);
    if (cls >= kNumClasses) return nullptr;

    std::lock_guard lock(mu_);

    auto& sc = classes_[cls];
    if (sc.free_slots.empty()) {
        assign_page(cls);
    }
    if (sc.free_slots.empty()) {
        return nullptr;
    }

    void* slot = sc.free_slots.back();
    sc.free_slots.pop_back();
    return slot;
}

void SlabAllocator::deallocate(void* ptr, size_t size) {
    size_t cls = class_for_size(size);
    if (cls >= kNumClasses || ptr == nullptr) return;

    std::lock_guard lock(mu_);
    classes_[cls].free_slots.push_back(ptr);
}

}  // namespace pensieve
