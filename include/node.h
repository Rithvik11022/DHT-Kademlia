#pragma once
#include <array>
#include <string>
#include <vector>
#include <optional>
#include <cstdint>   // uint8_t, uint16_t
#include <variant>   // std::variant
#include <memory>

using byte = uint8_t;
static constexpr size_t ID_BYTES = 20; // 160-bit

struct NodeID {
    std::array<byte, ID_BYTES> b;
    static NodeID random();
    static NodeID from_hex(const std::string &hex);
    std::string to_hex() const;
    NodeID operator^(const NodeID &o) const;
    bool operator==(const NodeID &o) const;
    bool operator!=(const NodeID &o) const;
    bool less_than(const NodeID &o) const; // lexicographic
    int prefix_len_to(const NodeID &o) const; // -1 if same
};

struct NodeInfo {
    NodeID id;
    std::string addr; // "ip:port"
    NodeInfo() = default;
    NodeInfo(const NodeID &i, std::string a): id(i), addr(std::move(a)) {}
};

struct FindNodeResult {
    std::vector<NodeInfo> nodes;
};
struct FindValueResult {
    std::optional<std::string> value;
    std::vector<NodeInfo> nodes;
};

class RoutingTable;
class Storage;
class UDPNetwork;

class Node {
public:
    Node(const NodeID &id, const std::string &addr, uint16_t port);
    ~Node();

    void start(); // starts recv thread
    void stop();

    // RPC handlers (called by network)
    bool handle_ping(const NodeInfo &from);
    FindNodeResult handle_find_node(const NodeInfo &from, const NodeID &target);
    FindValueResult handle_find_value(const NodeInfo &from, const std::string &key_hex, const NodeID &key_id);
    void handle_store(const NodeInfo &from, const std::string &key_hex, const std::string &value);

    // High-level client RPCs (call remote node)
    bool rpc_ping(const std::string &peer_addr, int timeout_ms = 500);
    std::optional<FindNodeResult> rpc_find_node(const std::string &peer_addr, const NodeID &target, int timeout_ms = 800);
    std::optional<FindValueResult> rpc_find_value(const std::string &peer_addr, const std::string &key_hex, const NodeID &key_id, int timeout_ms = 800);
    bool rpc_store(const std::string &peer_addr, const std::string &key_hex, const std::string &value, int timeout_ms = 800);

    // Higher-level algorithms
    std::vector<NodeInfo> iterative_find_node(const NodeID &target);
    std::variant<std::string, std::vector<NodeInfo>> iterative_find_value(const std::string &key_hex, const NodeID &key_id);
    bool store_value(const NodeID &key_id, const std::string &key_hex, const std::string &value);

    // bootstrap: contact known peer
    void bootstrap(const std::string &bootstrap_addr);

    // accessors
    NodeID id() const { return _id; }
    std::string addr() const { return _addr; }
private:
    NodeID _id;
    std::string _addr; // ip:port form
    uint16_t _port;

    RoutingTable* _rt = nullptr;
    Storage* _store = nullptr;
    UDPNetwork* _net = nullptr;

    void process_incoming_message(const std::string &msg, const std::string &from_addr);
};