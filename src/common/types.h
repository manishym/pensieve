#pragma once

#include <cstddef>
#include <cstdint>

namespace pensieve {

using fd_t = int;

constexpr uint32_t kDefaultQueueDepth = 256;
constexpr size_t   kCacheLineSize     = 64;

}  // namespace pensieve
