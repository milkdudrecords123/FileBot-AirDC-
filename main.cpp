// main.cpp – ADC bot with RSS, release management, Hangman, and text‑file commands
#include <iostream>
#include <string>
#include <cstring>
#include <thread>
#include <chrono>
#include <map>
#include <memory>
#include <fstream>
#include <sstream>
#include <vector>
#include <mutex>
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <random>
#include <regex>
#include <iomanip>
#include <curl/curl.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509.h>
#include <openssl/sha.h>
#include <openssl/pem.h>
#include <nlohmann/json.hpp>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include "adclib_core.h"
#include "hangman.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "libcurl.dll.a")

using json = nlohmann::json;
namespace fs = std::filesystem;

// ------------------------------------------------------------------
// Configuration
// ------------------------------------------------------------------
struct BotConfig {
    std::string hub_url, bot_nick, bot_password, client_ip, messages_dir;
    std::string feeds_file, users_file, cert_file, key_file;
    int refresh_minutes;
    struct { int tcp, udp, tls_tcp; } ports;
    struct { std::string mode, external_ip; int tcp_port, udp_port, tls_tcp_port; } connection;
};
static BotConfig config;

// RSS data structures
struct FeedItem { std::string guid, title, link, pubDate, description; };
struct Feed { std::string url, tag; bool force; std::vector<FeedItem> items; time_t lastCheck; };
struct RssUser { std::string nick; bool enabled, pm; std::map<std::string, bool> muted; };

static std::vector<Feed> feeds;
static std::vector<RssUser> rssUsers;
static std::mutex rssMutex;
static int refreshMinutes = 10;
static bool rssRunning = true;
static SSL* hubSsl = nullptr;
static SOCKET plainSock = INVALID_SOCKET;
static bool useTls = true;
static std::string mySid;
static std::atomic<bool> keepRunning{true};
static std::map<std::string, std::string> nickToSid;
static std::mutex nickMutex;

// RSS enable/disable flag and mutex
static bool rssEnabled = true;
static std::mutex rssEnabledMutex;

// Release enable/disable flag and mutex
static bool releaseEnabled = true;
static std::mutex releaseEnabledMutex;

// Forward declarations from release.cpp
void initReleaseManagement();
void cmdAddRel(const std::string&, const std::string&, const std::string&);
void cmdShowRel(const std::string&, const std::string&);
void cmdDelRel(const std::string&, int);
void cmdSearchRel(const std::string&, const std::string&);
void cmdTopAdders(const std::string&);
void cmdRelHelp(const std::string&);
void cmdPruneRel(const std::string&, int);
void cmdReloadRel(const std::string&);
void cmdAnnounceRel(const std::string&, const std::string&, const std::string&, const std::string&);
void cmdRelOff(const std::string&);
void cmdRelOn(const std::string&);
void cmdAddCategory(const std::string&, const std::string&);
void cmdDelCategory(const std::string&, const std::string&);
void cmdListCategories(const std::string&);

// Forward declarations for RSS commands (implemented below)
void cmdRssList(const std::string&);
void cmdRssAdd(const std::string&, const std::string&, const std::string&);
void cmdRssRemove(const std::string&, const std::string&);
void cmdRssForce(const std::string&, const std::string&);
void cmdRssRefresh(const std::string&, int);
void cmdRssSubscribe(const std::string&, const std::string&);
void cmdRssUnsubscribe(const std::string&, const std::string&);
void cmdRssMute(const std::string&, const std::string&);
void cmdRssUnmute(const std::string&, const std::string&);
void cmdRssHelp(const std::string&);

// RSS enable/disable functions
void setRssEnabled(bool e) {
    std::lock_guard<std::mutex> lock(rssEnabledMutex);
    rssEnabled = e;
}
bool isRssEnabled() {
    std::lock_guard<std::mutex> lock(rssEnabledMutex);
    return rssEnabled;
}

// Release enable/disable functions
void setReleaseEnabled(bool e) {
    std::lock_guard<std::mutex> lock(releaseEnabledMutex);
    releaseEnabled = e;
}
bool isReleaseEnabled() {
    std::lock_guard<std::mutex> lock(releaseEnabledMutex);
    return releaseEnabled;
}

// ------------------------------------------------------------------
// Helper functions
// ------------------------------------------------------------------
std::string escapeAdc(const std::string& input) {
    std::string out; out.reserve(input.size()*2);
    for (char c : input) {
        switch (c) {
            case ' ': out += "\\s"; break;
            case '\n': out += "\\n"; break;
            case '\\': out += "\\\\"; break;
            default: out += c;
        }
    }
    return out;
}

void sendLine(const std::string& cmd) {
    std::string line = cmd + "\n";
    if (useTls && hubSsl) SSL_write(hubSsl, line.c_str(), (int)line.size());
    else if (!useTls && plainSock != INVALID_SOCKET) send(plainSock, line.c_str(), (int)line.size(), 0);
}

std::string recvLine() {
    char buf[4096]; int pos=0;
    while (pos < 4095) {
        int n = useTls ? SSL_read(hubSsl, buf+pos, 1) : recv(plainSock, buf+pos, 1, 0);
        if (n <= 0) return "";
        if (buf[pos] == '\n') break;
        ++pos;
    }
    buf[pos]='\0';
    std::string line(buf);
    if (!line.empty() && line.back()=='\r') line.pop_back();
    return line;
}

std::string getParam(const std::string& line, const std::string& key) {
    size_t pos = line.find(key);
    if (pos == std::string::npos) return "";
    pos += key.size();
    size_t end = line.find(' ', pos);
    return (end == std::string::npos) ? line.substr(pos) : line.substr(pos, end - pos);
}

void sendPrivateMessage(const std::string& targetSid, const std::string& message) {
    std::string escaped = escapeAdc(message);
    sendLine("EMSG " + mySid + " " + targetSid + " " + escaped + " PM" + mySid);
}

void sendToNick(const std::string& nick, const std::string& message) {
    std::lock_guard<std::mutex> lock(nickMutex);
    auto it = nickToSid.find(nick);
    if (it != nickToSid.end()) sendPrivateMessage(it->second, message);
}

void sendToAll(const std::string& from, const std::string& message) {
    std::string escaped = escapeAdc(message);
    sendLine("BMSG " + mySid + " " + escaped);
}

void sendToOps(const std::string& from, const std::string& message) { sendToAll(from, message); }

std::string getNickBySid(const std::string& sid) {
    std::lock_guard<std::mutex> lock(nickMutex);
    for (const auto& [nick, s] : nickToSid) if (s == sid) return nick;
    return "";
}

// ------------------------------------------------------------------
// Persistent PID
// ------------------------------------------------------------------
std::string loadOrGeneratePid() {
    const std::string pidFile = "bot_pid.txt";
    if (fs::exists(pidFile)) { std::ifstream f(pidFile); std::string pid; std::getline(f, pid); if (pid.size()==39) return pid; }
    const char* base32_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    std::random_device rd; std::mt19937 gen(rd()); std::uniform_int_distribution<> dis(0,31);
    std::string pid; pid.reserve(39);
    for (int i=0; i<39; ++i) pid += base32_chars[dis(gen)];
    std::ofstream f(pidFile); f << pid; return pid;
}

