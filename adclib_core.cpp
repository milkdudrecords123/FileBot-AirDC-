#include "adclib_core.h"
#include "base32.h"
#include "tiger.h"
#include <cstring>
#include <cstdlib>

namespace ADCLibCore {
    std::string base32_encode(const unsigned char* data, size_t len) {
        return ADCLIB::BASE32::TOBASE32(data, len);
    }

    std::string hash_pid(const std::string& pid) {
        const int SIZE = 192/8;
        unsigned char cid[SIZE];
        memset(cid, 0, sizeof(cid));
        ADCLIB::BASE32::FROMBASE32(pid.c_str(), cid, sizeof(cid));
        ADCLIB::TigerHash Tiger;
        Tiger.update(cid, SIZE);
        Tiger.finalize();
        return ADCLIB::BASE32::TOBASE32(Tiger.getResult(), ADCLIB::TigerHash::HASH_SIZE);
    }

    std::string hash_pas(const std::string& password, const std::string& salt) {
        size_t saltBytes = salt.size() * 5 / 8;
        unsigned char* chunk = new unsigned char[saltBytes];
        memset(chunk, 0, saltBytes);
        ADCLIB::BASE32::FROMBASE32(salt.c_str(), chunk, saltBytes);
        ADCLIB::TigerHash Tiger;
        Tiger.update(password.data(), password.length());
        Tiger.update(chunk, saltBytes);
        Tiger.finalize();
        std::string result = ADCLIB::BASE32::TOBASE32(Tiger.getResult(), ADCLIB::TigerHash::HASH_SIZE);
        delete[] chunk;
        return result;
    }

    std::string hash_pas_oldschool(const std::string& password, const std::string& salt, const std::string& cid) {
        const int SIZE = 192/8;
        size_t saltBytes = salt.size() * 5 / 8;
        unsigned char* chunk1 = new unsigned char[saltBytes];
        unsigned char chunk2[SIZE];
        memset(chunk1, 0, saltBytes);
        memset(chunk2, 0, sizeof(chunk2));
        ADCLIB::BASE32::FROMBASE32(salt.c_str(), chunk1, saltBytes);
        ADCLIB::BASE32::FROMBASE32(cid.c_str(), chunk2, sizeof(chunk2));
        ADCLIB::TigerHash Tiger;
        Tiger.update(chunk2, SIZE);
        Tiger.update(password.data(), password.length());
        Tiger.update(chunk1, saltBytes);
        Tiger.finalize();
        std::string result = ADCLIB::BASE32::TOBASE32(Tiger.getResult(), ADCLIB::TigerHash::HASH_SIZE);
        delete[] chunk1;
        return result;
    }

    // Minimal UTF-8 stubs
    std::string escape(const std::string& s) { return s; }
    std::string unescape(const std::string& s) { return s; }
    bool validate_utf8(const std::string&) { return true; }
    std::string sanitize_utf8(const std::string& s) { return s; }
}