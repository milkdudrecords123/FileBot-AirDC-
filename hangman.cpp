// hangman.cpp – Hangman game (Single/Multi in PM, Free‑4‑All in main chat)
#include "hangman.h"
#include <iostream>
#include <sstream>
#include <algorithm>
#include <filesystem>
#include <ctime>
#include <regex>

namespace fs = std::filesystem;

// Helper: check ASCII
bool HangmanManager::isAscii(const std::string& str) {
    for (unsigned char c : str) if (c > 127) return false;
    return true;
}

// Singleton
HangmanManager& HangmanManager::getInstance() {
    static HangmanManager instance;
    return instance;
}

// Enable/disable
void HangmanManager::setEnabled(bool e) {
    std::lock_guard<std::mutex> lock(enabledMutex);
    enabled = e;
}

bool HangmanManager::isEnabled() const {
    std::lock_guard<std::mutex> lock(enabledMutex);
    return enabled;
}

// Initialization / Shutdown
void HangmanManager::init() {
    std::lock_guard<std::mutex> lock(mtx);
    loadWords();
    loadPoints();
    loadStars();
    if (words.empty()) {
        sendToOps(botName, "*** No word database. Use !import to create one.");
    }
    running = true;
    timerThread = std::thread(&HangmanManager::timerLoop, this);
}

void HangmanManager::shutdown() {
    {
        std::lock_guard<std::mutex> lock(mtx);
        running = false;
    }
    if (timerThread.joinable())
        timerThread.join();
    savePoints();
    saveStars();
}

// File I/O
void HangmanManager::loadWords() {
    words.clear();
    if (!fs::exists("Hangman")) return;
    std::ifstream f("Hangman/Word.list");
    if (!f.is_open()) return;
    std::string line;
    while (std::getline(f, line)) {
        if (line.size() >= (size_t)minWordLen && isAscii(line))
            words.push_back(line);
    }
    f.close();
}

void HangmanManager::saveWords() {
    if (!fs::exists("Hangman")) fs::create_directory("Hangman");
    std::ofstream f("Hangman/Word.list");
    for (const auto& w : words) f << w << "\n";
    f.close();
}

void HangmanManager::loadPoints() {
    points.clear();
    std::ifstream f("Hangman/Point.list");
    if (!f.is_open()) return;
    json j; f >> j;
    for (auto& [nick, modes] : j.items())
        for (auto& [mode, val] : modes.items())
            points[nick][mode] = val.get<int>();
}

void HangmanManager::savePoints() {
    json j;
    for (const auto& [nick, modes] : points)
        for (const auto& [mode, val] : modes)
            j[nick][mode] = val;
    std::ofstream f("Hangman/Point.list"); f << j.dump(4);
}

void HangmanManager::loadStars() {
    stars.clear();
    std::ifstream f("Hangman/Star.list");
    if (!f.is_open()) return;
    json j; f >> j;
    for (auto& [nick, val] : j.items()) stars[nick] = val.get<int>();
}

void HangmanManager::saveStars() {
    json j;
    for (const auto& [nick, val] : stars) j[nick] = val;
    std::ofstream f("Hangman/Star.list"); f << j.dump(4);
}

// Random word
std::string HangmanManager::randomWord() {
    if (words.empty()) return "";
    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, words.size()-1);
    return words[dis(gen)];
}

// Build secret
std::string HangmanManager::buildSecret(const std::string& word) {
    std::string secret;
    for (char c : word) {
        if (alphabet.find(tolower(c)) != std::string::npos)
            secret += icon;
        else
            secret += c;
    }
    return secret;
}

// Get stars
std::string HangmanManager::getStars(const std::string& nick) {
    auto it = stars.find(nick);
    if (it == stars.end() || it->second == 0) return "";
    std::string s;
    for (int i=0; i<it->second; ++i) s += icon;
    return s;
}

// Points
void HangmanManager::awardPoints(const std::string& nick, const std::string& mode, int lettersFound, int attemptsUsed) {
    int awarded = lettersFound * pointsRemAlpha + (7 - (attemptsUsed - 1)) * pointsRemTry;
    points[nick][mode] += awarded;
    savePoints();
}
void HangmanManager::detractPoints(const std::string& nick, const std::string& mode, int pts) {
    points[nick][mode] -= pts;
    savePoints();
}
void HangmanManager::checkWinner(const std::string& nick) {
    bool won = false;
    for (const auto& [mode, val] : points[nick])
        if (val >= winThreshold) won = true;
    if (won) {
        stars[nick]++;
        saveStars();
        for (auto& [mode, val] : points[nick]) val = 0;
        savePoints();
        sendToAll(botName, "*** " + nick + " has reached " + std::to_string(winThreshold) + " points in hangman!");
    }
}

