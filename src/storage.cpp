#include "storage.h"
#include "sha1.h"
#include <mutex>

std::string Storage::hash_key(const std::string &key_hex) const {
    return sha1::hash_hex(key_hex);
}

void Storage::put(const std::string &key_hex, const std::string &value) {
    std::unique_lock<std::shared_mutex> lk(mu);
    std::string hashed_key = hash_key(key_hex);
    data[hashed_key] = value;
}

std::optional<std::string> Storage::get(const std::string &key_hex) const {
    std::shared_lock<std::shared_mutex> lk(mu);
    std::string hashed_key = hash_key(key_hex);
    auto it = data.find(hashed_key);
    if (it == data.end()) return std::nullopt;
    return it->second;
}
