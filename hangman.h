// hangman.h – Hangman game manager for ADC bot (main chat version)
#pragma once

#include <string>
#include <vector>
#include <map>
#include <random>
#include <mutex>
#include <thread>
#include <chrono>
#include <fstream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// Forward declarations (implemented in main.cpp)
void sendPrivateMessage(const std::string& targetSid, const std::string& message);
void sendToNick(const std::string& nick, const std::string& message);
void sendToAll(const std::string& from, const std::string& message);
void sendToOps(const std::string& from, const std::string& message);

class HangmanManager {
public:
    static HangmanManager& getInstance();

    void init();
    void shutdown();

    void cmdStart(const std::string& fromSid, const std::string& nick, const std::string& args);
    void cmdPlay(const std::string& fromSid, const std::string& nick, const std::string& args);
    void cmdFree(const std::string& fromSid, const std::string& nick);
    void cmdHelp(const std::string& fromSid);
    void cmdScore(const std::string& fromSid, const std::string& args);
    void cmdImport(const std::string& fromSid, const std::string& args);
    void cmdAddWord(const std::string& fromSid, const std::string& args);
    void cmdDelWord(const std::string& fromSid, const std::string& args);
    void cmdEditWord(const std::string& fromSid, const std::string& args);
    // New: quit any game
    void cmdExit(const std::string& fromSid, const std::string& nick);

    void processGuess(const std::string& fromSid, const std::string& nick, const std::string& msg);

    // Enable/disable Hangman
    void setEnabled(bool e);
    bool isEnabled() const;

private:
    HangmanManager() = default;
    ~HangmanManager() { shutdown(); }

    struct SingleGame {
        std::string word, secret, tried, alphabet;
        int attempts, lettersLeft;
    };
    struct MultiGame {
        std::string word, secret, tried, alphabet;
        int attempts, lettersLeft;
        std::string opponentNick;
        bool opponentActive;
    };
    struct FreeGame {
        std::string word, secret, tried, alphabet;
        int attempts, lettersLeft;
        bool active;
        std::chrono::steady_clock::time_point lastActivity;
    };

    std::vector<std::string> words;
    std::map<std::string, int> stars;
    std::map<std::string, std::map<std::string, int>> points;
    std::map<std::string, SingleGame> singleGames;
    std::map<std::string, MultiGame> multiGames;
    std::map<std::string, FreeGame> freeGames;
    std::map<std::string, std::string> freeActive;
    mutable std::mutex mtx;
    std::thread timerThread;
    bool running = false;
    std::string alphabet = "abcdefghijklmnopqrstuvwxyz";
    std::string botName = "•Hang-Man•";

    int minWordLen = 5;
    int pointsRemTry = 7;
    int pointsRemAlpha = 2;
    int pointsSkip = 1;
    int winThreshold = 1500;
    int idleMinutes = 2;
    int maxScoreDisplay = 10;
    bool textOnly = false;
    std::string icon = "*";   // ASCII only

    // Enable/disable flag and mutex
    bool enabled = true;
    mutable std::mutex enabledMutex;

    std::string randomWord();
    void saveWords();
    void loadWords();
    void savePoints();
    void loadPoints();
    void saveStars();
    void loadStars();
    std::string buildSecret(const std::string& word);
    std::string getStars(const std::string& nick);
    std::string getGraphics(int stage, bool correct, const std::string& word = "",
                            const std::string& secret = "", int lettersLeft = 0);
    void awardPoints(const std::string& nick, const std::string& mode, int lettersFound, int attemptsUsed);
    void detractPoints(const std::string& nick, const std::string& mode, int points);
    void checkWinner(const std::string& nick);
    void timerLoop();
    bool isBuiltinCommand(const std::string& msg);
    bool isAscii(const std::string& str);
};

#define Hangman HangmanManager::getInstance()