// Graphics
std::string HangmanManager::getGraphics(int stage, bool correct, const std::string& word, const std::string& secret, int lettersLeft) {
    if (textOnly) {
        std::string out = "\n\n";
        out += "  Current word: " + secret + "  (" + std::to_string(lettersLeft) + " letters left)\n";
        out += "  Chances left: " + std::to_string(7 - (stage-1)) + "/7\n";
        return out;
    }
    static const std::vector<std::string> stages = {
        "   ______\n  |      |\n  |      \n  |      \n  |      \n__|__\n",
        "   ______\n  |      |\n  |      O\n  |      \n  |      \n__|__\n",
        "   ______\n  |      |\n  |      O\n  |     /|\\\n  |      \n__|__\n",
        "   ______\n  |      |\n  |      O\n  |     /|\\\n  |     / \\\n__|__\n"
    };
    int idx = std::min(stage-1, (int)stages.size()-1);
    std::string out = "\n\n";
    out += stages[idx];
    out += "  Word: " + secret + "  (" + std::to_string(lettersLeft) + " left)\n";
    out += "  Chances: " + std::to_string(7 - (stage-1)) + "/7\n";
    return out;
}

// Timer loop
void HangmanManager::timerLoop() {
    while (running) {
        std::this_thread::sleep_for(std::chrono::minutes(idleMinutes));
        std::lock_guard<std::mutex> lock(mtx);
        for (auto it = freeGames.begin(); it != freeGames.end(); ) {
            if (!it->second.active) {
                auto sid = freeActive.find(it->first);
                if (sid != freeActive.end())
                    sendPrivateMessage(sid->second, "*** You have been idle too long. Quitting Free‑4‑All.");
                freeActive.erase(it->first);
                it = freeGames.erase(it);
            } else {
                it->second.active = false;
                ++it;
            }
        }
    }
}

// Check built‑in commands
bool HangmanManager::isBuiltinCommand(const std::string& msg) {
    static const std::vector<std::string> cmds = {
        "ping", "help", "rules", "listfiles", "rss", "relhelp",
        "addrel", "releases", "delrel", "searchrel", "topadders",
        "prunerel", "reloadrel", "announcerel", "reloff", "relon",
        "addcat", "delcat", "listcats",
        "starthm", "playhm", "freehm", "hmhelp", "hmscore",
        "import", "addword", "delword", "chword", "exit"
    };
    return std::find(cmds.begin(), cmds.end(), msg) != cmds.end();
}

// ---------- Commands ----------

// Single Player – PM
void HangmanManager::cmdStart(const std::string& fromSid, const std::string& nick, const std::string& args) {
    if (!isEnabled()) { sendPrivateMessage(fromSid, "*** Hangman is currently disabled."); return; }
    std::lock_guard<std::mutex> lock(mtx);
    if (freeGames.find(nick) != freeGames.end()) {
        sendPrivateMessage(fromSid, "*** You are in Free‑4‑All. Please quit first (!freehm or !exit).");
        return;
    }
    if (multiGames.find(nick) != multiGames.end()) {
        sendPrivateMessage(fromSid, "*** You are in a two‑player game. Finish or quit first.");
        return;
    }
    if (singleGames.find(nick) != singleGames.end()) {
        sendPrivateMessage(fromSid, "*** Skipping current game. Word was: " + singleGames[nick].word);
        detractPoints(nick, "SINGLE", pointsSkip);
        singleGames.erase(nick);
    }
    std::string word = randomWord();
    if (word.empty()) {
        sendPrivateMessage(fromSid, "*** No words in database. Use !import.");
        return;
    }
    SingleGame g;
    g.word = word;
    g.secret = buildSecret(word);
    g.tried = "";
    g.alphabet = alphabet;
    g.attempts = 1;
    g.lettersLeft = std::count_if(word.begin(), word.end(), [this](char c){ return alphabet.find(tolower(c)) != std::string::npos; });
    singleGames[nick] = g;
    if (points.find(nick) == points.end()) points[nick] = {{"SINGLE",0}, {"MULTI",0}, {"FREE4ALL",0}};
    std::string msg = "*** Single‑player game started.\n";
    msg += getGraphics(1, true, word, g.secret, g.lettersLeft);
    msg += "Guess a letter (or !exit to quit): ";
    sendPrivateMessage(fromSid, msg);
}