// ------------------------------------------------------------------
// Keep‑alive thread
// ------------------------------------------------------------------
void keepAlive() {
    while (keepRunning) { std::this_thread::sleep_for(std::chrono::seconds(60)); sendLine(""); }
}

// ------------------------------------------------------------------
// Hub URL parser
// ------------------------------------------------------------------
struct HubInfo { std::string host, keyprint; int port; bool isSecure; };
HubInfo parseHubUrl(const std::string& url) {
    HubInfo info; info.isSecure=false; info.port=411;
    size_t scheme_end = url.find("://");
    if (scheme_end == std::string::npos) throw std::runtime_error("Missing scheme");
    std::string scheme = url.substr(0, scheme_end);
    if (scheme == "adcs") info.isSecure = true;
    else if (scheme != "adc") throw std::runtime_error("Unsupported scheme");
    std::regex reSecure(R"(adcs://([^:/]+)(?::(\d+))?/\?kp=(?:SHA256/)?([A-Z2-7]+))");
    std::regex rePlain(R"(adc://([^:/]+)(?::(\d+))?(?:/|$))");
    std::smatch match;
    if (info.isSecure) {
        if (std::regex_match(url, match, reSecure)) {
            info.host = match[1];
            if (match[2].matched) info.port = std::stoi(match[2]);
            info.keyprint = match[3];
        } else throw std::runtime_error("Invalid adcs:// URL");
    } else {
        if (std::regex_match(url, match, rePlain)) {
            info.host = match[1];
            if (match[2].matched) info.port = std::stoi(match[2]);
        } else throw std::runtime_error("Invalid adc:// URL");
    }
    if (info.host.empty()) throw std::runtime_error("Missing host");
    return info;
}

// ------------------------------------------------------------------
// Load configuration
// ------------------------------------------------------------------
void loadConfig() {
    std::ifstream f("config.json");
    if (!f.is_open()) { std::cerr << "ERROR: config.json not found.\n"; exit(1); }
    json j; f >> j;
    config.hub_url = j.value("hub_url", "");
    config.bot_nick = j.value("bot_nick", "");
    config.bot_password = j.value("bot_password", "");
    config.client_ip = j.value("client_ip", "");
    config.messages_dir = j.value("messages_dir", "messages");
    config.feeds_file = j.value("feeds_file", "feeds.json");
    config.users_file = j.value("users_file", "rss_users.json");
    config.cert_file = j.value("cert_file", "client.crt.txt");
    config.key_file = j.value("key_file", "client.key.txt");
    config.refresh_minutes = j.value("refresh_minutes", 10);
    refreshMinutes = config.refresh_minutes;
    if (j.contains("ports")) {
        config.ports.tcp = j["ports"].value("tcp", 23168);
        config.ports.udp = j["ports"].value("udp", 23168);
        config.ports.tls_tcp = j["ports"].value("tls_tcp", 23169);
    }
    if (j.contains("connection")) {
        auto& conn = j["connection"];
        config.connection.mode = conn.value("mode", "passive");
        config.connection.external_ip = conn.value("external_ip", "");
        config.connection.tcp_port = conn.value("tcp_port", 23168);
        config.connection.udp_port = conn.value("udp_port", 23168);
        config.connection.tls_tcp_port = conn.value("tls_tcp_port", 23169);
    }
    if (config.hub_url.empty() || config.bot_nick.empty() || config.bot_password.empty()) {
        std::cerr << "ERROR: Missing required fields in config.json.\n"; exit(1);
    }
}

// ------------------------------------------------------------------
// JSON escape (for RSS)
// ------------------------------------------------------------------
std::string jsonEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch(c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c;
        }
    }
    return out;
}

// ------------------------------------------------------------------
// RSS serialization / deserialization
// ------------------------------------------------------------------
std::string serializeFeeds() {
    std::string out = "[\n";
    for (size_t i=0; i<feeds.size(); ++i) {
        const auto& f = feeds[i];
        out += "  {\n";
        out += "    \"url\": \"" + jsonEscape(f.url) + "\",\n";
        out += "    \"tag\": \"" + jsonEscape(f.tag) + "\",\n";
        out += "    \"force\": " + std::string(f.force ? "true" : "false") + ",\n";
        out += "    \"items\": [\n";
        for (size_t j=0; j<f.items.size(); ++j) {
            const auto& it = f.items[j];
            out += "      {\n";
            out += "        \"guid\": \"" + jsonEscape(it.guid) + "\",\n";
            out += "        \"title\": \"" + jsonEscape(it.title) + "\",\n";
            out += "        \"link\": \"" + jsonEscape(it.link) + "\",\n";
            out += "        \"pubDate\": \"" + jsonEscape(it.pubDate) + "\",\n";
            out += "        \"description\": \"" + jsonEscape(it.description) + "\"\n";
            out += "      }";
            if (j != f.items.size()-1) out += ",";
            out += "\n";
        }
        out += "    ]\n";
        out += "  }";
        if (i != feeds.size()-1) out += ",";
        out += "\n";
    }
    out += "]\n";
    return out;
}

void deserializeFeeds(const std::string& data) {
    feeds.clear();
    size_t pos=0;
    while (true) {
        size_t start = data.find("{\n", pos);
        if (start == std::string::npos) break;
        size_t end = data.find("}\n", start);
        if (end == std::string::npos) break;
        std::string obj = data.substr(start, end - start + 1);
        Feed f;
        size_t urlPos = obj.find("\"url\": \"");
        if (urlPos != std::string::npos) {
            urlPos += 8;
            size_t urlEnd = obj.find("\"", urlPos);
            if (urlEnd != std::string::npos) f.url = obj.substr(urlPos, urlEnd - urlPos);
        }
        size_t tagPos = obj.find("\"tag\": \"");
        if (tagPos != std::string::npos) {
            tagPos += 8;
            size_t tagEnd = obj.find("\"", tagPos);
            if (tagEnd != std::string::npos) f.tag = obj.substr(tagPos, tagEnd - tagPos);
        }
        size_t forcePos = obj.find("\"force\": ");
        if (forcePos != std::string::npos) {
            forcePos += 9;
            f.force = (obj.substr(forcePos, 4) == "true");
        }
        size_t itemsStart = obj.find("\"items\": [");
        if (itemsStart != std::string::npos) {
            size_t itemsEnd = obj.find("]", itemsStart);
            if (itemsEnd != std::string::npos) {
                std::string itemsStr = obj.substr(itemsStart, itemsEnd - itemsStart);
                size_t itemPos=0;
                while (true) {
                    size_t itemStart = itemsStr.find("{\n", itemPos);
                    if (itemStart == std::string::npos) break;
                    size_t itemEnd = itemsStr.find("}\n", itemStart);
                    if (itemEnd == std::string::npos) break;
                    std::string itemObj = itemsStr.substr(itemStart, itemEnd - itemStart + 1);
                    FeedItem it;
                    size_t guidPos = itemObj.find("\"guid\": \"");
                    if (guidPos != std::string::npos) {
                        guidPos += 9; size_t guidEnd = itemObj.find("\"", guidPos);
                        if (guidEnd != std::string::npos) it.guid = itemObj.substr(guidPos, guidEnd - guidPos);
                    }
                    size_t titlePos = itemObj.find("\"title\": \"");
                    if (titlePos != std::string::npos) {
                        titlePos += 10; size_t titleEnd = itemObj.find("\"", titlePos);
                        if (titleEnd != std::string::npos) it.title = itemObj.substr(titlePos, titleEnd - titlePos);
                    }
                    size_t linkPos = itemObj.find("\"link\": \"");
                    if (linkPos != std::string::npos) {
                        linkPos += 9; size_t linkEnd = itemObj.find("\"", linkPos);
                        if (linkEnd != std::string::npos) it.link = itemObj.substr(linkPos, linkEnd - linkPos);
                    }
                    size_t pubPos = itemObj.find("\"pubDate\": \"");
                    if (pubPos != std::string::npos) {
                        pubPos += 12; size_t pubEnd = itemObj.find("\"", pubPos);
                        if (pubEnd != std::string::npos) it.pubDate = itemObj.substr(pubPos, pubEnd - pubPos);
                    }
                    size_t descPos = itemObj.find("\"description\": \"");
                    if (descPos != std::string::npos) {
                        descPos += 15; size_t descEnd = itemObj.find("\"", descPos);
                        if (descEnd != std::string::npos) it.description = itemObj.substr(descPos, descEnd - descPos);
                    }
                    f.items.push_back(it);
                    itemPos = itemEnd + 1;
                }
            }
        }
        feeds.push_back(f);
        pos = end + 1;
    }
}

