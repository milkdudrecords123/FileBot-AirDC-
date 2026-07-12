// release.cpp – Release management with dynamic categories (save to categories.json)
#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <ctime>
#include <iomanip>
#include <nlohmann/json.hpp>

using json = nlohmann::json;
using namespace std::literals;

extern void sendPrivateMessage(const std::string& targetSid, const std::string& message);
extern void sendToNick(const std::string& nick, const std::string& message);

struct Release {
    int id;
    std::string category;
    std::string adder;
    std::string date;
    std::string name;
};

static std::vector<Release> releases;
static int nextId = 1;
static std::map<std::string, int> topAdders;
static std::map<std::string, std::string> categories; // category name -> display name (same for now)
static std::map<std::string, bool> releaseOptOut;
static std::string categoriesFile = "categories.json";

std::string todayDate() {
    auto t = std::time(nullptr);
    auto tm = *std::localtime(&t);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d");
    return oss.str();
}

// Load categories from file
void loadCategories() {
    std::ifstream f(categoriesFile);
    if (!f.is_open()) {
        // Default categories if file missing
        categories["Movies"] = "Movies";
        categories["Music"] = "Music";
        categories["Games"] = "Games";
        return;
    }
    json j;
    f >> j;
    categories.clear();
    for (auto& [key, val] : j.items()) {
        categories[key] = val.get<std::string>();
    }
}

// Save categories to file
void saveCategories() {
    json j;
    for (const auto& [key, val] : categories) {
        j[key] = val;
    }
    std::ofstream f(categoriesFile);
    f << j.dump(4);
}

// Load releases from releases.json
void loadReleases() {
    std::ifstream f("releases.json");
    if (!f.is_open()) return;
    json j;
    f >> j;
    releases.clear();
    topAdders.clear();
    nextId = 1;
    for (const auto& item : j) {
        Release r;
        r.id = item["id"];
        r.category = item["category"];
        r.adder = item["adder"];
        r.date = item["date"];
        r.name = item["name"];
        releases.push_back(r);
        topAdders[r.adder]++;
        if (r.id >= nextId) nextId = r.id + 1;
    }
}

void saveReleases() {
    json j = json::array();
    for (const auto& r : releases) {
        j.push_back({
            {"id", r.id},
            {"category", r.category},
            {"adder", r.adder},
            {"date", r.date},
            {"name", r.name}
        });
    }
    std::ofstream f("releases.json");
    f << j.dump(4);
}

// Add a new category
void cmdAddCategory(const std::string& fromSid, const std::string& category) {
    std::string cat = category;
    // Capitalize first letter (optional)
    if (!cat.empty()) cat[0] = std::toupper(cat[0]);
    if (categories.find(cat) != categories.end()) {
        sendPrivateMessage(fromSid, "Category already exists: " + cat);
        return;
    }
    categories[cat] = cat;
    saveCategories();
    sendPrivateMessage(fromSid, "Category added: " + cat);
}

// Delete a category (only if no releases use it)
void cmdDelCategory(const std::string& fromSid, const std::string& category) {
    std::string cat = category;
    if (cat.empty()) {
        sendPrivateMessage(fromSid, "Usage: !delcat <category>");
        return;
    }
    auto it = categories.find(cat);
    if (it == categories.end()) {
        sendPrivateMessage(fromSid, "Category not found: " + cat);
        return;
    }
    // Check if any release uses this category
    for (const auto& r : releases) {
        if (r.category == cat) {
            sendPrivateMessage(fromSid, "Cannot delete category '" + cat + "' because it has releases. Remove them first.");
            return;
        }
    }
    categories.erase(it);
    saveCategories();
    sendPrivateMessage(fromSid, "Category deleted: " + cat);
}

// List all categories
void cmdListCategories(const std::string& fromSid) {
    if (categories.empty()) {
        sendPrivateMessage(fromSid, "No categories defined.");
        return;
    }
    std::string msg = "Available categories:\n";
    for (const auto& [name, disp] : categories) {
        msg += name + "\n";
    }
    sendPrivateMessage(fromSid, msg);
}

// Add release (checks category existence)
void cmdAddRel(const std::string& fromSid, const std::string& category, const std::string& releaseName) {
    if (categories.find(category) == categories.end()) {
        sendPrivateMessage(fromSid, "Unknown category. Use !listcats to see available categories.");
        return;
    }
    for (const auto& r : releases) {
        if (r.name == releaseName && r.category == category) {
            sendPrivateMessage(fromSid, "Release already exists in category " + category);
            return;
        }
    }
    Release r;
    r.id = nextId++;
    r.category = category;
    r.adder = "User"; // will be replaced later; main.cpp should pass actual nick
    r.date = todayDate();
    r.name = releaseName;
    releases.push_back(r);
    topAdders[r.adder]++;
    saveReleases();
    sendPrivateMessage(fromSid, "Release added: " + releaseName + " to " + category);
}

// Show releases (category or "new")
void cmdShowRel(const std::string& fromSid, const std::string& category) {
    if (category == "new") {
        const int MAX_NEW = 50;
        std::string msg = "=== Newest Releases ===\n";
        int count = 0;
        for (auto it = releases.rbegin(); it != releases.rend() && count < MAX_NEW; ++it, ++count) {
            msg += std::to_string(it->id) + ". [" + it->category + "] " + it->name + " (by " + it->adder + " on " + it->date + ")\n";
        }
        if (count == 0) msg += "No releases found.\n";
        sendPrivateMessage(fromSid, msg);
    } else {
        if (categories.find(category) == categories.end()) {
            sendPrivateMessage(fromSid, "Unknown category. Use !listcats to see available categories.");
            return;
        }
        std::string msg = "=== Releases in " + category + " ===\n";
        int count = 0;
        for (const auto& r : releases) {
            if (r.category == category) {
                msg += std::to_string(r.id) + ". " + r.name + " (by " + r.adder + " on " + r.date + ")\n";
                count++;
            }
        }
        if (count == 0) msg += "No releases found.\n";
        sendPrivateMessage(fromSid, msg);
    }
}

