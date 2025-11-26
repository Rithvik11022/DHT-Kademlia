#include "node.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <variant>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <net/if.h>    // for IFF_UP

// Returns the first non-loopback IPv4 address found on the host, or "127.0.0.1"
// if none found. Uses getifaddrs so it works reliably on Linux.
std::string detect_first_nonloopback_ipv4() {
    struct ifaddrs *ifaddr = nullptr;
    if (getifaddrs(&ifaddr) == -1) {
        return std::string("127.0.0.1");
    }

    std::string result = "127.0.0.1";
    for (struct ifaddrs *ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) continue;
        if (ifa->ifa_addr->sa_family == AF_INET) {
            // IPv4
            char buf[INET_ADDRSTRLEN]{0};
            void *addrptr = &((struct sockaddr_in *)ifa->ifa_addr)->sin_addr;
            inet_ntop(AF_INET, addrptr, buf, INET_ADDRSTRLEN);
            std::string ip(buf);
            // skip loopback addresses
            if (ip == "127.0.0.1" || ip.rfind("127.", 0) == 0) continue;
            // ensure interface is up
            unsigned int flags = ifa->ifa_flags;
            if ((flags & IFF_UP) == 0) continue;
            // found a candidate
            result = ip;
            break;
        }
    }
    freeifaddrs(ifaddr);
    return result;
}

bool split_addr(const std::string &addr, std::string &ip, uint16_t &port) {
    auto pos = addr.find(':');
    if (pos == std::string::npos) return false;
    ip = addr.substr(0, pos);
    port = (uint16_t)std::stoi(addr.substr(pos + 1));
    return true;
}


// Simple CLI:
// ./kademlia --port 3000 [--bootstrap ip:port]
int main(int argc, char **argv) {
    uint16_t port = 3000;
    std::string bootstrap;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--port" && i+1 < argc) {
            port = (uint16_t)std::stoi(argv[++i]);
        } else if (a == "--bootstrap" && i+1 < argc) {
            bootstrap = argv[++i];
        }
        else {
            std::cerr << "Unknown arg: " << a << "\n";
        }
    }

    // detect a suitable local IPv4 if advertise not provided
    std::string addr;
    std::string ip = detect_first_nonloopback_ipv4();
    addr = ip + ":" + std::to_string(port);

    NodeID id = NodeID::random();
    // NOTE: Node constructor in your current headers expects 3 args (id, addr, port).
    Node node(id, addr, port);
    node.start();
    if (!bootstrap.empty()) {
        std::string ip;
        uint16_t bp_port;
        if (split_addr(bootstrap, ip, bp_port)) {
            if (ip == "127.0.0.1") {
                std::string fixed_ip = detect_first_nonloopback_ipv4();
                bootstrap = fixed_ip + ":" + std::to_string(bp_port);
            }
        }
        std::cout << "Bootstrapping to " << bootstrap << "...\n";
        node.bootstrap(bootstrap);
    }

    std::cout << "Node started at " << addr << " id=" << id.to_hex()<< "\n";
    std::cout << "Commands:\n  STORE key value\n  FINDVAL_AT key\n  FINDVAL_TRACE key\n FINDNODE hexid\n  QUIT\n HELP\n";
    std::string line;
    while (true) {
        std::cout << "> " << std::flush;
        if (!std::getline(std::cin, line)) break;
        if (line.empty()) continue;
        if (line == "QUIT"||line == "quit") break;
        if (line == "HELP" || line == "help"){
            std::cout << "Node started at " << addr << " id=" << id.to_hex()<< "\n";
            std::cout << "Commands:\n  STORE key value\n  FINDVAL_AT key\n FINDVAL_TRACE key\n FINDNODE hexid\n  QUIT HELP\n";
            continue;
        }
        if (line.rfind("STORE ", 0) == 0 || line.rfind("store ", 0) == 0) {
            auto rest = line.substr(6);
            auto sp = rest.find(' ');
            if (sp == std::string::npos) { std::cout << "usage: STORE key_hex value\n"; continue; }
            std::string key_hex = rest.substr(0, sp);
            std::string value = rest.substr(sp+1);
            NodeID keyid = NodeID::from_hex(key_hex);
            node.store_value(keyid, key_hex, value);
            std::cout << "STORE initiated\n";
        } else if (line.rfind("FINDVAL_AT ", 0) == 0 || line.rfind("findval_at ", 0) == 0 ) {
            std::string key_hex = line.substr(11);
            NodeID keyid = NodeID::from_hex(key_hex);
            auto res = node.iterative_find_value(key_hex, keyid);
            if (std::holds_alternative<std::string>(res)) {
                std::cout << "VALUE: " << std::get<std::string>(res) << "\n";
            } else {
                auto v = std::get<std::vector<NodeInfo>>(res);
                std::cout << "Closest nodes (" << v.size() << "):\n";
                for (auto &n : v) std::cout << "  " << n.addr << " id=" << n.id.to_hex()<< "\n";
            }
        } else if (line.rfind("FINDVAL_TRACE ", 0) == 0 || line.rfind("findval_trace ", 0) == 0 ) {
            std::string key_hex = line.substr(14);
            NodeID keyid = NodeID::from_hex(key_hex);
            auto res = node.iterative_find_value_trace(key_hex, keyid);
            if (std::holds_alternative<std::string>(res)) {
                std::cout << "VALUE: " << std::get<std::string>(res) << "\n";
            } else {
                auto v = std::get<std::vector<NodeInfo>>(res);
                std::cout << "Closest nodes (" << v.size() << "):\n";
                for (auto &n : v) std::cout << "  " << n.addr << " id=" << n.id.to_hex() << "\n";
            }
        } else if (line.rfind("FINDNODE ", 0) == 0 || line.rfind("findnode ", 0) == 0) {
            std::string hexid = line.substr(9);
            NodeID idt = NodeID::from_hex(hexid);
            auto v = node.iterative_find_node(idt);
            std::cout << "Found nodes:\n";
            for (auto &n : v) std::cout << "  " << n.addr << " id=" << n.id.to_hex() << "\n";
        } else {
            std::cout << "Unknown command\n";
        }
    }

    node.stop();
    return 0;
}