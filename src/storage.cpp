#include "storage.h"
#include <mutex>

void Storage::put(const std::string &key_hex, const std::string &value) {
    std::unique_lock<std::shared_mutex> lk(mu);
    data[key_hex] = value;
}

std::optional<std::string> Storage::get(const std::string &key_hex) const {
    std::shared_lock<std::shared_mutex> lk(mu);
    auto it = data.find(key_hex);
    if (it == data.end()) return std::nullopt;
    return it->second;
}
