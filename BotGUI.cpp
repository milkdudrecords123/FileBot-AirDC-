// BotGUI.cpp – Win32 GUI wrapper for bot.exe with config editor, file manager, and console
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <process.h>
#include <string>
#include <fstream>
#include <sstream>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <nlohmann/json.hpp>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ws2_32.lib")

using json = nlohmann::json;
namespace fs = std::filesystem;

// Control IDs
#define IDC_HUB_URL         101
#define IDC_BOT_NICK        102
#define IDC_BOT_PASS        103
#define IDC_FEEDS_FILE      104
#define IDC_USERS_FILE      105
#define IDC_CERT_FILE       106
#define IDC_KEY_FILE        107
#define IDC_REFRESH         108
#define IDC_TCP_PORT        109
#define IDC_UDP_PORT        110
#define IDC_TLS_PORT        111
#define IDC_CONN_MODE       112
#define IDC_EXT_IP          113
#define IDC_FILE_LIST       114
#define IDC_BTN_ADD         115
#define IDC_BTN_EDIT        116
#define IDC_BTN_DEL         117
#define IDC_BTN_REFRESH     118
#define IDC_BTN_CONNECT     119
#define IDC_BTN_STOP        120
#define IDC_CONSOLE         121
#define IDC_BTN_SAVE        122
#define IDC_ENABLE_HANGMAN  123
#define IDC_ENABLE_RSS      124
#define IDC_ENABLE_RELEASE  125

// Custom message for console output
#define WM_APPEND_TEXT      (WM_USER + 100)

// Global handles
HWND g_hWndMain;
HWND g_hEdit[13];
HWND g_hFileList;
HWND g_hConsole;
HWND g_hBtnConnect, g_hBtnStop;
HWND g_hChkHangman, g_hChkRss, g_hChkRelease;
HANDLE g_hChildProcess = NULL;
HANDLE g_hChildStdoutRd = NULL, g_hChildStdoutWr = NULL;
HANDLE g_hChildStdinRd = NULL, g_hChildStdinWr = NULL;
PROCESS_INFORMATION g_pi;
bool g_bBotRunning = false;
std::string g_configPath = "config.json";

void AppendConsole(const std::string& text) {
    PostMessage(g_hWndMain, WM_APPEND_TEXT, 0, (LPARAM)new std::string(text));
}

unsigned __stdcall ReadStdoutThread(void* param) {
    char buf[4096];
    DWORD read;
    while (true) {
        if (!ReadFile(g_hChildStdoutRd, buf, sizeof(buf)-1, &read, NULL) || read == 0) break;
        buf[read] = '\0';
        AppendConsole(buf);
    }
    return 0;
}

bool StartBot() {
    if (g_bBotRunning) return false;

    SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };
    CreatePipe(&g_hChildStdoutRd, &g_hChildStdoutWr, &sa, 0);
    SetHandleInformation(g_hChildStdoutRd, HANDLE_FLAG_INHERIT, 0);
    CreatePipe(&g_hChildStdinRd, &g_hChildStdinWr, &sa, 0);
    SetHandleInformation(g_hChildStdinWr, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = { sizeof(STARTUPINFOA) };
    si.hStdOutput = g_hChildStdoutWr;
    si.hStdError = g_hChildStdoutWr;
    si.hStdInput = g_hChildStdinRd;
    si.dwFlags = STARTF_USESTDHANDLES;

    char cmdLine[] = "bot.exe";
    if (!CreateProcessA(NULL, cmdLine, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &g_pi)) {
        MessageBoxA(g_hWndMain, "Failed to start bot.exe. Make sure it exists.", "Error", MB_ICONERROR);
        CloseHandle(g_hChildStdoutRd); CloseHandle(g_hChildStdoutWr);
        CloseHandle(g_hChildStdinRd); CloseHandle(g_hChildStdinWr);
        return false;
    }

    CloseHandle(g_hChildStdoutWr);
    CloseHandle(g_hChildStdinRd);
    g_hChildProcess = g_pi.hProcess;
    _beginthreadex(NULL, 0, ReadStdoutThread, NULL, 0, NULL);

    g_bBotRunning = true;
    EnableWindow(g_hBtnConnect, FALSE);
    EnableWindow(g_hBtnStop, TRUE);
    AppendConsole(">>> Bot started.\r\n");
    return true;
}