std::string serializeUsers() {
    std::string out = "[\n";
    for (size_t i=0; i<rssUsers.size(); ++i) {
        const auto& u = rssUsers[i];
        out += "  {\n";
        out += "    \"nick\": \"" + jsonEscape(u.nick) + "\",\n";
        out += "    \"enabled\": " + std::string(u.enabled ? "true" : "false") + ",\n";
        out += "    \"pm\": " + std::string(u.pm ? "true" : "false") + ",\n";
        out += "    \"muted\": {\n";
        size_t cnt=0;
        for (const auto& [tag, muted] : u.muted) {
            out += "      \"" + jsonEscape(tag) + "\": " + std::string(muted ? "true" : "false");
            if (++cnt != u.muted.size()) out += ",";
            out += "\n";
        }
        out += "    }\n";
        out += "  }";
        if (i != rssUsers.size()-1) out += ",";
        out += "\n";
    }
    out += "]\n";
    return out;
}

void deserializeUsers(const std::string& data) {
    rssUsers.clear();
    size_t pos=0;
    while (true) {
        size_t start = data.find("{\n", pos);
        if (start == std::string::npos) break;
        size_t end = data.find("}\n", start);
        if (end == std::string::npos) break;
        std::string obj = data.substr(start, end - start + 1);
        RssUser u;
        size_t nickPos = obj.find("\"nick\": \"");
        if (nickPos != std::string::npos) {
            nickPos += 9; size_t nickEnd = obj.find("\"", nickPos);
            if (nickEnd != std::string::npos) u.nick = obj.substr(nickPos, nickEnd - nickPos);
        }
        size_t enPos = obj.find("\"enabled\": ");
        if (enPos != std::string::npos) {
            enPos += 11; u.enabled = (obj.substr(enPos, 4) == "true");
        }
        size_t pmPos = obj.find("\"pm\": ");
        if (pmPos != std::string::npos) {
            pmPos += 7; u.pm = (obj.substr(pmPos, 4) == "true");
        }
        size_t mutedStart = obj.find("\"muted\": {");
        if (mutedStart != std::string::npos) {
            size_t mutedEnd = obj.find("}", mutedStart);
            if (mutedEnd != std::string::npos) {
                std::string mutedStr = obj.substr(mutedStart, mutedEnd - mutedStart);
                size_t kvPos=0;
                while (true) {
                    size_t keyStart = mutedStr.find("\"", kvPos);
                    if (keyStart == std::string::npos) break;
                    size_t keyEnd = mutedStr.find("\"", keyStart+1);
                    if (keyEnd == std::string::npos) break;
                    std::string tag = mutedStr.substr(keyStart+1, keyEnd - keyStart - 1);
                    size_t valStart = mutedStr.find(":", keyEnd);
                    if (valStart == std::string::npos) break;
                    valStart++;
                    while (valStart < mutedStr.size() && mutedStr[valStart] == ' ') valStart++;
                    bool muted = (mutedStr.substr(valStart, 4) == "true");
                    u.muted[tag] = muted;
                    kvPos = valStart + 4;
                }
            }
        }
        rssUsers.push_back(u);
        pos = end + 1;
    }
}

void loadFeeds() {
    std::ifstream f(config.feeds_file);
    if (!f.is_open()) return;
    std::stringstream buffer; buffer << f.rdbuf();
    std::string data = buffer.str();
    if (!data.empty()) deserializeFeeds(data);
}

void saveFeeds() {
    std::ofstream f(config.feeds_file);
    f << serializeFeeds();
}

void loadRssUsers() {
    std::ifstream f(config.users_file);
    if (!f.is_open()) return;
    std::stringstream buffer; buffer << f.rdbuf();
    std::string data = buffer.str();
    if (!data.empty()) deserializeUsers(data);
}

void saveRssUsers() {
    std::ofstream f(config.users_file);
    f << serializeUsers();
}

// ------------------------------------------------------------------
// HTTP GET
// ------------------------------------------------------------------
size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* output) {
    size_t total = size * nmemb;
    output->append((char*)contents, total);
    return total;
}

std::string httpGet(const std::string& url) {
    CURL* curl = curl_easy_init();
    if (!curl) return "";
    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    CURLcode res = curl_easy_perform(curl);
    long http_code=0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);
    if (res != CURLE_OK || http_code != 200) return "";
    return response;
}

