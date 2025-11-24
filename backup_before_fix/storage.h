#pragma once
#include <string>
#include <unordered_map>
#include <shared_mutex>
#include <optional>

class Storage {
public:
    void put(const std::string &key_hex, const std::string &value);
    std::optional<std::string> get(const std::string &key_hex) const;
private:
    std::unordered_map<std::string, std::string> data;
    mutable std::shared_mutex mu;
};