void StopBot() {
    if (!g_bBotRunning) return;
    TerminateProcess(g_hChildProcess, 0);
    CloseHandle(g_hChildProcess);
    CloseHandle(g_hChildStdoutRd);
    CloseHandle(g_pi.hThread);
    g_hChildProcess = NULL;
    g_bBotRunning = false;
    EnableWindow(g_hBtnConnect, TRUE);
    EnableWindow(g_hBtnStop, FALSE);
    AppendConsole(">>> Bot stopped.\r\n");
}

void LoadConfig() {
    std::ifstream f(g_configPath);
    if (!f.is_open()) return;
    json j;
    f >> j;
    SetWindowTextA(g_hEdit[0], j.value("hub_url", "").c_str());
    SetWindowTextA(g_hEdit[1], j.value("bot_nick", "FileBot").c_str());
    SetWindowTextA(g_hEdit[2], j.value("bot_password", "").c_str());
    SetWindowTextA(g_hEdit[3], j.value("feeds_file", "feeds.json").c_str());
    SetWindowTextA(g_hEdit[4], j.value("users_file", "rss_users.json").c_str());
    SetWindowTextA(g_hEdit[5], j.value("cert_file", "client.crt.txt").c_str());
    SetWindowTextA(g_hEdit[6], j.value("key_file", "client.key.txt").c_str());
    SetWindowTextA(g_hEdit[7], std::to_string(j.value("refresh_minutes", 10)).c_str());
    
    if (j.contains("ports")) {
        SetWindowTextA(g_hEdit[8], std::to_string(j["ports"].value("tcp", 23168)).c_str());
        SetWindowTextA(g_hEdit[9], std::to_string(j["ports"].value("udp", 23168)).c_str());
        SetWindowTextA(g_hEdit[10], std::to_string(j["ports"].value("tls_tcp", 23169)).c_str());
    }
    if (j.contains("connection")) {
        SetWindowTextA(g_hEdit[11], j["connection"].value("mode", "passive").c_str());
        SetWindowTextA(g_hEdit[12], j["connection"].value("external_ip", "").c_str());
    }

    // Load Hangman, RSS, and Release enable states
    std::ifstream hmf("hangman_enabled.txt");
    bool en = true;
    if (hmf.is_open()) { std::string s; std::getline(hmf, s); if (s == "0") en = false; hmf.close(); }
    SendMessage(g_hChkHangman, BM_SETCHECK, en ? BST_CHECKED : BST_UNCHECKED, 0);

    std::ifstream rssf("rss_enabled.txt");
    en = true;
    if (rssf.is_open()) { std::string s; std::getline(rssf, s); if (s == "0") en = false; rssf.close(); }
    SendMessage(g_hChkRss, BM_SETCHECK, en ? BST_CHECKED : BST_UNCHECKED, 0);

    std::ifstream relf("release_enabled.txt");
    en = true;
    if (relf.is_open()) { std::string s; std::getline(relf, s); if (s == "0") en = false; relf.close(); }
    SendMessage(g_hChkRelease, BM_SETCHECK, en ? BST_CHECKED : BST_UNCHECKED, 0);
}

void SaveConfig() {
    char buf[1024];
    json j;
    GetWindowTextA(g_hEdit[0], buf, sizeof(buf)); j["hub_url"] = buf;
    GetWindowTextA(g_hEdit[1], buf, sizeof(buf)); j["bot_nick"] = buf;
    GetWindowTextA(g_hEdit[2], buf, sizeof(buf)); j["bot_password"] = buf;
    GetWindowTextA(g_hEdit[3], buf, sizeof(buf)); j["feeds_file"] = buf;
    GetWindowTextA(g_hEdit[4], buf, sizeof(buf)); j["users_file"] = buf;
    GetWindowTextA(g_hEdit[5], buf, sizeof(buf)); j["cert_file"] = buf;
    GetWindowTextA(g_hEdit[6], buf, sizeof(buf)); j["key_file"] = buf;
    GetWindowTextA(g_hEdit[7], buf, sizeof(buf)); j["refresh_minutes"] = std::stoi(buf);
    
    json ports;
    GetWindowTextA(g_hEdit[8], buf, sizeof(buf)); ports["tcp"] = std::stoi(buf);
    GetWindowTextA(g_hEdit[9], buf, sizeof(buf)); ports["udp"] = std::stoi(buf);
    GetWindowTextA(g_hEdit[10], buf, sizeof(buf)); ports["tls_tcp"] = std::stoi(buf);
    j["ports"] = ports;
    
    json conn;
    GetWindowTextA(g_hEdit[11], buf, sizeof(buf)); conn["mode"] = buf;
    GetWindowTextA(g_hEdit[12], buf, sizeof(buf)); conn["external_ip"] = buf;
    j["connection"] = conn;

    std::ofstream f(g_configPath);
    if (f.is_open()) {
        f << j.dump(4);
        MessageBoxA(g_hWndMain, "config.json saved. Restart bot for changes.", "Success", MB_OK);
    } else {
        MessageBoxA(g_hWndMain, "Failed to save config.json", "Error", MB_ICONERROR);
    }
}

