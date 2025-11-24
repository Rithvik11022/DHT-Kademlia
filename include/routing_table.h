#pragma once
#include "node.h"
#include <vector>
#include <deque>
#include <mutex>

// very small routing table + k-bucket LRU
class RoutingTable {
public:
    RoutingTable(const NodeID &self);
    void update_contact(const NodeInfo &n);
    std::vector<NodeInfo> find_closest(const NodeID &target, size_t count) const;
private:
    NodeID _self;
    static constexpr size_t ID_BITS = ID_BYTES * 8;
    std::vector<std::deque<NodeInfo>> buckets; // simple LRU deque per bucket
    mutable std::mutex _mu;
    int bucket_index(const NodeID &id) const;
};