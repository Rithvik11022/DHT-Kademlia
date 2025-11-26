#include "node.h"
#include "network.h"
#include "routing_table.h"
#include "storage.h"
#include <random>
#include <sstream>
#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <algorithm>
#include <unordered_set>
#include <variant>

// NodeID implementation
NodeID NodeID::random() {
    NodeID id;
    static thread_local std::mt19937_64 rng((unsigned)std::chrono::high_resolution_clock::now().time_since_epoch().count());
    for (size_t i = 0; i < ID_BYTES; ++i) {
        // use rng() to get random 64-bit then take low byte
        id.b[i] = static_cast<byte>(rng() & 0xFFULL);
    }
    return id;
}

NodeID NodeID::from_hex(const std::string &hex) {
    NodeID id{};
    size_t pos = 0;
    for (size_t i = 0; i < ID_BYTES && pos + 1 < hex.size(); ++i) {
        auto hi = std::stoi(hex.substr(pos,1), nullptr, 16);
        auto lo = std::stoi(hex.substr(pos+1,1), nullptr, 16);
        id.b[i] = static_cast<byte>((hi<<4) | lo);
        pos += 2;
    }
    return id;
}

std::string NodeID::to_hex() const {
    static const char *h = "0123456789abcdef";
    std::string s; s.reserve(ID_BYTES*2);
    for (size_t i = 0; i < ID_BYTES; ++i) {
        unsigned c = (unsigned)b[i];
        s.push_back(h[c >> 4]);
        s.push_back(h[c & 0xF]);
    }
    return s;
}

NodeID NodeID::operator^(const NodeID &o) const {
    NodeID r;
    for (size_t i = 0; i < ID_BYTES; ++i) r.b[i] = static_cast<byte>(b[i] ^ o.b[i]);
    return r;
}
bool NodeID::operator==(const NodeID &o) const { return b == o.b; }
bool NodeID::operator!=(const NodeID &o) const { return !(*this == o); }
bool NodeID::less_than(const NodeID &o) const {
    for (size_t i = 0; i < ID_BYTES; ++i) {
        if (b[i] < o.b[i]) return true;
        if (b[i] > o.b[i]) return false;
    }
    return false;
}
int NodeID::prefix_len_to(const NodeID &o) const {
    NodeID x = (*this) ^ o;
    for (size_t i = 0; i < ID_BYTES; ++i) {
        byte v = x.b[i];
        if (v != 0) {
            for (int bit = 7; bit >= 0; --bit) {
                if (v & (1u << bit)) {
                    return (int)(i*8 + (7 - bit));
                }
            }
        }
    }
    return -1;
}

// Node implementation
static const size_t K_BUCKET_SIZE = 20;
static const size_t ALPHA = 3;

Node::Node(const NodeID &id, const std::string &addr, uint16_t port)
    : _id(id), _addr(addr), _port(port) {
    _rt = new RoutingTable(_id);
    _store = new Storage();
    _net = new UDPNetwork(port);
}

Node::~Node() {
    stop();
    delete _rt;
    delete _store;
    delete _net;
}

void Node::start() {
    _net->start_receive([this](const std::string &msg, const std::string &from_addr){
        this->process_incoming_message(msg, from_addr);
    });
}

void Node::stop() {
    _net->stop_receive();
}

// on-wire protocol: messages are newline-terminated ASCII "CMD|args..."
// handlers:

bool Node::handle_ping(const NodeInfo &from) {
    _rt->update_contact(from);
    return true;
}

FindNodeResult Node::handle_find_node(const NodeInfo &from, const NodeID &target) {
    _rt->update_contact(from);
    auto v = _rt->find_closest(target, K_BUCKET_SIZE);
    return {v};
}

FindValueResult Node::handle_find_value(const NodeInfo &from, const std::string &key_hex, const NodeID &key_id) {
    _rt->update_contact(from);
    auto val = _store->get(key_hex);
    if (val) return {val, {}};
    auto v = _rt->find_closest(key_id, K_BUCKET_SIZE);
    return {std::nullopt, v};
}

void Node::handle_store(const NodeInfo &from, const std::string &key_hex, const std::string &value) {
    _rt->update_contact(from);
    _store->put(key_hex, value);
}

// helpers for parsing/sending
static std::vector<std::string> split_pipe(const std::string &s) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t p = s.find('|', pos);
        if (p == std::string::npos) p = s.size();
        out.push_back(s.substr(pos, p-pos));
        pos = p+1;
    }
    return out;
}