void RefreshFileList() {
    SendMessage(g_hFileList, LB_RESETCONTENT, 0, 0);
    if (!fs::exists("messages")) fs::create_directory("messages");
    for (const auto& entry : fs::directory_iterator("messages")) {
        if (entry.is_regular_file() && entry.path().extension() == ".txt") {
            SendMessageA(g_hFileList, LB_ADDSTRING, 0, (LPARAM)entry.path().filename().string().c_str());
        }
    }
}

// Add File dialog (modal window)
static char g_newFileName[256];
LRESULT CALLBACK InputDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    static HWND hEdit;
    switch (msg) {
        case WM_CREATE:
            CreateWindow("STATIC", "Filename (without .txt):", WS_CHILD | WS_VISIBLE,
                         10, 10, 150, 20, hDlg, NULL, GetModuleHandle(NULL), NULL);
            hEdit = CreateWindowEx(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                   10, 35, 180, 20, hDlg, (HMENU)1001, GetModuleHandle(NULL), NULL);
            CreateWindow("BUTTON", "OK", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                         30, 65, 60, 25, hDlg, (HMENU)IDOK, GetModuleHandle(NULL), NULL);
            CreateWindow("BUTTON", "Cancel", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                         110, 65, 60, 25, hDlg, (HMENU)IDCANCEL, GetModuleHandle(NULL), NULL);
            SetFocus(hEdit);
            return 0;
        case WM_COMMAND:
            if (LOWORD(wParam) == IDOK) {
                GetWindowTextA(hEdit, g_newFileName, sizeof(g_newFileName));
                DestroyWindow(hDlg);
            } else if (LOWORD(wParam) == IDCANCEL) {
                g_newFileName[0] = '\0';
                DestroyWindow(hDlg);
            }
            return 0;
        case WM_CLOSE:
            g_newFileName[0] = '\0';
            DestroyWindow(hDlg);
            return 0;
    }
    return DefWindowProc(hDlg, msg, wParam, lParam);
}

void AddFile() {
    WNDCLASSEX wc = { sizeof(WNDCLASSEX) };
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = InputDlgProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW+1);
    wc.lpszClassName = "TempInputDlg";
    RegisterClassEx(&wc);

    EnableWindow(g_hWndMain, FALSE);
    HWND hDlg = CreateWindowEx(0, "TempInputDlg", "Add text file",
                               WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                               CW_USEDEFAULT, CW_USEDEFAULT, 220, 130,
                               g_hWndMain, NULL, GetModuleHandle(NULL), NULL);
    MSG msg;
    while (IsWindow(hDlg) && GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    EnableWindow(g_hWndMain, TRUE);
    SetForegroundWindow(g_hWndMain);

    if (strlen(g_newFileName) > 0) {
        std::string path = "messages/" + std::string(g_newFileName) + ".txt";
        if (fs::exists(path)) {
            MessageBoxA(g_hWndMain, "File already exists.", "Error", MB_ICONERROR);
        } else {
            std::ofstream f(path);
            f << "# " << g_newFileName << "\n\nEdit this file in Notepad.";
            f.close();
            RefreshFileList();
        }
    }
}

void EditFile() {
    int idx = SendMessage(g_hFileList, LB_GETCURSEL, 0, 0);
    if (idx == LB_ERR) { MessageBoxA(g_hWndMain, "Select a file first.", "Error", MB_ICONERROR); return; }
    char fname[256];
    SendMessageA(g_hFileList, LB_GETTEXT, idx, (LPARAM)fname);
    std::string path = "messages/" + std::string(fname);
    ShellExecuteA(g_hWndMain, "open", "notepad.exe", path.c_str(), NULL, SW_SHOW);
}

void DeleteFile() {
    int idx = SendMessage(g_hFileList, LB_GETCURSEL, 0, 0);
    if (idx == LB_ERR) { MessageBoxA(g_hWndMain, "Select a file first.", "Error", MB_ICONERROR); return; }
    char fname[256];
    SendMessageA(g_hFileList, LB_GETTEXT, idx, (LPARAM)fname);
    std::string path = "messages/" + std::string(fname);
    if (MessageBoxA(g_hWndMain, ("Delete " + std::string(fname) + "?").c_str(), "Confirm", MB_YESNO) == IDYES) {
        fs::remove(path);
        RefreshFileList();
    }
}

