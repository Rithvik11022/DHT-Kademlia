#include "network.h"
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <thread>
#include <iostream>
#include <cstring>
#include <netdb.h>
#include <sstream>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <atomic>

static int create_socket_bind(uint16_t port) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { perror("socket"); return -1; }
    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if (bind(sock, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); close(sock); return -1; }
    return sock;
}

struct UDPNetwork::Impl {
    int sock = -1;
    std::thread recv_thread;
    std::atomic<bool> running{false};
    std::function<void(const std::string&, const std::string&)> handler;
    std::mutex reply_mutex;
    std::condition_variable reply_cv;
    std::string last_reply;
    std::string last_reply_from;
};

static std::string addr_to_string(const sockaddr_in &sa) {
    char buf[64];
    inet_ntop(AF_INET, &sa.sin_addr, buf, sizeof(buf));
    uint16_t p = ntohs(sa.sin_port);
    std::ostringstream ss;
    ss << buf << ":" << p;
    return ss.str();
}

UDPNetwork::UDPNetwork(uint16_t port) {
    impl = new Impl();
    impl->sock = create_socket_bind(port);
    if (impl->sock < 0) throw std::runtime_error("failed to create/bind UDP socket");
    impl->running = false;
}

UDPNetwork::~UDPNetwork() {
    stop_receive();
    if (impl) {
        if (impl->sock >= 0) close(impl->sock);
        delete impl;
        impl = nullptr;
    }
}

void UDPNetwork::start_receive(std::function<void(const std::string&, const std::string&)> handler) {
    impl->handler = handler;
    impl->running = true;
    impl->recv_thread = std::thread([this](){
        Impl *i = this->impl;
        char buf[4096];
        while (i->running) {
            sockaddr_in from{};
            socklen_t fromlen = sizeof(from);
            ssize_t n = recvfrom(i->sock, buf, sizeof(buf)-1, 0, (sockaddr*)&from, &fromlen);
            if (n <= 0) {
                if (i->running) std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            buf[n] = '\0';
            std::string msg(buf);
            std::string from_addr = addr_to_string(from);
            {
                std::unique_lock<std::mutex> lk(i->reply_mutex);
                i->last_reply = msg;
                i->last_reply_from = from_addr;
                i->reply_cv.notify_all();
            }
            if (i->handler) i->handler(msg, from_addr);
        }
    });
}

void UDPNetwork::stop_receive() {
    if (!impl) return;
    if (impl->running) {
        impl->running = false;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t len = sizeof(addr);
        getsockname(impl->sock, (sockaddr*)&addr, &len);
        sendto(impl->sock, "\n", 1, 0, (sockaddr*)&addr, len);
        if (impl->recv_thread.joinable()) impl->recv_thread.join();
    }
}

bool UDPNetwork::send_message(const std::string &to_addr, const std::string &msg) {
    std::string ip; uint16_t port;
    if (!split_addr(to_addr, ip, port)) return false;
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return false;
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    inet_pton(AF_INET, ip.c_str(), &dst.sin_addr);
    dst.sin_port = htons(port);
    ssize_t sent = sendto(sock, msg.c_str(), (int)msg.size(), 0, (sockaddr*)&dst, sizeof(dst));
    close(sock);
    return sent == (ssize_t)msg.size();
}

std::optional<std::string> UDPNetwork::send_request_wait_response(const std::string &to_addr, const std::string &msg, int timeout_ms) {
    std::string ip; uint16_t port;
    if (!split_addr(to_addr, ip, port)) return std::nullopt;
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    inet_pton(AF_INET, ip.c_str(), &dst.sin_addr);
    dst.sin_port = htons(port);
    ssize_t sent = sendto(impl->sock, msg.c_str(), (int)msg.size(), 0, (sockaddr*)&dst, sizeof(dst));
    if (sent <= 0) return std::nullopt;
    std::unique_lock<std::mutex> lk(impl->reply_mutex);
    if (impl->reply_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms)) == std::cv_status::timeout) {
        return std::nullopt;
    }
    return impl->last_reply;
}

bool UDPNetwork::split_addr(const std::string &addr, std::string &ip, uint16_t &port) {
    auto pos = addr.find(':');
    if (pos == std::string::npos) return false;
    ip = addr.substr(0, pos);
    port = (uint16_t)std::stoi(addr.substr(pos+1));
    return true;
}