static std::string join_pipe(const std::vector<std::string> &parts) {
    std::string r;
    for (size_t i = 0; i < parts.size(); ++i) {
        r += parts[i];
        if (i + 1 < parts.size()) r.push_back('|');
    }
    return r;
}

void Node::process_incoming_message(const std::string &msg, const std::string &from_addr) {
    // remove trailing newline
    std::string m = msg;
    if (!m.empty() && m.back() == '\n') m.pop_back();
    auto parts = split_pipe(m);
    if (parts.empty()) return;
    std::string cmd = parts[0];

    // expected addresses are "ip:port"
    if (cmd == "PING") {
        if (parts.size() < 3) return;
        NodeID from_id = NodeID::from_hex(parts[1]);
        std::string from_address = parts[2];
        bool ok = handle_ping(NodeInfo{from_id, from_address});
        std::string reply = std::string("PONG|") + (ok ? "1" : "0") +std::string("|") + _id.to_hex() + "\n";
        _net->send_message(from_address, reply);
    } else if (cmd == "FIND_NODE") {
        if (parts.size() < 4) return;
        NodeID from_id = NodeID::from_hex(parts[1]);
        std::string from_address = parts[2];
        NodeID target = NodeID::from_hex(parts[3]);
        auto res = handle_find_node(NodeInfo{from_id, from_address}, target);
        std::string body;
        for (auto &ni : res.nodes) {
            if (!body.empty()) body.push_back(';');
            body += ni.addr + "," + ni.id.to_hex();
        }
        std::string reply = std::string("FIND_NODE_REPLY|") + body + "\n";
        // std::cout<<reply<<std::endl;
        _net->send_message(from_address, reply);
    } else if (cmd == "FIND_VALUE") {
        if (parts.size() < 4) return;
        NodeID from_id = NodeID::from_hex(parts[1]);
        std::string from_address = parts[2];
        std::string key_hex = parts[3];
        NodeID key_id = NodeID::from_hex(key_hex);
        auto res = handle_find_value(NodeInfo{from_id, from_address}, key_hex, key_id);
        if (res.value.has_value()) {
            std::string reply = std::string("FIND_VALUE_REPLY|VALUE|") + *res.value + "\n";
            _net->send_message(from_address, reply);
        } else {
            std::string body;
            for (auto &ni : res.nodes) {
                if (!body.empty()) body.push_back(';');
                body += ni.addr + "," + ni.id.to_hex();
            }
            std::string reply = std::string("FIND_VALUE_REPLY|NODES|") + body + "\n";
            _net->send_message(from_address, reply);
        }
    } else if (cmd == "STORE") {
        if (parts.size() < 5) return;
        NodeID from_id = NodeID::from_hex(parts[1]);
        std::string from_address = parts[2];
        std::string key_hex = parts[3];
        std::string value = parts[4];
        handle_store(NodeInfo{from_id, from_address}, key_hex, value);
        std::string reply = "STORE_REPLY|OK\n";
        _net->send_message(from_address, reply);
    } else {
        // std::cout<<"hello"<<std::endl;
        // std::cout<<m<<std::endl;
    }
}

// Client RPCs
bool Node::rpc_ping(const std::string &peer_addr, int timeout_ms) {
    std::string msg = "PING|" + _id.to_hex() + "|" + _addr + "\n";
    auto res = _net->send_request_wait_response(peer_addr, msg, timeout_ms);
    if (!res) return false;
    std::string r = *res;
    if (r.find("PONG|1") != std::string::npos) {
        std::cout << peer_addr << ' ' << "eher\n" << r <<"\n" ; 
        std::string id_node = r.substr(std::string("PONG|1|").size());
        id_node.erase(id_node.size()-1);
        // std::cout << peer_addr << ' ' << "eher\n" << r <<"\n" ; 
        _rt->update_contact(NodeInfo{ NodeID::from_hex(id_node), peer_addr});
        return true;
    }
    return false;
}

