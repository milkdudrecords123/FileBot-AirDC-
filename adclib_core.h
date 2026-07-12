#ifndef ADCLIB_CORE_H
#define ADCLIB_CORE_H

#include <string>

namespace ADCLibCore {
    std::string hash_pid(const std::string& pid);
    std::string hash_pas(const std::string& password, const std::string& salt);
    std::string hash_pas_oldschool(const std::string& password, const std::string& salt, const std::string& cid);
    std::string base32_encode(const unsigned char* data, size_t len);
    std::string escape(const std::string& s);
    std::string unescape(const std::string& s);
    bool validate_utf8(const std::string& str);
    std::string sanitize_utf8(const std::string& str);
}
#endif