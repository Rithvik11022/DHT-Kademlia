#include "storage.h"

void Storage::put(const std::string &key_hex, const std::string &value) {
    std::unique_lock lk(mu);
    data[key_hex] = value;
}

std::optional<std::string> Storage::get(const std::string &key_hex) const {
    std::shared_lock lk(mu);
    auto it = data.find(key_hex);
    if (it == data.end()) return std::nullopt;
    return it->second;
}