std::optional<FindNodeResult> Node::rpc_find_node(const std::string &peer_addr, const NodeID &target, int timeout_ms) {
    std::string msg = "FIND_NODE|" + _id.to_hex() + "|" + _addr + "|" + target.to_hex() + "\n";
    // std::cout<<_id.to_hex()<<std::endl;
    // std::cout<<target.to_hex()<<std::endl;
    // std::cout<<msg<<" hello "<<std::endl;
    auto res = _net->send_request_wait_response(peer_addr, msg, timeout_ms);
    if (!res) return std::nullopt;
    std::string r = *res;
    if (r.rfind("FIND_NODE_REPLY|", 0) != 0) return std::nullopt;
    std::string payload = r.substr(std::string("FIND_NODE_REPLY|").size());
    // if (!payload.empty() && payload.back() == '\n') payload.pop_back();
    std::vector<NodeInfo> out;
    if (!payload.empty()) {
        size_t pos = 0;
        while (pos < payload.size()) {
            size_t semi = payload.find(';', pos);
            if (semi == std::string::npos) semi = payload.size();
            std::string entry = payload.substr(pos, semi - pos);
            size_t comma = entry.find(',');
            if (comma != std::string::npos) {
                std::string adr = entry.substr(0, comma);
                std::string idh = entry.substr(comma+1);
                out.emplace_back(NodeID::from_hex(idh), adr);
                // std::cout<<idh <<" c "<<adr<<std::endl;
            }
            pos = semi + 1;
        }
    }

    return FindNodeResult{out};
}

std::optional<FindValueResult> Node::rpc_find_value(const std::string &peer_addr, const std::string &key_hex, const NodeID &key_id, int timeout_ms) {
    std::string msg = "FIND_VALUE|" + _id.to_hex() + "|" + _addr + "|" + key_hex + "\n";
    auto res = _net->send_request_wait_response(peer_addr, msg, timeout_ms);
    if (!res) return std::nullopt;
    std::string r = *res;
    if (r.rfind("FIND_VALUE_REPLY|", 0) != 0) return std::nullopt;
    std::string tail = r.substr(std::string("FIND_VALUE_REPLY|").size());
    if (tail.rfind("VALUE|", 0) == 0) {
        std::string val = tail.substr(std::string("VALUE|").size());
        if (!val.empty() && val.back() == '\n') val.pop_back();
        return FindValueResult{val, {}};
    } else if (tail.rfind("NODES|", 0) == 0) {
        std::string payload = tail.substr(std::string("NODES|").size());
        if (!payload.empty() && payload.back() == '\n') payload.pop_back();
        std::vector<NodeInfo> out;
        size_t pos = 0;
        while (pos < payload.size()) {
            size_t semi = payload.find(';', pos);
            if (semi == std::string::npos) semi = payload.size();
            std::string entry = payload.substr(pos, semi - pos);
            size_t comma = entry.find(',');
            if (comma != std::string::npos) {
                std::string adr = entry.substr(0, comma);
                std::string idh = entry.substr(comma+1);
                out.emplace_back(NodeID::from_hex(idh), adr);
            }
            pos = semi + 1;
        }
        return FindValueResult{std::nullopt, out};
    }
    return std::nullopt;
}

bool Node::rpc_store(const std::string &peer_addr, const std::string &key_hex, const std::string &value, int timeout_ms) {
    std::string msg = "STORE|" + _id.to_hex() + "|" + _addr + "|" + key_hex + "|" + value + "\n";
    auto res = _net->send_request_wait_response(peer_addr, msg, timeout_ms);
    if (!res) return false;
    std::string r = *res;
    if (r.find("STORE_REPLY|OK") != std::string::npos) return true;
    return false;
}

// iterative find_node (basic)
std::vector<NodeInfo> Node::iterative_find_node(const NodeID &target) {
    auto candidates = _rt->find_closest(target, K_BUCKET_SIZE);
    std::unordered_set<std::string> queried;
    bool progress = true;
    while (progress) {
        progress = false;
        size_t q = 0;
        for (size_t i = 0; i < candidates.size() && q < ALPHA; ++i) {
            if (queried.count(candidates[i].addr)) continue;
            queried.insert(candidates[i].addr);
            q++;
            auto res = rpc_find_node(candidates[i].addr, target);
            if (res) {
                for (auto &n : res->nodes) {
                    bool exists = false;
                    for (auto &c : candidates) if (c.addr == n.addr) { exists = true; break; }
                    if (!exists) { candidates.push_back(n); progress = true; }
                }
            }
        }
        std::sort(candidates.begin(), candidates.end(), [&](const NodeInfo &a, const NodeInfo &b){
            NodeID da = a.id ^ target, db = b.id ^ target; return da.less_than(db);
        });
        if (candidates.size() > K_BUCKET_SIZE) candidates.resize(K_BUCKET_SIZE);
    }
    return candidates;
}