// Two‑Player – PM to both players
void HangmanManager::cmdPlay(const std::string& fromSid, const std::string& nick, const std::string& args) {
    if (!isEnabled()) { sendPrivateMessage(fromSid, "*** Hangman is currently disabled."); return; }
    std::lock_guard<std::mutex> lock(mtx);
    std::istringstream iss(args);
    std::string oppNick;
    iss >> oppNick;
    if (oppNick.empty()) {
        sendPrivateMessage(fromSid, "*** Usage: !playhm <opponent> [rounds]");
        return;
    }
    if (oppNick == nick) {
        sendPrivateMessage(fromSid, "*** You cannot play against yourself.");
        return;
    }
    if (singleGames.find(nick) != singleGames.end()) {
        sendPrivateMessage(fromSid, "*** You are in a single game. Finish or quit first.");
        return;
    }
    if (freeGames.find(nick) != freeGames.end()) {
        sendPrivateMessage(fromSid, "*** You are in Free‑4‑All. Please quit first (!freehm or !exit).");
        return;
    }
    if (multiGames.find(nick) != multiGames.end()) {
        sendPrivateMessage(fromSid, "*** You already have a two‑player game.");
        return;
    }
    if (singleGames.find(oppNick) != singleGames.end() ||
        multiGames.find(oppNick) != multiGames.end() ||
        freeGames.find(oppNick) != freeGames.end()) {
        sendPrivateMessage(fromSid, "*** Opponent is already in a game.");
        return;
    }
    std::string word = randomWord();
    if (word.empty()) {
        sendPrivateMessage(fromSid, "*** No words in database. Use !import.");
        return;
    }
    MultiGame g;
    g.word = word;
    g.secret = buildSecret(word);
    g.tried = "";
    g.alphabet = alphabet;
    g.attempts = 1;
    g.lettersLeft = std::count_if(word.begin(), word.end(), [this](char c){ return alphabet.find(tolower(c)) != std::string::npos; });
    g.opponentNick = oppNick;
    multiGames[nick] = g;
    MultiGame g2 = g;
    g2.opponentNick = nick;
    multiGames[oppNick] = g2;
    if (points.find(nick) == points.end()) points[nick] = {{"SINGLE",0}, {"MULTI",0}, {"FREE4ALL",0}};
    if (points.find(oppNick) == points.end()) points[oppNick] = {{"SINGLE",0}, {"MULTI",0}, {"FREE4ALL",0}};
    std::string msg = "*** Two‑player game started between " + nick + " and " + oppNick + ".\n";
    msg += getGraphics(1, true, word, g.secret, g.lettersLeft);
    msg += "First to finish wins! Guess a letter (or !exit to quit): ";
    sendPrivateMessage(fromSid, msg);
    sendToNick(oppNick, "*** " + nick + " challenged you to Hangman! Game started.\n" + msg);
}

// Free‑4‑All – broadcast (public)
void HangmanManager::cmdFree(const std::string& fromSid, const std::string& nick) {
    if (!isEnabled()) { sendPrivateMessage(fromSid, "*** Hangman is currently disabled."); return; }
    std::lock_guard<std::mutex> lock(mtx);
    auto it = freeGames.find(nick);
    if (it != freeGames.end()) {
        freeActive.erase(nick);
        freeGames.erase(it);
        sendToAll(botName, "*** " + nick + " left the Free‑4‑All game.");
        return;
    }
    if (singleGames.find(nick) != singleGames.end()) {
        sendPrivateMessage(fromSid, "*** Exiting single-player game.");
        singleGames.erase(nick);
    }
    if (multiGames.find(nick) != multiGames.end()) {
        sendPrivateMessage(fromSid, "*** Exiting two‑player game.");
        auto opp = multiGames[nick].opponentNick;
        multiGames.erase(nick);
        multiGames.erase(opp);
        sendToNick(opp, "*** Your opponent " + nick + " left the game.");
    }
    std::string word;
    if (freeGames.empty()) {
        word = randomWord();
        if (word.empty()) {
            sendPrivateMessage(fromSid, "*** No words in database. Use !import.");
            return;
        }
    } else {
        word = freeGames.begin()->second.word;
    }
    FreeGame g;
    g.word = word;
    g.secret = buildSecret(word);
    g.tried = "";
    g.alphabet = alphabet;
    g.attempts = 1;
    g.lettersLeft = std::count_if(word.begin(), word.end(), [this](char c){ return alphabet.find(tolower(c)) != std::string::npos; });
    g.active = true;
    g.lastActivity = std::chrono::steady_clock::now();
    freeGames[nick] = g;
    freeActive[nick] = fromSid;
    if (points.find(nick) == points.end()) points[nick] = {{"SINGLE",0}, {"MULTI",0}, {"FREE4ALL",0}};
    std::string msg = "*** " + nick + " joined the Free‑4‑All game.\n";
    msg += getGraphics(1, true, word, g.secret, g.lettersLeft);
    msg += "Guess a letter (or !exit to quit): ";
    sendToAll(botName, msg);
}

