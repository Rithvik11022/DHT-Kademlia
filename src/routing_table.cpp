#include "routing_table.h"
#include <iostream>
#include <algorithm>
#include <unordered_map>

RoutingTable::RoutingTable(const NodeID &self): _self(self), buckets(ID_BYTES * 8) {}

int RoutingTable::bucket_index(const NodeID &id) const {
    return _self.prefix_len_to(id);
}

void RoutingTable::update_contact(const NodeInfo &n) {
    int idx = bucket_index(n.id);
    if (idx < 0 || idx >= (int)buckets.size()) return;
    std::lock_guard<std::mutex> lk(_mu);
    auto &dq = buckets[idx];
    std::cout << buckets.size() << " here " << std::endl ; 
    // remove if exists
    dq.erase(std::remove_if(dq.begin(), dq.end(), [&](const NodeInfo &x){ return x.id == n.id; }), dq.end());
    dq.push_front(n);
    // cap
    if (dq.size() > 20) dq.pop_back();
}

std::vector<NodeInfo> RoutingTable::find_closest(const NodeID &target, size_t count) const {
    std::vector<NodeInfo> all;
    {
        std::lock_guard<std::mutex> lk(_mu); // bucket -> vector<deque> 
        for (auto &dq : buckets) {
            for (auto &ni : dq) all.push_back(ni);
        }
    }
    // unique by addr
    std::unordered_map<std::string, NodeInfo> uniq;
    for (auto &n : all) uniq[n.addr] = n;
    std::vector<NodeInfo> v;
    v.reserve(uniq.size());
    for (auto &p : uniq) v.push_back(p.second);
    std::sort(v.begin(), v.end(), [&](const NodeInfo &a, const NodeInfo &b){
        NodeID da = a.id ^ target, db = b.id ^ target; return da.less_than(db);
    });
    if (v.size() > count) v.resize(count);
    return v;
}
