#include "node.h"
#include <iostream>
#include <thread>
#include <chrono>

// Simple CLI:
// ./kademlia --port 3000 [--bootstrap ip:port]
int main(int argc, char **argv) {
    uint16_t port = 3000;
    std::string bootstrap;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--port" && i+1 < argc) { port = (uint16_t)std::stoi(argv[++i]); }
        else if (a == "--bootstrap" && i+1 < argc) { bootstrap = argv[++i]; }
    }
    // create node id and addr
    NodeID id = NodeID::random();
    std::string addr = "127.0.0.1:" + std::to_string(port);
    Node node(id, addr, port);
    node.start();
    if (!bootstrap.empty()) {
        std::cout << "Bootstrapping to " << bootstrap << "...\n";
        node.bootstrap(bootstrap);
    }
    std::cout << "Node started at " << addr << " id=" << id.to_hex().substr(0,8) << "...\n";
    std::cout << "Commands:\n  STORE key value\n  FINDVAL key\n  FINDNODE hexid\n  QUIT\n";
    std::string line;
    while (true) {
        std::cout << "> " << std::flush;
        if (!std::getline(std::cin, line)) break;
        if (line.empty()) continue;
        if (line == "QUIT") break;
        if (line.rfind("STORE ", 0) == 0) {
            // STORE key value
            auto rest = line.substr(6);
            auto sp = rest.find(' ');
            if (sp == std::string::npos) { std::cout << "usage: STORE key_hex value\n"; continue; }
            std::string key_hex = rest.substr(0, sp);
            std::string value = rest.substr(sp+1);
            NodeID keyid = NodeID::from_hex(key_hex);
            node.store_value(keyid, key_hex, value);
            std::cout << "STORE initiated\n";
        } else if (line.rfind("FINDVAL ", 0) == 0) {
            std::string key_hex = line.substr(8);
            NodeID keyid = NodeID::from_hex(key_hex);
            auto res = node.iterative_find_value(key_hex, keyid);
            if (std::holds_alternative<std::string>(res)) {
                std::cout << "VALUE: " << std::get<std::string>(res) << "\n";
            } else {
                auto v = std::get<std::vector<NodeInfo>>(res);
                std::cout << "Closest nodes (" << v.size() << "):\n";
                for (auto &n : v) std::cout << "  " << n.addr << " id=" << n.id.to_hex().substr(0,8) << "...\n";
            }
        } else if (line.rfind("FINDNODE ", 0) == 0) {
            std::string hexid = line.substr(9);
            NodeID idt = NodeID::from_hex(hexid);
            auto v = node.iterative_find_node(idt);
            std::cout << "Found nodes:\n";
            for (auto &n : v) std::cout << "  " << n.addr << " id=" << n.id.to_hex().substr(0,8) << "...\n";
        } else {
            std::cout << "Unknown command\n";
        }
    }

    node.stop();
    return 0;
}