// Help – PM
void HangmanManager::cmdHelp(const std::string& fromSid) {
    if (!isEnabled()) { sendPrivateMessage(fromSid, "*** Hangman is currently disabled."); return; }
    std::string help = "\n\n";
    help += "   O==============================================================O\n";
    help += "                         Hang-Man Commands:\n";
    help += "   o--------------------------------------------------------------o\n\n";
    help += "\t!starthm [games]       \t - Start a single player game (PM)\n";
    help += "\t!playhm <nick> [games] \t - Challenge a user to a two player game (PM)\n";
    help += "\t!freehm                \t - Join/Quit the 'Free 4 All' (public)\n";
    help += "\t!exit                  \t - Quit your current game (any mode)\n";
    help += "\t!hmhelp                \t - Show this help\n";
    help += "\t!hmscore [nick]        \t - Show scoreboard or user's scores\n";
    help += "\n   O==============================================================O\n";
    help += "                         Admin Commands:\n";
    help += "   o--------------------------------------------------------------o\n\n";
    help += "\t!addword <word>        \t - Add a word to the database\n";
    help += "\t!chword \"old\" \"new\"  \t - Edit a word\n";
    help += "\t!delword <word>        \t - Delete a word\n";
    help += "\t!import <mode> <file>  \t - Import word list (mode: a=append, w=overwrite)\n";
    sendPrivateMessage(fromSid, help);
}