void cmdDelRel(const std::string& fromSid, int id) {
    auto it = std::find_if(releases.begin(), releases.end(), [id](const Release& r) { return r.id == id; });
    if (it == releases.end()) {
        sendPrivateMessage(fromSid, "Release not found.");
        return;
    }
    releases.erase(it);
    topAdders.clear();
    for (const auto& r : releases) topAdders[r.adder]++;
    saveReleases();
    sendPrivateMessage(fromSid, "Release ID " + std::to_string(id) + " deleted.");
}

void cmdSearchRel(const std::string& fromSid, const std::string& search) {
    std::string lowerSearch = search;
    std::transform(lowerSearch.begin(), lowerSearch.end(), lowerSearch.begin(), ::tolower);
    std::string msg = "=== Search results for '" + search + "' ===\n";
    int count = 0;
    for (const auto& r : releases) {
        std::string lowerName = r.name;
        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
        if (lowerName.find(lowerSearch) != std::string::npos) {
            msg += std::to_string(r.id) + ". [" + r.category + "] " + r.name + " (by " + r.adder + " on " + r.date + ")\n";
            count++;
        }
    }
    if (count == 0) msg += "No matches found.\n";
    sendPrivateMessage(fromSid, msg);
}

void cmdTopAdders(const std::string& fromSid) {
    std::vector<std::pair<std::string, int>> sorted;
    for (const auto& p : topAdders) sorted.push_back(p);
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    std::string msg = "=== Top Adders ===\n";
    int count = 0;
    for (const auto& p : sorted) {
        msg += p.first + ": " + std::to_string(p.second) + "\n";
        if (++count >= 20) break;
    }
    sendPrivateMessage(fromSid, msg);
}

void cmdRelHelp(const std::string& fromSid) {
    std::string help = "Release Commands:\n"
                       "!addrel <category> <name>\n"
                       "!releases <category|new>\n"
                       "!delrel <id>\n"
                       "!searchrel <text>\n"
                       "!topadders\n"
                       "!prunerel [days]\n"
                       "!reloadrel\n"
                       "!announcerel <real-nick> <category> <name>\n"
                       "!reloff\n"
                       "!relon\n"
                       "!addcat <category>\n"
                       "!delcat <category>\n"
                       "!listcats";
    sendPrivateMessage(fromSid, help);
}

void cmdPruneRel(const std::string& fromSid, int days) {
    if (days <= 0) days = 360;
    auto now = std::chrono::system_clock::now();
    std::time_t now_t = std::chrono::system_clock::to_time_t(now);
    std::string today = todayDate();
    std::tm today_tm = {};
    std::istringstream ss(today);
    ss >> std::get_time(&today_tm, "%Y-%m-%d");
    std::time_t today_time = std::mktime(&today_tm);
    const int SECONDS_PER_DAY = 86400;
    int removed = 0;
    for (auto it = releases.begin(); it != releases.end(); ) {
        std::tm tm = {};
        std::istringstream ss2(it->date);
        ss2 >> std::get_time(&tm, "%Y-%m-%d");
        std::time_t rel_time = std::mktime(&tm);
        double diff = std::difftime(today_time, rel_time) / SECONDS_PER_DAY;
        if (diff > days) {
            it = releases.erase(it);
            removed++;
        } else {
            ++it;
        }
    }
    topAdders.clear();
    for (const auto& r : releases) topAdders[r.adder]++;
    saveReleases();
    sendPrivateMessage(fromSid, "Pruned " + std::to_string(removed) + " releases older than " + std::to_string(days) + " days.");
}

void cmdReloadRel(const std::string& fromSid) {
    loadReleases();
    loadCategories();
    sendPrivateMessage(fromSid, "Releases and categories reloaded.");
}

void cmdAnnounceRel(const std::string& fromSid, const std::string& alibi, const std::string& category, const std::string& releaseName) {
    if (categories.find(category) == categories.end()) {
        sendPrivateMessage(fromSid, "Unknown category. Use !listcats.");
        return;
    }
    Release r;
    r.id = nextId++;
    r.category = category;
    r.adder = alibi;
    r.date = todayDate();
    r.name = releaseName;
    releases.push_back(r);
    topAdders[alibi]++;
    saveReleases();
    sendPrivateMessage(fromSid, "Release announced: " + releaseName + " by " + alibi);
}

void loadReleaseOptOut() {
    std::ifstream f("release_optout.json");
    if (!f.is_open()) return;
    json j;
    f >> j;
    for (auto& [nick, val] : j.items()) releaseOptOut[nick] = val.get<bool>();
}

void saveReleaseOptOut() {
    json j;
    for (const auto& p : releaseOptOut) j[p.first] = p.second;
    std::ofstream f("release_optout.json");
    f << j.dump(4);
}

void cmdRelOff(const std::string& fromSid) {
    releaseOptOut[fromSid] = true;
    saveReleaseOptOut();
    sendPrivateMessage(fromSid, "You will no longer receive release announcements.");
}

void cmdRelOn(const std::string& fromSid) {
    releaseOptOut[fromSid] = false;
    saveReleaseOptOut();
    sendPrivateMessage(fromSid, "You will now receive release announcements.");
}

void initReleaseManagement() {
    loadCategories();
    loadReleases();
    loadReleaseOptOut();
}