// ------------------------------------------------------------------
// RSS feed parsing
// ------------------------------------------------------------------
std::vector<FeedItem> parseFeed(const std::string& xml, const std::string& feedUrl) {
    std::vector<FeedItem> items;
    bool isRss = (xml.find("<rss") != std::string::npos) || (xml.find("<rdf:RDF") != std::string::npos);
    bool isAtom = (xml.find("<feed") != std::string::npos) || (xml.find("<atom:feed") != std::string::npos);
    if (!isRss && !isAtom) return items;
    std::string itemOpen = isRss ? "<item>" : "<entry>";
    std::string itemClose = isRss ? "</item>" : "</entry>";
    size_t pos=0;
    while (true) {
        size_t start = xml.find(itemOpen, pos);
        if (start == std::string::npos) break;
        size_t end = xml.find(itemClose, start);
        if (end == std::string::npos) break;
        std::string itemXml = xml.substr(start + itemOpen.length(), end - start - itemOpen.length());
        FeedItem fi;
        size_t titleStart = itemXml.find("<title");
        if (titleStart != std::string::npos) {
            titleStart = itemXml.find(">", titleStart);
            if (titleStart != std::string::npos) {
                titleStart++;
                size_t titleEnd = itemXml.find("</title>", titleStart);
                if (titleEnd != std::string::npos) fi.title = itemXml.substr(titleStart, titleEnd - titleStart);
            }
        }
        size_t linkStart = itemXml.find("<link");
        if (linkStart != std::string::npos) {
            size_t href = itemXml.find("href=\"", linkStart);
            if (href != std::string::npos && href < linkStart + 100) {
                href += 6;
                size_t hrefEnd = itemXml.find("\"", href);
                if (hrefEnd != std::string::npos) fi.link = itemXml.substr(href, hrefEnd - href);
            } else {
                size_t linkEndTag = itemXml.find("</link>", linkStart);
                if (linkEndTag != std::string::npos) {
                    size_t linkValueStart = itemXml.find(">", linkStart);
                    if (linkValueStart != std::string::npos) {
                        linkValueStart++;
                        fi.link = itemXml.substr(linkValueStart, linkEndTag - linkValueStart);
                    }
                }
            }
        }
        size_t guidStart = itemXml.find("<guid");
        if (guidStart != std::string::npos) {
            size_t guidEnd = itemXml.find("</guid>", guidStart);
            if (guidEnd != std::string::npos) {
                size_t guidValueStart = itemXml.find(">", guidStart);
                if (guidValueStart != std::string::npos) {
                    guidValueStart++;
                    fi.guid = itemXml.substr(guidValueStart, guidEnd - guidValueStart);
                }
            }
        } else {
            size_t idStart = itemXml.find("<id");
            if (idStart != std::string::npos) {
                size_t idEnd = itemXml.find("</id>", idStart);
                if (idEnd != std::string::npos) {
                    size_t idValueStart = itemXml.find(">", idStart);
                    if (idValueStart != std::string::npos) {
                        idValueStart++;
                        fi.guid = itemXml.substr(idValueStart, idEnd - idValueStart);
                    }
                }
            }
        }
        size_t pubStart = itemXml.find("<pubDate");
        if (pubStart != std::string::npos) {
            size_t pubEnd = itemXml.find("</pubDate>", pubStart);
            if (pubEnd != std::string::npos) {
                size_t pubValueStart = itemXml.find(">", pubStart);
                if (pubValueStart != std::string::npos) {
                    pubValueStart++;
                    fi.pubDate = itemXml.substr(pubValueStart, pubEnd - pubValueStart);
                }
            }
        } else {
            size_t publishedStart = itemXml.find("<published");
            if (publishedStart != std::string::npos) {
                size_t publishedEnd = itemXml.find("</published>", publishedStart);
                if (publishedEnd != std::string::npos) {
                    size_t pubValueStart = itemXml.find(">", publishedStart);
                    if (pubValueStart != std::string::npos) {
                        pubValueStart++;
                        fi.pubDate = itemXml.substr(pubValueStart, publishedEnd - pubValueStart);
                    }
                }
            }
        }
        size_t descStart = itemXml.find("<description");
        if (descStart != std::string::npos) {
            size_t descEnd = itemXml.find("</description>", descStart);
            if (descEnd != std::string::npos) {
                size_t descValueStart = itemXml.find(">", descStart);
                if (descValueStart != std::string::npos) {
                    descValueStart++;
                    fi.description = itemXml.substr(descValueStart, descEnd - descValueStart);
                }
            }
        } else {
            size_t summaryStart = itemXml.find("<summary");
            if (summaryStart != std::string::npos) {
                size_t summaryEnd = itemXml.find("</summary>", summaryStart);
                if (summaryEnd != std::string::npos) {
                    size_t sumValueStart = itemXml.find(">", summaryStart);
                    if (sumValueStart != std::string::npos) {
                        sumValueStart++;
                        fi.description = itemXml.substr(sumValueStart, summaryEnd - sumValueStart);
                    }
                }
            }
        }
        if (!fi.description.empty()) {
            bool inTag = false;
            std::string clean;
            for (char c : fi.description) {
                if (c == '<') inTag = true;
                else if (c == '>') inTag = false;
                else if (!inTag) clean += c;
            }
            fi.description = clean;
        }
        if (fi.guid.empty()) fi.guid = fi.link;
        items.push_back(fi);
        pos = end + itemClose.length();
    }
    return items;
}

// ------------------------------------------------------------------
// RSS check and send
// ------------------------------------------------------------------
void checkFeed(Feed& feed) {
    if (!isRssEnabled()) return;
    std::string xml = httpGet(feed.url);
    if (xml.empty()) return;
    auto newItems = parseFeed(xml, feed.url);
    if (newItems.empty()) return;
    std::vector<FeedItem> fresh;
    for (auto& item : newItems) {
        bool found = false;
        for (auto& old : feed.items) {
            if (old.guid == item.guid) { found = true; break; }
        }
        if (!found) fresh.push_back(item);
    }
    if (fresh.empty()) return;
    for (auto& item : fresh) feed.items.push_back(item);
    if (feed.items.size() > 50) feed.items.erase(feed.items.begin(), feed.items.end() - 50);
    for (auto& user : rssUsers) {
        if (!user.enabled) continue;
        if (!feed.force && user.muted[feed.tag]) continue;
        std::string msg = "[" + feed.tag + "] " + (feed.force ? "[FORCED] " : "") + "New: " + fresh.back().title + " - " + fresh.back().link;
        sendToNick(user.nick, msg);
    }
}

void rssWorker() {
    while (rssRunning) {
        std::this_thread::sleep_for(std::chrono::minutes(refreshMinutes));
        std::lock_guard<std::mutex> lock(rssMutex);
        if (isRssEnabled()) {
            for (auto& feed : feeds) checkFeed(feed);
            saveFeeds();
        }
    }
}

// ------------------------------------------------------------------
// RSS command handlers
// ------------------------------------------------------------------
void cmdRssList(const std::string& fromSid) {
    if (!isRssEnabled()) { sendPrivateMessage(fromSid, "RSS is currently disabled."); return; }
    std::string reply = "Feeds:\n";
    int i=1;
    for (auto& feed : feeds) {
        reply += std::to_string(i++) + ". " + feed.tag + " (" + feed.url + ")";
        if (feed.force) reply += " [force]";
        reply += "\n";
    }
    sendPrivateMessage(fromSid, reply);
}

void cmdRssAdd(const std::string& fromSid, const std::string& url, const std::string& tag) {
    if (!isRssEnabled()) { sendPrivateMessage(fromSid, "RSS is currently disabled."); return; }
    if (url.empty() || tag.empty()) { sendPrivateMessage(fromSid, "Usage: !rss add <url> <tag>"); return; }
    for (auto& f : feeds) if (f.tag == tag) { sendPrivateMessage(fromSid, "Tag already exists."); return; }
    Feed f; f.url=url; f.tag=tag; f.force=false; feeds.push_back(f); saveFeeds();
    sendPrivateMessage(fromSid, "Feed added: " + tag);
}

void cmdRssRemove(const std::string& fromSid, const std::string& tag) {
    if (!isRssEnabled()) { sendPrivateMessage(fromSid, "RSS is currently disabled."); return; }
    auto it = std::find_if(feeds.begin(), feeds.end(), [&](const Feed& f){ return f.tag == tag; });
    if (it == feeds.end()) { sendPrivateMessage(fromSid, "Feed not found."); return; }
    feeds.erase(it); saveFeeds();
    sendPrivateMessage(fromSid, "Feed removed: " + tag);
}