std::variant<std::string, std::vector<NodeInfo>> Node::iterative_find_value(const std::string &key_hex, const NodeID &key_id) {
    auto candidates = _rt->find_closest(key_id, K_BUCKET_SIZE);
    if(candidates.size()==0){
         auto val = _store->get(key_hex);
        if (val.has_value()) {
            return *val;
        }
        return std::vector<NodeInfo>();
    }
    std::unordered_set<std::string> queried;
    bool progress = true;
    while (progress) {
        progress = false;
        size_t q = 0;
        for (size_t i = 0; i < candidates.size() && q < ALPHA; ++i) {
            if (queried.count(candidates[i].addr)) continue;
            queried.insert(candidates[i].addr);
            q++;
            auto res = rpc_find_value(candidates[i].addr, key_hex, key_id);
            if (!res) continue;
            if (res->value.has_value()){
                return *res->value;
            } 
            for (auto &n : res->nodes) {
                bool exists = false;
                for (auto &c : candidates) if (c.addr == n.addr) { exists = true; break; }
                if (!exists) { candidates.push_back(n); progress = true; }
            }
        }
        std::sort(candidates.begin(), candidates.end(), [&](const NodeInfo &a, const NodeInfo &b){
            NodeID da = a.id ^ key_id, db = b.id ^ key_id; return da.less_than(db);
        });
        if (candidates.size() > K_BUCKET_SIZE) candidates.resize(K_BUCKET_SIZE);
    }
    return candidates;
}

std::variant<std::string, std::vector<NodeInfo>> Node::iterative_find_value_trace(const std::string &key_hex, const NodeID &key_id) {
    auto candidates = _rt->find_closest(key_id, K_BUCKET_SIZE);
    if(candidates.size()==0){
         auto val = _store->get(key_hex);
        if (val.has_value()) {
            return *val;
        }
        return std::vector<NodeInfo>();
    }
    std::vector<std::vector<NodeInfo>> global_trace;
    std::unordered_set<std::string> queried;
    bool progress = true;
    while (progress) {
        std::vector<NodeInfo> trace;
        progress = false;
        size_t q = 0;
        for (size_t i = 0; i < candidates.size() && q < ALPHA; ++i) {
            if (queried.count(candidates[i].addr)) continue;
            queried.insert(candidates[i].addr);
            q++;
            auto res = rpc_find_value(candidates[i].addr, key_hex, key_id);
            trace.push_back(candidates[i]);
            if (!res) continue;
            if (res->value.has_value()){
                for(auto path : global_trace)
                {
                    std::cout<<"+-- ";
                    int i;
                    for(i=0;i<(3<path.size()?3:path.size())-1;i++)
                    {
                        std::cout<<path[i].addr<<" || ";
                    }
                    std::cout<<path[i].addr<<"--+\n|\n";
                }
                return *res->value;
            } 
            for (auto &n : res->nodes) {
                bool exists = false;
                for (auto &c : candidates) if (c.addr == n.addr) { exists = true; break; }
                if (!exists) { candidates.push_back(n); progress = true; }
            }
        }
        std::sort(candidates.begin(), candidates.end(), [&](const NodeInfo &a, const NodeInfo &b){
            NodeID da = a.id ^ key_id, db = b.id ^ key_id; return da.less_than(db);
        });
        if (candidates.size() > K_BUCKET_SIZE) candidates.resize(K_BUCKET_SIZE);
    }
    return candidates;
}

bool Node::store_value(const NodeID &key_id, const std::string &key_hex, const std::string &value) {
    // store locally first (so the node that initiated the STORE can answer FINDVAL immediately)

    // find k-closest peers and send STORE RPCs (replicate)
    auto closest = iterative_find_node(key_id);
    for (auto &peer : closest) {
        rpc_store(peer.addr, key_hex, value);
    }
    if(closest.size()==0){
        _store->put(key_hex,value);
    }
    return true;
}


void Node::bootstrap(const std::string &bootstrap_addr) {
    if (bootstrap_addr == _addr){
        std::cerr << "Bootstrap address is the same as the local node address\n";
        exit (1);
    };
    if (!rpc_ping(bootstrap_addr)) {
        std::cerr << " Bootstrap node " << bootstrap_addr << " is not reachable (PING failed).\n";
        exit (1);
    };
    auto res = rpc_find_node(bootstrap_addr, _id);
    if (!res) return;
    for (auto &ni : res->nodes) _rt->update_contact(ni);
}