// ------------------------------------------------------------------
// Window procedure
// ------------------------------------------------------------------
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            g_hWndMain = hWnd;
            HFONT hFont = CreateFont(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET,
                                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                                     FIXED_PITCH | FF_MODERN, "Consolas");

            CreateWindow("BUTTON", "Bot Configuration", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                         10, 10, 580, 380, hWnd, NULL, GetModuleHandle(NULL), NULL);
            
            const char* labels[] = {
                "Hub URL:", "Bot Nick:", "Bot Password:", "Feeds File:", "Users File:",
                "Cert File:", "Key File:", "Refresh (min):", "TCP Port:", "UDP Port:",
                "TLS TCP Port:", "Conn Mode:", "External IP:"
            };
            int y = 35, x = 20, labelW = 90, editW = 200;
            for (int i = 0; i < 7; i++) {
                CreateWindow("STATIC", labels[i], WS_CHILD | WS_VISIBLE,
                             x, y, labelW, 20, hWnd, NULL, GetModuleHandle(NULL), NULL);
                DWORD style = WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_BORDER;
                if (i == 2) style |= ES_PASSWORD;
                g_hEdit[i] = CreateWindowEx(WS_EX_CLIENTEDGE, "EDIT", "",
                                            style, x + labelW, y, editW, 20,
                                            hWnd, (HMENU)(INT_PTR)(IDC_HUB_URL + i), GetModuleHandle(NULL), NULL);
                SendMessage(g_hEdit[i], WM_SETFONT, (WPARAM)hFont, TRUE);
                y += 28;
            }
            y = 35; x = 320;
            for (int i = 7; i < 13; i++) {
                CreateWindow("STATIC", labels[i], WS_CHILD | WS_VISIBLE,
                             x, y, labelW, 20, hWnd, NULL, GetModuleHandle(NULL), NULL);
                DWORD style = WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_BORDER;
                g_hEdit[i] = CreateWindowEx(WS_EX_CLIENTEDGE, "EDIT", "",
                                            style, x + labelW, y, editW, 20,
                                            hWnd, (HMENU)(INT_PTR)(IDC_HUB_URL + i), GetModuleHandle(NULL), NULL);
                SendMessage(g_hEdit[i], WM_SETFONT, (WPARAM)hFont, TRUE);
                y += 28;
            }

            // Checkboxes – place below config fields
            y += 10;
            g_hChkHangman = CreateWindow("BUTTON", "Enable Hangman", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                         320, y, 150, 20, hWnd, (HMENU)IDC_ENABLE_HANGMAN, GetModuleHandle(NULL), NULL);
            SendMessage(g_hChkHangman, WM_SETFONT, (WPARAM)hFont, TRUE);
            y += 25;
            g_hChkRss = CreateWindow("BUTTON", "Enable RSS", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                     320, y, 150, 20, hWnd, (HMENU)IDC_ENABLE_RSS, GetModuleHandle(NULL), NULL);
            SendMessage(g_hChkRss, WM_SETFONT, (WPARAM)hFont, TRUE);
            y += 25;
            g_hChkRelease = CreateWindow("BUTTON", "Enable Release Management", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                         320, y, 150, 20, hWnd, (HMENU)IDC_ENABLE_RELEASE, GetModuleHandle(NULL), NULL);
            SendMessage(g_hChkRelease, WM_SETFONT, (WPARAM)hFont, TRUE);
            
            CreateWindow("BUTTON", "Text files in messages folder:", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                         600, 10, 280, 280, hWnd, NULL, GetModuleHandle(NULL), NULL);
            g_hFileList = CreateWindowEx(WS_EX_CLIENTEDGE, "LISTBOX", "",
                                         WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY,
                                         610, 35, 260, 180, hWnd, (HMENU)IDC_FILE_LIST, GetModuleHandle(NULL), NULL);
            SendMessage(g_hFileList, WM_SETFONT, (WPARAM)hFont, TRUE);
            
            CreateWindow("BUTTON", "Add File", WS_CHILD | WS_VISIBLE,
                         610, 225, 80, 25, hWnd, (HMENU)IDC_BTN_ADD, GetModuleHandle(NULL), NULL);
            CreateWindow("BUTTON", "Edit File", WS_CHILD | WS_VISIBLE,
                         700, 225, 80, 25, hWnd, (HMENU)IDC_BTN_EDIT, GetModuleHandle(NULL), NULL);
            CreateWindow("BUTTON", "Delete File", WS_CHILD | WS_VISIBLE,
                         790, 225, 80, 25, hWnd, (HMENU)IDC_BTN_DEL, GetModuleHandle(NULL), NULL);
            CreateWindow("BUTTON", "Refresh List", WS_CHILD | WS_VISIBLE,
                         700, 260, 80, 25, hWnd, (HMENU)IDC_BTN_REFRESH, GetModuleHandle(NULL), NULL);
            
            CreateWindow("BUTTON", "Console Output", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                         10, 400, 870, 240, hWnd, NULL, GetModuleHandle(NULL), NULL);
            g_hConsole = CreateWindowEx(WS_EX_CLIENTEDGE, "EDIT", "",
                                        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
                                        20, 420, 850, 190, hWnd, (HMENU)IDC_CONSOLE, GetModuleHandle(NULL), NULL);
            SendMessage(g_hConsole, WM_SETFONT, (WPARAM)hFont, TRUE);
            
            g_hBtnConnect = CreateWindow("BUTTON", "Connect (Launch Bot)", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                         20, 650, 150, 30, hWnd, (HMENU)IDC_BTN_CONNECT, GetModuleHandle(NULL), NULL);
            g_hBtnStop = CreateWindow("BUTTON", "Stop Bot", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                      180, 650, 100, 30, hWnd, (HMENU)IDC_BTN_STOP, GetModuleHandle(NULL), NULL);
            EnableWindow(g_hBtnStop, FALSE);
            
            CreateWindow("BUTTON", "Save Config", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                         300, 650, 100, 30, hWnd, (HMENU)IDC_BTN_SAVE, GetModuleHandle(NULL), NULL);
            
            LoadConfig();
            RefreshFileList();
            break;
        }
        
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == IDC_BTN_CONNECT) StartBot();
            else if (id == IDC_BTN_STOP) StopBot();
            else if (id == IDC_BTN_SAVE) SaveConfig();
            else if (id == IDC_BTN_ADD) AddFile();
            else if (id == IDC_BTN_EDIT) EditFile();
            else if (id == IDC_BTN_DEL) DeleteFile();
            else if (id == IDC_BTN_REFRESH) RefreshFileList();
            else if (id == IDC_ENABLE_HANGMAN) {
                bool en = (SendMessage(g_hChkHangman, BM_GETCHECK, 0, 0) == BST_CHECKED);
                std::ofstream f("hangman_enabled.txt");
                if (f.is_open()) { f << (en ? "1" : "0"); f.close(); }
            }
            else if (id == IDC_ENABLE_RSS) {
                bool en = (SendMessage(g_hChkRss, BM_GETCHECK, 0, 0) == BST_CHECKED);
                std::ofstream f("rss_enabled.txt");
                if (f.is_open()) { f << (en ? "1" : "0"); f.close(); }
            }
            else if (id == IDC_ENABLE_RELEASE) {
                bool en = (SendMessage(g_hChkRelease, BM_GETCHECK, 0, 0) == BST_CHECKED);
                std::ofstream f("release_enabled.txt");
                if (f.is_open()) { f << (en ? "1" : "0"); f.close(); }
            }
            break;
        }
        
        case WM_APPEND_TEXT: {
            std::string* pText = (std::string*)lParam;
            SendMessage(g_hConsole, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
            SendMessage(g_hConsole, EM_REPLACESEL, FALSE, (LPARAM)pText->c_str());
            SendMessage(g_hConsole, EM_SCROLLCARET, 0, 0);
            delete pText;
            break;
        }
        
        case WM_CLOSE: {
            if (g_bBotRunning) StopBot();
            DestroyWindow(hWnd);
            break;
        }
        
        case WM_DESTROY: {
            PostQuitMessage(0);
            break;
        }
        
        default:
            return DefWindowProc(hWnd, msg, wParam, lParam);
    }
    return 0;
}

// ------------------------------------------------------------------
// WinMain
// ------------------------------------------------------------------
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    WNDCLASSEX wc = { sizeof(WNDCLASSEX) };
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW+1);
    wc.lpszClassName = "BotGUI";
    RegisterClassEx(&wc);
    
    HWND hWnd = CreateWindowEx(0, "BotGUI", "FileBot Configuration Editor",
                               WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                               CW_USEDEFAULT, CW_USEDEFAULT, 930, 750,
                               NULL, NULL, hInstance, NULL);
    if (!hWnd) return 1;
    
    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);
    
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return (int)msg.wParam;
}