void cmdRssForce(const std::string& fromSid, const std::string& tag) {
    if (!isRssEnabled()) { sendPrivateMessage(fromSid, "RSS is currently disabled."); return; }
    auto it = std::find_if(feeds.begin(), feeds.end(), [&](const Feed& f){ return f.tag == tag; });
    if (it == feeds.end()) { sendPrivateMessage(fromSid, "Feed not found."); return; }
    it->force = !it->force; saveFeeds();
    sendPrivateMessage(fromSid, "Feed " + tag + " force is now " + (it->force ? "ON" : "OFF"));
}

void cmdRssRefresh(const std::string& fromSid, int minutes) {
    if (!isRssEnabled()) { sendPrivateMessage(fromSid, "RSS is currently disabled."); return; }
    if (minutes < 1) minutes = 1;
    refreshMinutes = minutes;
    sendPrivateMessage(fromSid, "Refresh interval set to " + std::to_string(minutes) + " minutes.");
}

void cmdRssSubscribe(const std::string& fromSid, const std::string& nick) {
    if (!isRssEnabled()) { sendPrivateMessage(fromSid, "RSS is currently disabled."); return; }
    auto it = std::find_if(rssUsers.begin(), rssUsers.end(), [&](const RssUser& u){ return u.nick == nick; });
    if (it == rssUsers.end()) {
        RssUser u; u.nick=nick; u.enabled=true; u.pm=true; rssUsers.push_back(u); saveRssUsers();
        sendPrivateMessage(fromSid, "User " + nick + " subscribed (PM).");
    } else {
        it->enabled = true; saveRssUsers();
        sendPrivateMessage(fromSid, "User " + nick + " already subscribed, enabled.");
    }
}

void cmdRssUnsubscribe(const std::string& fromSid, const std::string& nick) {
    if (!isRssEnabled()) { sendPrivateMessage(fromSid, "RSS is currently disabled."); return; }
    auto it = std::find_if(rssUsers.begin(), rssUsers.end(), [&](const RssUser& u){ return u.nick == nick; });
    if (it == rssUsers.end()) { sendPrivateMessage(fromSid, "User not subscribed."); return; }
    it->enabled = false; saveRssUsers();
    sendPrivateMessage(fromSid, "User " + nick + " unsubscribed.");
}

void cmdRssMute(const std::string& fromSid, const std::string& tag) {
    if (!isRssEnabled()) { sendPrivateMessage(fromSid, "RSS is currently disabled."); return; }
    auto it = std::find_if(rssUsers.begin(), rssUsers.end(), [&](const RssUser& u){ return u.nick == fromSid; });
    if (it == rssUsers.end()) { sendPrivateMessage(fromSid, "You are not subscribed. Use !rss subscribe <nick> first."); return; }
    it->muted[tag] = true; saveRssUsers();
    sendPrivateMessage(fromSid, "Feed " + tag + " muted.");
}

void cmdRssUnmute(const std::string& fromSid, const std::string& tag) {
    if (!isRssEnabled()) { sendPrivateMessage(fromSid, "RSS is currently disabled."); return; }
    auto it = std::find_if(rssUsers.begin(), rssUsers.end(), [&](const RssUser& u){ return u.nick == fromSid; });
    if (it == rssUsers.end()) { sendPrivateMessage(fromSid, "You are not subscribed."); return; }
    it->muted[tag] = false; saveRssUsers();
    sendPrivateMessage(fromSid, "Feed " + tag + " unmuted.");
}

void cmdRssHelp(const std::string& fromSid) {
    if (!isRssEnabled()) { sendPrivateMessage(fromSid, "RSS is currently disabled."); return; }
    std::string help = "RSS Feed Commands:\n!rss list\n!rss add <url> <tag>\n!rss remove <tag>\n!rss force <tag>\n"
                       "!rss refresh <minutes>\n!rss subscribe <nick>\n!rss unsubscribe <nick>\n!rss mute <tag>\n!rss unmute <tag>\n!rss help";
    sendPrivateMessage(fromSid, help);
}

// ------------------------------------------------------------------
// Helper: send content of a text file
// ------------------------------------------------------------------
bool sendFileContent(const std::string& fromSid, const std::string& basename) {
    std::string safe = basename;
    for (char& c : safe) if (!isalnum(c) && c != '_' && c != '-') return false;
    fs::path filePath = fs::path(config.messages_dir) / (safe + ".txt");
    if (!fs::exists(filePath)) return false;
    std::ifstream f(filePath);
    if (!f.is_open()) return false;
    std::stringstream buffer; buffer << f.rdbuf();
    std::string content = buffer.str();
    if (content.empty()) sendPrivateMessage(fromSid, "File is empty.");
    else sendPrivateMessage(fromSid, content);
    return true;
}

// ------------------------------------------------------------------
// SSL helpers
// ------------------------------------------------------------------
SSL_CTX* init_ssl_ctx() {
    SSL_library_init();
    OpenSSL_add_all_algorithms();
    SSL_load_error_strings();
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) return nullptr;
    if (SSL_CTX_use_certificate_file(ctx, config.cert_file.c_str(), SSL_FILETYPE_PEM) != 1) { SSL_CTX_free(ctx); return nullptr; }
    if (SSL_CTX_use_PrivateKey_file(ctx, config.key_file.c_str(), SSL_FILETYPE_PEM) != 1) { SSL_CTX_free(ctx); return nullptr; }
    if (!SSL_CTX_check_private_key(ctx)) { SSL_CTX_free(ctx); return nullptr; }
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
    return ctx;
}

std::string getCertFingerprint(SSL* ssl) {
    X509* cert = SSL_get_peer_certificate(ssl);
    if (!cert) return "";
    unsigned char sha256[SHA256_DIGEST_LENGTH]; unsigned int len = SHA256_DIGEST_LENGTH;
    X509_digest(cert, EVP_sha256(), sha256, &len);
    X509_free(cert);
    return ADCLibCore::base32_encode(sha256, len);
}

SSL* makeTlsConnection(SSL_CTX* ctx, const std::string& host, int port, const std::string& expectedKeyprint) {
    WSADATA wsa; if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) return nullptr;
    struct addrinfo hints={}, *res=nullptr;
    hints.ai_family=AF_INET; hints.ai_socktype=SOCK_STREAM;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0) return nullptr;
    SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) { freeaddrinfo(res); return nullptr; }
    if (connect(sock, res->ai_addr, (int)res->ai_addrlen) == SOCKET_ERROR) { freeaddrinfo(res); closesocket(sock); return nullptr; }
    freeaddrinfo(res);
    SSL* ssl = SSL_new(ctx); SSL_set_fd(ssl, (int)sock);
    if (SSL_connect(ssl) != 1) { SSL_free(ssl); closesocket(sock); return nullptr; }
    std::string fp = getCertFingerprint(ssl);
    if (fp != expectedKeyprint) { SSL_free(ssl); closesocket(sock); return nullptr; }
    return ssl;
}