// Score – PM
void HangmanManager::cmdScore(const std::string& fromSid, const std::string& args) {
    if (!isEnabled()) { sendPrivateMessage(fromSid, "*** Hangman is currently disabled."); return; }
    std::lock_guard<std::mutex> lock(mtx);
    if (!args.empty()) {
        auto it = points.find(args);
        if (it == points.end()) {
            sendPrivateMessage(fromSid, "*** No record for user: " + args);
            return;
        }
        std::string msg = "\n\n   O==============================================================O\n";
        msg += "\t\t Scoreboard for " + args + "\n";
        msg += "   o--------------------------------------------------------------o\n\n";
        msg += "\t\t Single Player Score \t: " + std::to_string(it->second["SINGLE"]) + " Points\n";
        msg += "\t\t Two Player Score    \t: " + std::to_string(it->second["MULTI"]) + " Points\n";
        msg += "\t\t 'Free 4 All' Score\t\t: " + std::to_string(it->second["FREE4ALL"]) + " Points\n";
        sendPrivateMessage(fromSid, msg);
        return;
    }
    std::map<std::string, int> totals;
    for (const auto& [nick, modes] : points) {
        int total = 0;
        for (const auto& [mode, val] : modes) total += val;
        totals[nick] = total;
    }
    std::vector<std::pair<std::string, int>> sorted(totals.begin(), totals.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    std::string msg = "\n\n   O==============================================================O\n";
    msg += "\t\t Top Players Scoreboard \n";
    msg += "   o--------------------------------------------------------------o\n\n";
    int count = 0;
    for (const auto& [nick, score] : sorted) {
        if (++count > maxScoreDisplay) break;
        msg += "\t\t " + std::to_string(count) + ". " + getStars(nick) + " " + nick + " : " + std::to_string(score) + " Points\n";
    }
    sendPrivateMessage(fromSid, msg);
}

// Admin commands – remain broadcast
void HangmanManager::cmdImport(const std::string& fromSid, const std::string& args) {
    if (!isEnabled()) { sendPrivateMessage(fromSid, "*** Hangman is currently disabled."); return; }
    std::lock_guard<std::mutex> lock(mtx);
    std::istringstream iss(args);
    std::string mode, filename;
    iss >> mode >> filename;
    if (mode.empty() || filename.empty()) {
        sendToAll(botName, "*** Usage: !import <mode> <filename>\n   mode: a=append, w=overwrite");
        return;
    }
    if (mode != "a" && mode != "w") {
        sendToAll(botName, "*** Invalid mode. Use 'a' for append or 'w' for overwrite.");
        return;
    }
    std::ifstream f(filename);
    if (!f.is_open()) {
        f.open("Hangman/" + filename);
        if (!f.is_open()) {
            sendToAll(botName, "*** Cannot find file: " + filename);
            return;
        }
    }
    std::vector<std::string> newWords;
    std::string line;
    while (std::getline(f, line)) {
        if (line.size() >= (size_t)minWordLen && isAscii(line))
            newWords.push_back(line);
    }
    f.close();
    if (newWords.empty()) {
        sendToAll(botName, "*** No valid ASCII words of sufficient length found.");
        return;
    }
    if (mode == "w") words = newWords;
    else {
        words.insert(words.end(), newWords.begin(), newWords.end());
        std::sort(words.begin(), words.end());
        words.erase(std::unique(words.begin(), words.end()), words.end());
    }
    saveWords();
    sendToAll(botName, "*** Imported " + std::to_string(newWords.size()) + " words. Database now has " + std::to_string(words.size()) + " words.");
}

void HangmanManager::cmdAddWord(const std::string& fromSid, const std::string& args) {
    if (!isEnabled()) { sendPrivateMessage(fromSid, "*** Hangman is currently disabled."); return; }
    std::lock_guard<std::mutex> lock(mtx);
    if (args.empty()) {
        sendToAll(botName, "*** Usage: !addword <word>");
        return;
    }
    std::string word = args;
    std::transform(word.begin(), word.end(), word.begin(), ::tolower);
    if (!isAscii(word)) {
        sendToAll(botName, "*** Word must contain only ASCII letters.");
        return;
    }
    if (word.size() < (size_t)minWordLen) {
        sendToAll(botName, "*** Word must be at least " + std::to_string(minWordLen) + " letters.");
        return;
    }
    if (std::find(words.begin(), words.end(), word) != words.end()) {
        sendToAll(botName, "*** Word already exists.");
        return;
    }
    words.push_back(word);
    std::sort(words.begin(), words.end());
    saveWords();
    sendToAll(botName, "*** Word added: " + word);
}

void HangmanManager::cmdDelWord(const std::string& fromSid, const std::string& args) {
    if (!isEnabled()) { sendPrivateMessage(fromSid, "*** Hangman is currently disabled."); return; }
    std::lock_guard<std::mutex> lock(mtx);
    if (args.empty()) {
        sendToAll(botName, "*** Usage: !delword <word>");
        return;
    }
    std::string word = args;
    std::transform(word.begin(), word.end(), word.begin(), ::tolower);
    auto it = std::find(words.begin(), words.end(), word);
    if (it == words.end()) {
        sendToAll(botName, "*** Word not found.");
        return;
    }
    words.erase(it);
    saveWords();
    sendToAll(botName, "*** Word deleted: " + word);
}

void HangmanManager::cmdEditWord(const std::string& fromSid, const std::string& args) {
    if (!isEnabled()) { sendPrivateMessage(fromSid, "*** Hangman is currently disabled."); return; }
    std::lock_guard<std::mutex> lock(mtx);
    std::istringstream iss(args);
    std::string oldWord, newWord;
    iss >> oldWord >> newWord;
    if (oldWord.empty() || newWord.empty()) {
        sendToAll(botName, "*** Usage: !chword \"old\" \"new\"");
        return;
    }
    std::transform(oldWord.begin(), oldWord.end(), oldWord.begin(), ::tolower);
    std::transform(newWord.begin(), newWord.end(), newWord.begin(), ::tolower);
    if (!isAscii(newWord)) {
        sendToAll(botName, "*** New word must contain only ASCII letters.");
        return;
    }
    if (newWord.size() < (size_t)minWordLen) {
        sendToAll(botName, "*** New word must be at least " + std::to_string(minWordLen) + " letters.");
        return;
    }
    auto it = std::find(words.begin(), words.end(), oldWord);
    if (it == words.end()) {
        sendToAll(botName, "*** Word not found.");
        return;
    }
    *it = newWord;
    std::sort(words.begin(), words.end());
    saveWords();
    sendToAll(botName, "*** Word changed from '" + oldWord + "' to '" + newWord + "'.");
}

// New: Quit any game
void HangmanManager::cmdExit(const std::string& fromSid, const std::string& nick) {
    if (!isEnabled()) { sendPrivateMessage(fromSid, "*** Hangman is currently disabled."); return; }
    std::lock_guard<std::mutex> lock(mtx);
    // Single
    auto itSingle = singleGames.find(nick);
    if (itSingle != singleGames.end()) {
        sendPrivateMessage(fromSid, "*** You have quit the single-player game.");
        singleGames.erase(itSingle);
        return;
    }
    // Multi
    auto itMulti = multiGames.find(nick);
    if (itMulti != multiGames.end()) {
        std::string opp = itMulti->second.opponentNick;
        sendPrivateMessage(fromSid, "*** You have quit the two-player game.");
        sendToNick(opp, "*** " + nick + " has quit the game. You win by default!");
        multiGames.erase(nick);
        multiGames.erase(opp);
        return;
    }
    // Free-4-All
    auto itFree = freeGames.find(nick);
    if (itFree != freeGames.end()) {
        freeActive.erase(nick);
        freeGames.erase(itFree);
        sendToAll(botName, "*** " + nick + " left the Free‑4‑All game.");
        return;
    }
    sendPrivateMessage(fromSid, "*** You are not currently in any Hangman game.");
}

// ---------- Process a guess ----------
void HangmanManager::processGuess(const std::string& fromSid, const std::string& nick, const std::string& msg) {
    if (!isEnabled()) { sendPrivateMessage(fromSid, "*** Hangman is currently disabled."); return; }
    if (msg.size() != 1) return;
    char letter = tolower(msg[0]);
    if (alphabet.find(letter) == std::string::npos) return;

    std::lock_guard<std::mutex> lock(mtx);

    // Single player – PM to the player
    auto itSingle = singleGames.find(nick);
    if (itSingle != singleGames.end()) {
        auto& g = itSingle->second;
        if (g.tried.find(letter) != std::string::npos) {
            sendPrivateMessage(fromSid, "*** You already tried '" + std::string(1, letter) + "'.");
            return;
        }
        g.tried.push_back(letter);
        int found = 0;
        std::string newSecret = g.secret;
        for (size_t i = 0; i < g.word.size(); ++i) {
            if (tolower(g.word[i]) == letter) {
                newSecret[i] = g.word[i];
                found++;
            }
        }
        if (found > 0) {
            g.secret = newSecret;
            g.lettersLeft -= found;
            std::string msgOut = "*** Correct! '" + std::string(1, letter) + "' found " + std::to_string(found) + " time(s).\n";
            msgOut += getGraphics(g.attempts, true, g.word, g.secret, g.lettersLeft);
            if (g.lettersLeft == 0) {
                awardPoints(nick, "SINGLE", 0, g.attempts);
                msgOut += "*** You finished the word! Points awarded.\n";
                checkWinner(nick);
                singleGames.erase(nick);
            } else {
                msgOut += "Guess a letter (or !exit to quit): ";
            }
            sendPrivateMessage(fromSid, msgOut);
        } else {
            g.attempts++;
            if (g.attempts > 7) {
                sendPrivateMessage(fromSid, "*** Wrong! You have been hung! The word was: " + g.word + "\n" + getGraphics(8, false, g.word, g.secret, g.lettersLeft));
                singleGames.erase(nick);
            } else {
                sendPrivateMessage(fromSid, "*** Wrong! Attempt " + std::to_string(g.attempts) + "/7\n" + getGraphics(g.attempts, false, g.word, g.secret, g.lettersLeft) + "Guess a letter (or !exit to quit): ");
            }
        }
        return;
    }

    // Two‑player – PM to both players
    auto itMulti = multiGames.find(nick);
    if (itMulti != multiGames.end()) {
        auto& g = itMulti->second;
        if (g.tried.find(letter) != std::string::npos) {
            sendPrivateMessage(fromSid, "*** You already tried '" + std::string(1, letter) + "'.");
            return;
        }
        g.tried.push_back(letter);
        int found = 0;
        std::string newSecret = g.secret;
        for (size_t i = 0; i < g.word.size(); ++i) {
            if (tolower(g.word[i]) == letter) {
                newSecret[i] = g.word[i];
                found++;
            }
        }
        if (found > 0) {
            g.secret = newSecret;
            g.lettersLeft -= found;
            std::string msgOut = "*** Correct! '" + std::string(1, letter) + "' found " + std::to_string(found) + " time(s).\n";
            msgOut += getGraphics(g.attempts, true, g.word, g.secret, g.lettersLeft);
            if (g.lettersLeft == 0) {
                awardPoints(nick, "MULTI", 0, g.attempts);
                msgOut += "*** You finished the word! You win the two‑player game!\n";
                checkWinner(nick);
                sendToNick(g.opponentNick, "*** " + nick + " finished the word! You lose.");
                sendPrivateMessage(fromSid, msgOut);
                multiGames.erase(nick);
                multiGames.erase(g.opponentNick);
            } else {
                msgOut += "Guess a letter (or !exit to quit): ";
                sendPrivateMessage(fromSid, msgOut);
                sendToNick(g.opponentNick, "*** " + nick + " guessed correctly.\n" + getGraphics(g.attempts, true, g.word, g.secret, g.lettersLeft) + "Your turn? (wait for opponent)");
            }
        } else {
            g.attempts++;
            if (g.attempts > 7) {
                sendPrivateMessage(fromSid, "*** Wrong! You have been hung! The word was: " + g.word + "\n");
                sendToNick(g.opponentNick, "*** " + nick + " has been hung! You win!");
                multiGames.erase(nick);
                multiGames.erase(g.opponentNick);
            } else {
                sendPrivateMessage(fromSid, "*** Wrong! Attempt " + std::to_string(g.attempts) + "/7\n" + getGraphics(g.attempts, false, g.word, g.secret, g.lettersLeft) + "Guess a letter (or !exit to quit): ");
                sendToNick(g.opponentNick, "*** " + nick + " guessed wrong. They have " + std::to_string(7 - g.attempts) + " chances left.\n" + getGraphics(g.attempts, false, g.word, g.secret, g.lettersLeft));
            }
        }
        return;
    }

    // Free‑4‑All – broadcast
    auto itFree = freeGames.find(nick);
    if (itFree != freeGames.end()) {
        auto& g = itFree->second;
        g.active = true;
        if (g.tried.find(letter) != std::string::npos) {
            sendPrivateMessage(fromSid, "*** You already tried '" + std::string(1, letter) + "'.");
            return;
        }
        g.tried.push_back(letter);
        int found = 0;
        std::string newSecret = g.secret;
        for (size_t i = 0; i < g.word.size(); ++i) {
            if (tolower(g.word[i]) == letter) {
                newSecret[i] = g.word[i];
                found++;
            }
        }
        if (found > 0) {
            g.secret = newSecret;
            g.lettersLeft -= found;
            std::string msgOut = "*** " + nick + " correct! '" + std::string(1, letter) + "' found " + std::to_string(found) + " time(s).\n";
            msgOut += getGraphics(g.attempts, true, g.word, g.secret, g.lettersLeft);
            if (g.lettersLeft == 0) {
                awardPoints(nick, "FREE4ALL", 0, g.attempts);
                msgOut += "*** " + nick + " finished the word! " + nick + " wins the Free‑4‑All!\n";
                checkWinner(nick);
                sendToAll(botName, "*** Game ended. New game will start when someone joins.");
                freeGames.clear();
                freeActive.clear();
            } else {
                msgOut += "Guess a letter (or !exit to quit): ";
            }
            sendToAll(botName, msgOut);
        } else {
            g.attempts++;
            if (g.attempts > 7) {
                sendToAll(botName, "*** " + nick + " wrong! You have been hung! The word was: " + g.word + "\n");
                freeActive.erase(nick);
                freeGames.erase(nick);
                if (freeGames.empty())
                    sendToAll(botName, "*** All players eliminated. New game will start when someone joins.");
            } else {
                sendToAll(botName, "*** " + nick + " wrong! Attempt " + std::to_string(g.attempts) + "/7\n" + getGraphics(g.attempts, false, g.word, g.secret, g.lettersLeft) + "Guess a letter (or !exit to quit): ");
            }
        }
        return;
    }
}