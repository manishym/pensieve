#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "hash/murmurhash3.h"
#include "membership/node_info.h"

namespace pensieve {

constexpr uint32_t kDefaultHashSeed = 0x5EED;

inline uint32_t hash_key(std::string_view key) {
    uint32_t out = 0;
    murmurhash3_x86_32(key.data(), key.size(), kDefaultHashSeed, &out);
    return out;
}

inline uint32_t hash_node_token(const NodeId& node, uint32_t vnode_index) {
    std::string canonical = node.host + ":" +
                            std::to_string(node.gossip_port) + "#" +
                            std::to_string(vnode_index);
    uint32_t out = 0;
    murmurhash3_x86_32(canonical.data(), canonical.size(), kDefaultHashSeed,
                       &out);
    return out;
}

}  // namespace pensieve
