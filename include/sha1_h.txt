#pragma once
#include <string>

namespace sha1 {
    // compute raw 20-byte SHA-1 digest
    std::string hash_raw(const std::string &input);

    // compute 40-char lowercase hex SHA-1 digest
    std::string hash_hex(const std::string &input);
}
