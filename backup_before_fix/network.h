#pragma once
#include <string>
#include <optional>
#include <functional>
#include <cstdint>

// Very small UDP wrapper that knows how to send and receive text messages
// Messages are '\n' terminated.

class UDPNetwork {
public:
    UDPNetwork(uint16_t port);
    ~UDPNetwork();

    // start receiving; handler will be invoked on incoming messages.
    void start_receive(std::function<void(const std::string& msg, const std::string& from_addr)> handler);

    // stop
    void stop_receive();

    // send a message (fire-and-forget)
    bool send_message(const std::string &to_addr, const std::string &msg);

    // synchronous send-request-receive: send msg and wait up to timeout_ms for a reply
    std::optional<std::string> send_request_wait_response(const std::string &to_addr, const std::string &msg, int timeout_ms);

    // helper: split "ip:port" into ip and port
    static bool split_addr(const std::string &addr, std::string &ip, uint16_t &port);
};