SOCKET makePlainConnection(const std::string& host, int port) {
    WSADATA wsa; if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) return INVALID_SOCKET;
    struct addrinfo hints={}, *res=nullptr;
    hints.ai_family=AF_INET; hints.ai_socktype=SOCK_STREAM;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0) return INVALID_SOCKET;
    SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) { freeaddrinfo(res); return INVALID_SOCKET; }
    if (connect(sock, res->ai_addr, (int)res->ai_addrlen) == SOCKET_ERROR) { freeaddrinfo(res); closesocket(sock); return INVALID_SOCKET; }
    freeaddrinfo(res);
    return sock;
}

// ------------------------------------------------------------------
// Main
// ------------------------------------------------------------------
int main() {
    loadConfig();

    HubInfo hubInfo;
    try { hubInfo = parseHubUrl(config.hub_url); }
    catch (const std::exception& e) { std::cerr << "Invalid hub URL: " << e.what() << std::endl; return 1; }

    std::string pid = loadOrGeneratePid();
    std::string cid = ADCLibCore::hash_pid(pid);

    loadFeeds();
    loadRssUsers();
    curl_global_init(CURL_GLOBAL_ALL);

    if (hubInfo.isSecure) {
        useTls = true;
        SSL_CTX* ctx = init_ssl_ctx();
        if (!ctx) { std::cerr << "SSL_CTX init failed\n"; return 1; }
        hubSsl = makeTlsConnection(ctx, hubInfo.host, hubInfo.port, hubInfo.keyprint);
        if (!hubSsl) { std::cerr << "TLS connection failed\n"; SSL_CTX_free(ctx); return 1; }
    } else {
        useTls = false;
        plainSock = makePlainConnection(hubInfo.host, hubInfo.port);
        if (plainSock == INVALID_SOCKET) { std::cerr << "Plain TCP connection failed\n"; return 1; }
    }

    std::thread kaThread(keepAlive); kaThread.detach();

    sendLine("HSUP ADBASE ADTIGR");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    for (int i=0; i<20; ++i) {
        std::string line = recvLine();
        if (line.empty()) break;
        if (line.substr(0,4) == "ISID") { mySid = line.substr(5,4); break; }
    }
    if (mySid.empty()) { std::cerr << "No SID assigned\n"; return 1; }

    std::string i4 = "0.0.0.0", u4 = "";
    std::string binf = "BINF " + mySid +
                       " PD" + pid +
                       " ID" + cid +
                       " NI" + config.bot_nick +
                       " SL1 FS0 SS0 SF1 HN0 HR0 HO0" +
                       " VEFileBot/1.0" +
                       " LCen-US" +
                       " US125000000 DS125000000" +
                       " KPSHA256/" + (hubInfo.isSecure ? hubInfo.keyprint : "0000000000000000000000000000000000000000") +
                       " SUADC0,CCPM,TCP4,UDP4,ASCH" +
                       " I4" + i4 +
                       (u4.empty() ? "" : " U4" + u4) +
                       " U0";
    sendLine(binf);

    std::string salt;
    for (int i=0; i<10; ++i) {
        std::string line = recvLine();
        if (line.empty()) break;
        if (line.substr(0,4) == "IGPA") { salt = line.substr(5); break; }
    }
    if (salt.empty()) { std::cerr << "No IGPA received\n"; return 1; }

    std::string pdHash = ADCLibCore::hash_pas(config.bot_password, salt);
    sendLine("HPAS " + pdHash);

    bool loggedIn = false, retried = false;
    while (!loggedIn) {
        std::string line = recvLine();
        if (line.empty()) break;
        if (line.substr(0,4) == "ISTR" || line.substr(0,4) == "BINF" || line.substr(0,4) == "IINF") {
            loggedIn = true; break;
        }
        if (line.substr(0,4) == "ISTA") {
            if (line.find("223") != std::string::npos && !retried) {
                retried = true;
                pdHash = ADCLibCore::hash_pas_oldschool(config.bot_password, salt, cid);
                sendLine("HPAS " + pdHash);
            } else break;
        }
    }
    if (!loggedIn) { std::cerr << "Login failed\n"; return 1; }

    std::cout << "\n✅ Logged in as " << config.bot_nick << " (SID: " << mySid << ")\n";
    std::cout << "Commands: !ping, !help, !rules, !listfiles, !rss, !relhelp, !addcat, !delcat, !listcats\n";
    std::cout << "Hangman: !starthm, !playhm, !freehm, !hmhelp, !hmscore, !import, !addword, !chword, !delword, !exit\n";
    std::cout << "Any !<filename> sends messages/<filename>.txt\n";
    std::cout << "Press Ctrl+C to exit.\n\n";

    if (!fs::exists(config.messages_dir)) fs::create_directory(config.messages_dir);

    initReleaseManagement();
    std::thread rssThread(rssWorker); rssThread.detach();
    HangmanManager::getInstance().init();

    // ----- RSS enable/disable checker thread -----
    std::thread rssEnabledChecker([]{
        while (keepRunning) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            std::ifstream f("rss_enabled.txt");
            bool en = true;
            if (f.is_open()) {
                std::string s;
                std::getline(f, s);
                if (s == "0") en = false;
                f.close();
            }
            setRssEnabled(en);
        }
    });
    rssEnabledChecker.detach();

    // ----- Hangman enable/disable checker thread -----
    std::thread hangmanEnabledChecker([]{
        while (keepRunning) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            std::ifstream f("hangman_enabled.txt");
            bool en = true;
            if (f.is_open()) {
                std::string s;
                std::getline(f, s);
                if (s == "0") en = false;
                f.close();
            }
            HangmanManager::getInstance().setEnabled(en);
        }
    });
    hangmanEnabledChecker.detach();

    // ----- Release enable/disable checker thread -----
    std::thread releaseEnabledChecker([]{
        while (keepRunning) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            std::ifstream f("release_enabled.txt");
            bool en = true;
            if (f.is_open()) {
                std::string s;
                std::getline(f, s);
                if (s == "0") en = false;
                f.close();
            }
            setReleaseEnabled(en);
        }
    });
    releaseEnabledChecker.detach();

    // ---- Define a lambda for command routing ----
    auto routeCommand = [&](const std::string& fromSid, const std::string& senderNick, const std::string& msg) {
        // ----- Command routing -----
        if (msg == "!ping") {
            sendPrivateMessage(fromSid, "Pong!");
        }
        else if (msg == "!help") {
            std::string helpText = "Available commands:\n"
                                   "!ping - Ping the bot\n"
                                   "!help - Show this help\n"
                                   "!rules - Show hub rules (messages/rules.txt)\n"
                                   "!listfiles - List text files in messages folder\n"
                                   "!rss - RSS feed management (type !rss help)\n"
                                   "!relhelp - Release management\n"
                                   "!addcat, !delcat, !listcats - Manage release categories\n"
                                   "Hangman: !starthm, !playhm, !freehm, !hmhelp, !hmscore, !exit\n"
                                   "Admin: !addword, !chword, !delword, !import\n"
                                   "Any !<filename> sends messages/<filename>.txt";
            sendPrivateMessage(fromSid, helpText);
        }
        else if (msg == "!rules") {
            sendFileContent(fromSid, "rules");
        }
        else if (msg == "!listfiles") {
            std::string fileList = "Available text files:\n";
            for (const auto& entry : fs::directory_iterator(config.messages_dir)) {
                if (entry.is_regular_file() && entry.path().extension() == ".txt") {
                    std::string fname = entry.path().filename().string();
                    fname = fname.substr(0, fname.size()-4);
                    fileList += fname + "\n";
                }
            }
            if (fileList == "Available text files:\n") fileList += "None.";
            sendPrivateMessage(fromSid, fileList);
        }
        // Hangman
        else if (msg.rfind("!starthm",0)==0) {
            std::string args = msg.size()>8 ? msg.substr(8) : "";
            HangmanManager::getInstance().cmdStart(fromSid, senderNick, args);
        }
        else if (msg.rfind("!playhm",0)==0) {
            std::string args = msg.size()>7 ? msg.substr(7) : "";
            HangmanManager::getInstance().cmdPlay(fromSid, senderNick, args);
        }
        else if (msg == "!freehm") {
            HangmanManager::getInstance().cmdFree(fromSid, senderNick);
        }
        else if (msg == "!hmhelp") {
            HangmanManager::getInstance().cmdHelp(fromSid);
        }
        else if (msg.rfind("!hmscore",0)==0) {
            std::string args = msg.size()>8 ? msg.substr(8) : "";
            HangmanManager::getInstance().cmdScore(fromSid, args);
        }
        else if (msg == "!exit") {
            HangmanManager::getInstance().cmdExit(fromSid, senderNick);
        }
        else if (msg.rfind("!import",0)==0) {
            std::string args = msg.size()>7 ? msg.substr(7) : "";
            HangmanManager::getInstance().cmdImport(fromSid, args);
        }
        else if (msg.rfind("!addword",0)==0) {
            std::string args = msg.size()>8 ? msg.substr(8) : "";
            HangmanManager::getInstance().cmdAddWord(fromSid, args);
        }
        else if (msg.rfind("!chword",0)==0) {
            std::string args = msg.size()>7 ? msg.substr(7) : "";
            HangmanManager::getInstance().cmdEditWord(fromSid, args);
        }
        else if (msg.rfind("!delword",0)==0) {
            std::string args = msg.size()>8 ? msg.substr(8) : "";
            HangmanManager::getInstance().cmdDelWord(fromSid, args);
        }
        // RSS
        else if (msg.rfind("!rss",0)==0) {
            std::string rest = msg.size()>4 ? msg.substr(4) : "";
            size_t start = rest.find_first_not_of(" \t\r\n");
            if (start != std::string::npos) rest = rest.substr(start);
            else rest.clear();
            if (rest.empty()) { cmdRssHelp(fromSid); return; }
            std::istringstream iss2(rest);
            std::string subcmd; iss2 >> subcmd;
            if (subcmd == "list") cmdRssList(fromSid);
            else if (subcmd == "add") {
                std::string url, tag; iss2 >> url >> tag;
                if (url.empty() || tag.empty()) sendPrivateMessage(fromSid, "Usage: !rss add <url> <tag>");
                else cmdRssAdd(fromSid, url, tag);
            }
            else if (subcmd == "remove") {
                std::string tag; iss2 >> tag;
                if (tag.empty()) sendPrivateMessage(fromSid, "Usage: !rss remove <tag>");
                else cmdRssRemove(fromSid, tag);
            }
            else if (subcmd == "force") {
                std::string tag; iss2 >> tag;
                if (tag.empty()) sendPrivateMessage(fromSid, "Usage: !rss force <tag>");
                else cmdRssForce(fromSid, tag);
            }
            else if (subcmd == "refresh") {
                int minutes; iss2 >> minutes;
                if (minutes <= 0) sendPrivateMessage(fromSid, "Usage: !rss refresh <minutes>");
                else cmdRssRefresh(fromSid, minutes);
            }
            else if (subcmd == "subscribe") {
                std::string nick; iss2 >> nick;
                if (nick.empty()) sendPrivateMessage(fromSid, "Usage: !rss subscribe <nick>");
                else cmdRssSubscribe(fromSid, nick);
            }
            else if (subcmd == "unsubscribe") {
                std::string nick; iss2 >> nick;
                if (nick.empty()) sendPrivateMessage(fromSid, "Usage: !rss unsubscribe <nick>");
                else cmdRssUnsubscribe(fromSid, nick);
            }
            else if (subcmd == "mute") {
                std::string tag; iss2 >> tag;
                if (tag.empty()) sendPrivateMessage(fromSid, "Usage: !rss mute <tag>");
                else cmdRssMute(fromSid, tag);
            }
            else if (subcmd == "unmute") {
                std::string tag; iss2 >> tag;
                if (tag.empty()) sendPrivateMessage(fromSid, "Usage: !rss unmute <tag>");
                else cmdRssUnmute(fromSid, tag);
            }
            else if (subcmd == "help") cmdRssHelp(fromSid);
            else sendPrivateMessage(fromSid, "Unknown RSS subcommand. Try !rss help");
        }
        // Release commands
        else if (msg.rfind("!addrel",0)==0) {
            if (!isReleaseEnabled()) { sendPrivateMessage(fromSid, "Release management is currently disabled."); return; }
            std::string rest = msg.substr(7); rest.erase(0, rest.find_first_not_of(" \t\r\n"));
            size_t space = rest.find(' ');
            if (space == std::string::npos) sendPrivateMessage(fromSid, "Usage: !addrel <category> <releasename>");
            else {
                std::string cat = rest.substr(0, space);
                std::string name = rest.substr(space+1);
                name.erase(0, name.find_first_not_of(" \t\r\n"));
                if (!name.empty()) name.erase(name.find_last_not_of(" \t\r\n")+1);
                if (name.empty()) sendPrivateMessage(fromSid, "Usage: !addrel <category> <releasename>");
                else cmdAddRel(fromSid, cat, name);
            }
        }
        else if (msg.rfind("!releases",0)==0) {
            if (!isReleaseEnabled()) { sendPrivateMessage(fromSid, "Release management is currently disabled."); return; }
            std::string rest = msg.substr(9); rest.erase(0, rest.find_first_not_of(" \t\r\n"));
            if (rest.empty()) sendPrivateMessage(fromSid, "Usage: !releases <category> (or 'new' for newest)");
            else cmdShowRel(fromSid, rest);
        }
        else if (msg.rfind("!delrel",0)==0) {
            if (!isReleaseEnabled()) { sendPrivateMessage(fromSid, "Release management is currently disabled."); return; }
            std::string rest = msg.substr(7); rest.erase(0, rest.find_first_not_of(" \t\r\n"));
            if (rest.empty()) sendPrivateMessage(fromSid, "Usage: !delrel <id>");
            else { int id = std::stoi(rest); cmdDelRel(fromSid, id); }
        }
        else if (msg.rfind("!searchrel",0)==0) {
            if (!isReleaseEnabled()) { sendPrivateMessage(fromSid, "Release management is currently disabled."); return; }
            std::string search = msg.substr(10); search.erase(0, search.find_first_not_of(" \t\r\n"));
            if (search.empty()) sendPrivateMessage(fromSid, "Usage: !searchrel <search string>");
            else cmdSearchRel(fromSid, search);
        }
        else if (msg == "!topadders") {
            if (!isReleaseEnabled()) { sendPrivateMessage(fromSid, "Release management is currently disabled."); return; }
            cmdTopAdders(fromSid);
        }
        else if (msg == "!relhelp") {
            if (!isReleaseEnabled()) { sendPrivateMessage(fromSid, "Release management is currently disabled."); return; }
            cmdRelHelp(fromSid);
        }
        else if (msg.rfind("!prunerel",0)==0) {
            if (!isReleaseEnabled()) { sendPrivateMessage(fromSid, "Release management is currently disabled."); return; }
            std::string rest = msg.substr(9); rest.erase(0, rest.find_first_not_of(" \t\r\n"));
            int days = 360; if (!rest.empty()) days = std::stoi(rest);
            cmdPruneRel(fromSid, days);
        }
        else if (msg == "!reloadrel") {
            if (!isReleaseEnabled()) { sendPrivateMessage(fromSid, "Release management is currently disabled."); return; }
            cmdReloadRel(fromSid);
        }
        else if (msg.rfind("!announcerel",0)==0) {
            if (!isReleaseEnabled()) { sendPrivateMessage(fromSid, "Release management is currently disabled."); return; }
            std::string rest = msg.substr(12); rest.erase(0, rest.find_first_not_of(" \t\r\n"));
            std::istringstream iss2(rest);
            std::string alibi, cat, name; iss2 >> alibi >> cat >> name;
            if (alibi.empty() || cat.empty() || name.empty())
                sendPrivateMessage(fromSid, "Usage: !announcerel <real-nick> <category> <releasename>");
            else cmdAnnounceRel(fromSid, alibi, cat, name);
        }
        else if (msg == "!reloff") {
            if (!isReleaseEnabled()) { sendPrivateMessage(fromSid, "Release management is currently disabled."); return; }
            cmdRelOff(fromSid);
        }
        else if (msg == "!relon") {
            if (!isReleaseEnabled()) { sendPrivateMessage(fromSid, "Release management is currently disabled."); return; }
            cmdRelOn(fromSid);
        }
        else if (msg.rfind("!addcat",0)==0) {
            if (!isReleaseEnabled()) { sendPrivateMessage(fromSid, "Release management is currently disabled."); return; }
            std::string cat = msg.substr(7); cat.erase(0, cat.find_first_not_of(" \t\r\n"));
            if (cat.empty()) sendPrivateMessage(fromSid, "Usage: !addcat <category>");
            else cmdAddCategory(fromSid, cat);
        }
        else if (msg.rfind("!delcat",0)==0) {
            if (!isReleaseEnabled()) { sendPrivateMessage(fromSid, "Release management is currently disabled."); return; }
            std::string cat = msg.substr(7); cat.erase(0, cat.find_first_not_of(" \t\r\n"));
            if (cat.empty()) sendPrivateMessage(fromSid, "Usage: !delcat <category>");
            else cmdDelCategory(fromSid, cat);
        }
        else if (msg == "!listcats") {
            if (!isReleaseEnabled()) { sendPrivateMessage(fromSid, "Release management is currently disabled."); return; }
            cmdListCategories(fromSid);
        }
        // Dynamic file command (catch‑all)
        else if (msg.size() > 1 && msg[0] == '!') {
            std::string cmdName = msg.substr(1);
            if (!sendFileContent(fromSid, cmdName)) {
                sendPrivateMessage(fromSid, "Unknown command or no such file: " + msg);
            }
        }
        // Single‑letter guess for Hangman
        else if (msg.size() == 1 && isalpha(msg[0])) {
            HangmanManager::getInstance().processGuess(fromSid, senderNick, msg);
        }
    };

    // ---- Main event loop ----
    while (keepRunning) {
        std::string line = recvLine();
        if (line.empty()) break;

        if (line.substr(0,4) == "BINF") {
            std::string sid = line.substr(5,4);
            std::string nick = getParam(line, "NI");
            if (!nick.empty() && sid != mySid) {
                std::lock_guard<std::mutex> lock(nickMutex);
                nickToSid[nick] = sid;
            }
        }
        else if (line.substr(0,4) == "IQUI") {
            std::string sid = line.substr(5,4);
            std::lock_guard<std::mutex> lock(nickMutex);
            for (auto it = nickToSid.begin(); it != nickToSid.end(); ++it) {
                if (it->second == sid) { nickToSid.erase(it); break; }
            }
        }
        // ----- Handle private messages (EMSG / DMSG) -----
        else if (line.substr(0,4) == "EMSG" || line.substr(0,4) == "DMSG") {
            std::istringstream iss(line);
            std::string cmd, fromSid, toSid;
            iss >> cmd >> fromSid >> toSid;
            std::string rawMsg;
            std::getline(iss, rawMsg);
            if (!rawMsg.empty() && rawMsg[0] == ' ') rawMsg = rawMsg.substr(1);
            if (fromSid == mySid) continue;

            std::string msg;
            for (size_t i=0; i<rawMsg.size(); ++i) {
                if (rawMsg[i] == '\\' && i+1 < rawMsg.size()) {
                    char nx = rawMsg[++i];
                    if (nx == 's') msg += ' ';
                    else if (nx == 'n') msg += '\n';
                    else if (nx == '\\') msg += '\\';
                    else { msg += '\\'; msg += nx; }
                } else msg += rawMsg[i];
            }
            size_t pmPos = msg.find(" PM");
            if (pmPos != std::string::npos) msg = msg.substr(0, pmPos);
            msg.erase(0, msg.find_first_not_of(" \t\r\n"));
            if (!msg.empty()) msg.erase(msg.find_last_not_of(" \t\r\n") + 1);

            std::string senderNick = getNickBySid(fromSid);
            if (senderNick.empty()) senderNick = fromSid;

            routeCommand(fromSid, senderNick, msg);
        }
        // ----- Handle broadcast messages (BMSG) from the main chat -----
        else if (line.substr(0,4) == "BMSG") {
            std::istringstream iss(line);
            std::string cmd, fromSid;
            iss >> cmd >> fromSid;
            std::string rawMsg;
            std::getline(iss, rawMsg);
            if (!rawMsg.empty() && rawMsg[0] == ' ') rawMsg = rawMsg.substr(1);
            if (fromSid == mySid) continue;

            std::string msg;
            for (size_t i=0; i<rawMsg.size(); ++i) {
                if (rawMsg[i] == '\\' && i+1 < rawMsg.size()) {
                    char nx = rawMsg[++i];
                    if (nx == 's') msg += ' ';
                    else if (nx == 'n') msg += '\n';
                    else if (nx == '\\') msg += '\\';
                    else { msg += '\\'; msg += nx; }
                } else msg += rawMsg[i];
            }
            msg.erase(0, msg.find_first_not_of(" \t\r\n"));
            if (!msg.empty()) msg.erase(msg.find_last_not_of(" \t\r\n") + 1);

            std::string senderNick = getNickBySid(fromSid);
            if (senderNick.empty()) senderNick = fromSid;

            routeCommand(fromSid, senderNick, msg);
        }
    }

    keepRunning = false;
    HangmanManager::getInstance().shutdown();
    if (useTls && hubSsl) SSL_free(hubSsl);
    else if (!useTls && plainSock != INVALID_SOCKET) closesocket(plainSock);
    WSACleanup();
    curl_global_cleanup();
    return 0;
}