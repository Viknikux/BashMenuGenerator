// Created with love, and electricity
#include <iostream>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <termios.h>
#include <ctime>
#include <cstdlib>
#include <cctype>
#include <iomanip>

// ============================================================
// Console Color Configuration
// ============================================================
const std::string ANSI_RESET        = "\033[0m";
const std::string ANSI_CYAN         = "\033[1;36m";
const std::string ANSI_YELLOW       = "\033[1;33m";
const std::string ANSI_GREEN        = "\033[1;32m";
const std::string ANSI_RED          = "\033[1;31m";
const std::string ANSI_WHITE        = "\033[1;37m";
const std::string ANSI_MAGENTA      = "\033[1;35m";
const std::string ANSI_DARK_GREEN   = "\033[0;32m";
const std::string ANSI_DARK_RED     = "\033[0;31m";
const std::string ANSI_DARK_YELLOW  = "\033[0;33m";
const std::string ANSI_GRAY         = "\033[1;30m";
const std::string ANSI_LIGHT_GRAY   = "\033[0;37m";
const std::string ANSI_BLUE         = "\033[1;34m";
const std::string ANSI_VIOLET       = "\033[35m";

struct Theme {
    std::string header;
    std::string prompt;
    std::string input;
    std::string success;
    std::string error;
    std::string info;
    std::string banner;
    std::string text;
    std::string accent;
};

static Theme g_theme = { ANSI_CYAN, ANSI_YELLOW, ANSI_WHITE, ANSI_GREEN, ANSI_RED, ANSI_MAGENTA, ANSI_CYAN, ANSI_WHITE, ANSI_YELLOW };

static void SetColor(const std::string& color) {
    std::cout << color;
}

static void ApplyTheme(const std::string& themeName) {
    if (themeName == "matrix") {
        g_theme = { ANSI_DARK_GREEN, ANSI_GREEN, ANSI_GREEN, ANSI_GREEN, ANSI_DARK_GREEN, ANSI_GREEN, ANSI_DARK_GREEN, ANSI_GREEN, ANSI_GREEN };
    } else if (themeName == "cyberpunk") {
        g_theme = { ANSI_CYAN, ANSI_YELLOW, ANSI_MAGENTA, ANSI_GREEN, ANSI_RED, ANSI_MAGENTA, ANSI_CYAN, ANSI_WHITE, ANSI_YELLOW };
    } else if (themeName == "cli") {
        g_theme = { ANSI_LIGHT_GRAY, ANSI_GRAY, ANSI_WHITE, ANSI_WHITE, ANSI_GRAY, ANSI_LIGHT_GRAY, ANSI_GRAY, ANSI_WHITE, ANSI_WHITE };
    } else if (themeName == "violent") {
        g_theme = { ANSI_RED, ANSI_RED, ANSI_MAGENTA, ANSI_GREEN, ANSI_DARK_RED, ANSI_VIOLET, ANSI_DARK_RED, ANSI_WHITE, ANSI_RED };
    } else {
        g_theme = { ANSI_CYAN, ANSI_YELLOW, ANSI_WHITE, ANSI_GREEN, ANSI_RED, ANSI_MAGENTA, ANSI_CYAN, ANSI_WHITE, ANSI_YELLOW };
    }
}

#define C_H SetColor(g_theme.header)
#define C_P SetColor(g_theme.prompt)
#define C_I SetColor(g_theme.input)
#define C_S SetColor(g_theme.success)
#define C_E SetColor(g_theme.error)
#define C_N SetColor(g_theme.info)
#define C_B SetColor(g_theme.banner)
#define C_T SetColor(g_theme.text)
#define C_A SetColor(g_theme.accent)
#define C_R SetColor(ANSI_RESET)

static int getch() {
    struct termios oldt, newt;
    int ch;
    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    ch = getchar();
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    return ch;
}

static std::string ReadLine() {
    std::string line;
    std::getline(std::cin, line);
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
        line.pop_back();
    }
    return line;
}

static std::string Trim(const std::string& str) {
    if (str.empty()) return "";
    size_t first = 0;
    while (first < str.length() && (unsigned char)str[first] <= 32) {
        first++;
    }
    if (first == str.length()) return "";
    size_t last = str.length() - 1;
    while (last > first && (unsigned char)str[last] <= 32) {
        last--;
    }
    return str.substr(first, (last - first + 1));
}

static std::string EscapeBash(const std::string& s) {
    std::string r;
    r.reserve(s.length() * 2);
    for (char ch : s) {
        switch (ch) {
            case '"':  r += "\\\""; break;
            case '\\': r += "\\\\"; break;
            case '$':  r += "\\$";  break;
            case '`':  r += "\\`";  break;
            default:   r += ch;     break;
        }
    }
    return r;
}

struct OptionData {
    std::string label;
    std::vector<std::string> commands; 
    int runMode = 0;
};

// ============================================================
// Settings / bconfig Engine
// ============================================================
struct AppConfig {
    std::string theme = "default";
    int end = 0;        // 0=prompt, 1=main menu, 2=launch script, 3=exit
    int saveload = 0;   // 0=prompt, 1=GUI Dialog, 2=Manual
    int menuborder = 0; // 0=prompt, 1=Classic, 2=Modern, 3=ZigZag
    int bconfig = 1;    // 0=disabled, 1=enabled
};

static AppConfig g_appConfig;

static std::string GetExePath() {
    char buffer[1024];
    ssize_t len = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (len != -1) {
        buffer[len] = '\0';
        std::string path(buffer);
        size_t pos = path.find_last_of('/');
        if (pos != std::string::npos) return path.substr(0, pos + 1);
    }
    return "./";
}

static bool FileExists(const std::string& path) {
    struct stat st;
    return (stat(path.c_str(), &st) == 0 && !S_ISDIR(st.st_mode));
}

static bool DirExists(const std::string& path) {
    struct stat st;
    return (stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode));
}

static bool CreateDirs(const std::string& path) {
    std::string current = "";
    for (char ch : path) {
        current += ch;
        if (ch == '/') {
            if (!current.empty() && !DirExists(current)) {
                if (mkdir(current.c_str(), 0755) != 0) return false;
            }
        }
    }
    if (!current.empty() && !DirExists(current)) {
        if (mkdir(current.c_str(), 0755) != 0) return false;
    }
    return true;
}

static std::string GetSettingsFilePath() {
    std::string dir = GetExePath();
    std::string settingsPath = dir + "settings.txt";
    if (FileExists(settingsPath)) return settingsPath;

    std::string bconfigPath = dir + "bconfig.txt";
    if (FileExists(bconfigPath)) return bconfigPath;

    return settingsPath;
}

static void SaveSettingsFile(const AppConfig& cfg) {
    std::string path = GetSettingsFilePath();
    std::ofstream file(path, std::ios::out | std::ios::trunc);
    if (!file.is_open()) return;
    
    file << "theme: " << cfg.theme << "\n";
    file << "end: " << cfg.end << "\n";
    file << "saveload: " << cfg.saveload << "\n";
    file << "menuborder: " << cfg.menuborder << "\n";
    file << "bconfig: " << cfg.bconfig << "\n";
    file.close();
}

static void LoadSettingsFile(AppConfig& cfg, std::vector<std::string>& warnings) {
    std::string path = GetSettingsFilePath();
    if (!FileExists(path)) {
        SaveSettingsFile(cfg);
        return;
    }

    std::ifstream file(path);
    if (!file.is_open()) return;

    std::string line;
    while (std::getline(file, line)) {
        std::string wline = Trim(line);
        if (wline.empty() || wline.substr(0, 2) == "//") continue;

        size_t colon = wline.find(':');
        if (colon == std::string::npos) continue;

        std::string key = Trim(wline.substr(0, colon));
        std::string val = Trim(wline.substr(colon + 1));

        std::transform(key.begin(), key.end(), key.begin(), ::tolower);

        if (key == "theme") {
            std::string lval = val;
            std::transform(lval.begin(), lval.end(), lval.begin(), ::tolower);
            if (lval == "default" || lval == "matrix" || lval == "cyberpunk" || lval == "cli" || lval == "violent") {
                cfg.theme = lval;
                ApplyTheme(cfg.theme);
            } else {
                warnings.push_back("\"" + val + "\" is not a recognized theme. Valid options are: default, matrix, cyberpunk, cli, violent");
            }
        } else if (key == "end") {
            int v = std::atoi(val.c_str());
            if (v >= 0 && v <= 3) cfg.end = v;
            else warnings.push_back("\"" + val + "\" is not a recognized end option. Valid options are: 0 (Prompt), 1 (Main Menu), 2 (Launch), 3 (Exit)");
        } else if (key == "saveload" || key == "save/load" || key == "load") {
            int v = std::atoi(val.c_str());
            if (v >= 0 && v <= 2) cfg.saveload = v;
            else warnings.push_back("\"" + val + "\" is not a recognized saveload option. Valid options are: 0 (Prompt), 1 (GUI Dialog), 2 (Manual)");
        } else if (key == "menuborder") {
            int v = std::atoi(val.c_str());
            if (v >= 0 && v <= 3) cfg.menuborder = v;
            else warnings.push_back("\"" + val + "\" is not a recognized menuborder option. Valid options are: 0 (Prompt), 1 (Classic), 2 (Modern), 3 (ZigZag)");
        } else if (key == "bconfig") {
            int v = std::atoi(val.c_str());
            if (v == 0 || v == 1) cfg.bconfig = v;
            else warnings.push_back("\"" + val + "\" is not a recognized bconfig option. Valid options are: 0 (Disabled), 1 (Enabled)");
        }
    }
    file.close();
}

static void RunSettingsMenu() {
    while (true) {
        system("clear");
        C_B; std::cout << "  ============================================\n"; C_N;
        std::cout << "           SETTINGS (bconfig)\n"; C_B;
        std::cout << "  ============================================\n\n"; C_T;

        std::string endStr[] = { "0 [Prompt question]", "1 [Main menu]", "2 [Launch script]", "3 [Exit]" };
        std::string saveloadStr[] = { "0 [Prompt question]", "1 [GUI File Dialog]", "2 [Manual path]" };
        std::string borderStr[] = { "0 [Prompt question]", "1 [Classic =]", "2 [Modern -]", "3 [ZigZag Z]" };

        C_P; std::cout << "    1) theme      : "; C_I; std::cout << g_appConfig.theme << "\n";
        C_P; std::cout << "    2) end        : "; C_I; std::cout << endStr[g_appConfig.end] << "\n";
        C_P; std::cout << "    3) saveload   : "; C_I; std::cout << saveloadStr[g_appConfig.saveload] << "\n";
        C_P; std::cout << "    4) menuborder : "; C_I; std::cout << borderStr[g_appConfig.menuborder] << "\n";
        C_P; std::cout << "    5) bconfig    : "; C_I; std::cout << (g_appConfig.bconfig ? "1 [Use settings automatically]" : "0 [Do not use automatically]") << "\n";

        C_P; std::cout << "\n\n    S) Save & Exit\n    0) Cancel\n\n  Select option: "; C_I;
        int ch = getch(); C_T;

        if (ch == '1') {
            const char* themes[] = { "default", "matrix", "cyberpunk", "cli", "violent" };
            for (int i = 0; i < 5; i++) {
                if (g_appConfig.theme == themes[i]) {
                    g_appConfig.theme = themes[(i + 1) % 5];
                    ApplyTheme(g_appConfig.theme);
                    break;
                }
            }
        } else if (ch == '2') {
            g_appConfig.end = (g_appConfig.end + 1) % 4;
        } else if (ch == '3') {
            g_appConfig.saveload = (g_appConfig.saveload + 1) % 3;
        } else if (ch == '4') {
            g_appConfig.menuborder = (g_appConfig.menuborder + 1) % 4;
        } else if (ch == '5') {
            g_appConfig.bconfig = g_appConfig.bconfig ? 0 : 1;
        } else if (ch == 'S' || ch == 's') {
            SaveSettingsFile(g_appConfig);
            C_S; std::cout << "\n\n  [SUCCESS] Settings saved to disk!\n"; C_R;
            sleep(1);
            break;
        } else if (ch == '0' || ch == 27) {
            break;
        }
    }
    system("clear");
}

// ============================================================
// AutoSave
// ============================================================
static std::string GetAppDataDir() {
    const char* home = getenv("HOME");
    std::string dir;
    if (home) {
        dir = std::string(home) + "/.local/share/BashMenuGenerator/";
    } else {
        dir = "/tmp/BashMenuGenerator/";
    }
    CreateDirs(dir);
    return dir;
}

static std::string GetAutoSavePath() {
    return GetAppDataDir() + "autosave.txt";
}

static std::string ReadAutoSaveTimestamp(const std::string& path) {
    if (!FileExists(path)) return "";
    std::ifstream file(path);
    if (!file.is_open()) return "unknown date";
    std::string line;
    if (std::getline(file, line)) {
        size_t pos = line.find("// Autosave from ");
        if (pos != std::string::npos) {
            std::string ts = Trim(line.substr(16));
            file.close();
            return ts;
        }
    }
    file.close();
    return "unknown date";
}

static void WriteAutoSave(const std::vector<std::string>& lines) {
    std::string path = GetAutoSavePath();
    std::ofstream file(path, std::ios::out | std::ios::trunc);
    if (!file.is_open()) return;
    for (const auto& line : lines) {
        file << line << "\n";
    }
    file.close();
}

static std::vector<std::string> ReadAutoSave(const std::string& path) {
    std::vector<std::string> result;
    if (path.empty() || !FileExists(path)) return result;
    std::ifstream file(path);
    if (!file.is_open()) return result;
    std::string line;
    while (std::getline(file, line)) {
        result.push_back(line);
    }
    file.close();
    return result;
}

static void DeleteAutoSave(const std::string& path) {
    if (!path.empty() && FileExists(path)) {
        remove(path.c_str());
    }
}

static std::string MakeBorder(char ch) {
    return std::string(57, ch);
}

// ============================================================
// Generator Engine
// ============================================================
static std::string GenerateBash(const std::string& title,
                                 const std::vector<std::string>& description,
                                 const std::vector<OptionData>& options,
                                 const std::string& password,
                                 bool requireRoot,
                                 char borderChar) {

    std::string border = MakeBorder(borderChar);
    if (border.empty()) border = MakeBorder('=');

    std::string sh;
    sh.reserve(8192);

    sh += "#!/usr/bin/env bash\n";
    sh += "# Created with BashMenuGenerator!\n\n";

    if (requireRoot) {
        sh += "# Root privileges check\n";
        sh += "if [ \"$EUID\" -ne 0 ]; then\n";
        sh += "    echo \"" + border + "\"\n";
        sh += "    echo \"  Requires root privileges!\"\n";
        sh += "    echo \"  Relaunching with sudo...\"\n";
        sh += "    echo \"" + border + "\"\n";
        sh += "    sleep 3\n";
        sh += "    exec sudo \"$0\" \"$@\"\n";
        sh += "    exit 1\n";
        sh += "fi\n\n";
    }

    if (!password.empty()) {
        sh += "auth_gate() {\n";
        sh += "    clear\n";
        sh += "    read -rsp \"Enter password: \" pass_input\n";
        sh += "    echo \"\"\n";
        sh += "    if [ \"$pass_input\" == \"" + EscapeBash(password) + "\" ]; then\n";
        sh += "        return 0\n";
        sh += "    fi\n";
        sh += "    echo \"[!] Incorrect password.\"\n";
        sh += "    read -n 1 -s -r -p \"Press any key to continue . . .\"\n";
        sh += "    echo \"\"\n";
        sh += "    auth_gate\n";
        sh += "}\n";
        sh += "auth_gate\n\n";
    }

    sh += "set_title() {\n";
    sh += "    echo -ne \"\\033]0;$1\\007\"\n";
    sh += "}\n\n";

    sh += "set_title \"" + EscapeBash(title) + "\"\n\n";

    sh += "while true; do\n";
    sh += "    clear\n";
    sh += "    echo \"" + border + "\"\n";

    const int BANNER_WIDTH = 57;
    int titleLen = (int)title.length();
    int padLeft = (BANNER_WIDTH - titleLen) / 2;
    if (padLeft < 0) padLeft = 0;

    std::string spaces(padLeft, ' ');
    sh += "    echo \"" + spaces + EscapeBash(title) + "\"\n";
    sh += "    echo \"" + border + "\"\n";
    sh += "    echo \"\"\n";

    if (!description.empty()) {
        for (const auto& line : description) {
            if (line.empty()) {
                sh += "    echo \"\"\n";
            } else {
                sh += "    echo \"  " + EscapeBash(line) + "\"\n";
            }
        }
        sh += "    echo \"\"\n";
        sh += "    echo \"" + border + "\"\n";
        sh += "    echo \"\"\n";
    }

    for (size_t i = 0; i < options.size(); i++) {
        std::string l = options[i].label.empty() ? "Option" : options[i].label;
        sh += "    echo \"  " + std::to_string(i + 1) + ") " + EscapeBash(l) + "\"\n";
    }
    sh += "    echo \"  X) Exit\"\n";
    sh += "    echo \"\"\n";
    sh += "    echo \"" + border + "\"\n";
    sh += "    echo \"\"\n";

    sh += "    read -rp \"Select Option (1-" + std::to_string(options.size()) + "): \" choice\n\n";

    sh += "    case \"$choice\" in\n";

    for (size_t i = 0; i < options.size(); i++) {
        sh += "        " + std::to_string(i + 1) + ")\n";
        sh += "            clear\n";

        if (options[i].commands.empty()) {
            sh += "            echo \"No command specified.\"\n";
        } else {
            for (size_t j = 0; j < options[i].commands.size(); j++) {
                std::string cmd = options[i].commands[j];
                if (cmd.length() >= 2 && cmd.front() == '"' && cmd.back() == '"')
                    cmd = cmd.substr(1, cmd.length() - 2);

                if (cmd.empty()) continue;

                if (options[i].runMode == 1) {
                    sh += "            bash -c \"" + EscapeBash(cmd) + "\"\n";
                } else {
                    sh += "            " + cmd + "\n";
                }
            }
        }

        sh += "            echo \"\"\n";
        sh += "            read -n 1 -s -r -p \"Press any key to continue . . .\"\n";
        sh += "            echo \"\"\n";
        sh += "            ;;\n";
    }

    sh += "        X|x)\n";
    sh += "            clear\n";
    sh += "            set_title \"bash\"\n";
    sh += "            exit 0\n";
    sh += "            ;;\n";
    sh += "        *)\n";
    sh += "            ;;\n";
    sh += "    esac\n";
    sh += "done\n";

    return sh;
}

static bool WriteScriptToFile(const std::string& path, const std::string& script) {
    std::ofstream file(path, std::ios::out | std::ios::binary);
    if (!file.is_open()) return false;

    file << script;
    file.close();
    chmod(path.c_str(), 0755);
    return true;
}

static std::string GenerateTempFilePath() {
    std::string tempDir = "/tmp/";
    std::time_t t = std::time(nullptr);
    std::tm* st = std::localtime(&t);
    char buf[256];
    snprintf(buf, sizeof(buf), "%sBMGtempScript_%04d%02d%02d_%02d%02d%02d.sh",
        tempDir.c_str(), st->tm_year + 1900, st->tm_mon + 1, st->tm_mday, st->tm_hour, st->tm_min, st->tm_sec);
    return std::string(buf);
}

static void DeleteTempFile(const std::string& path) {
    if (path.empty()) return;
    if (path.find("BMGtempScript_") != std::string::npos && FileExists(path)) {
        remove(path.c_str());
    }
}

static std::string OpenFileDialogLinux(bool saveMode, const std::string& filterExt) {
    std::string cmd;
    if (system("which zenity > /dev/null 2>&1") == 0) {
        cmd = saveMode ? "zenity --file-selection --save --confirm-overwrite --file-filter=*." + filterExt
                       : "zenity --file-selection --file-filter=*." + filterExt;
    } else if (system("which kdialog > /dev/null 2>&1") == 0) {
        cmd = saveMode ? "kdialog --getsavefilename . *." + filterExt
                       : "kdialog --getopenfilename . *." + filterExt;
    } else {
        return "";
    }
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";
    char buffer[1024];
    std::string result = "";
    if (fgets(buffer, sizeof(buffer), pipe) != NULL) {
        result = buffer;
        result = Trim(result);
    }
    pclose(pipe);
    return result;
}

static bool SaveToManualPath(const std::string& script, std::string& outSavedPath, std::string forcedFile = "", std::string forcedDir = "") {
    while (true) {
        std::string filename = forcedFile;
        if (filename.empty()) {
            C_P; std::cout << "\n  Enter filename: "; C_I;
            filename = ReadLine();
            if (filename.length() >= 2 && filename.front() == '"' && filename.back() == '"')
                filename = filename.substr(1, filename.length() - 2);
            filename = Trim(filename);
            if (filename.empty()) { C_E; std::cout << "  [Error] Filename cannot be empty.\n"; C_R; continue; }
            const std::string illegal = "<>:\"/\\|?*";
            bool invalid = false;
            for (char c : filename) { if (illegal.find(c) != std::string::npos) { invalid = true; break; } }
            if (invalid) { C_E; std::cout << "  [Error] Invalid filename.\n"; C_R; continue; }
        }
        if (filename.length() < 3 || filename.substr(filename.length() - 3) != ".sh")
            filename += ".sh";
        std::string dir = forcedDir;
        if (dir.empty()) {
            C_P; std::cout << "  Enter path [Press Enter for Current Directory]: "; C_I;
            dir = ReadLine();
            if (dir.length() >= 2 && dir.front() == '"' && dir.back() == '"') dir = dir.substr(1, dir.length() - 2);
            dir = Trim(dir);
        }
        if (dir.empty()) {
            char buffer[1024];
            if (getcwd(buffer, sizeof(buffer)) != NULL) dir = buffer;
        }
        if (!dir.empty() && dir.back() != '/') dir += "/";
        std::string fullPath = dir + filename;
        std::string baseName = filename.substr(0, filename.length() - 3);
        std::string checkDir = dir;
        if (!checkDir.empty() && checkDir.back() == '/' && checkDir.length() > 1) {
            checkDir.pop_back();
        }
        if (!checkDir.empty() && !DirExists(checkDir)) {
            if (!forcedDir.empty()) {
                CreateDirs(checkDir);
            } else {
                C_E; std::cout << "  [Error] Destination folder does not exist!\n"; C_P;
                std::cout << "    1) Retype file details\n    2) Create folder automatically\n  Select option (1-2) [1]: "; C_I;
                std::string opt = ReadLine();
                if (opt == "2") {
                    if (!CreateDirs(checkDir)) {
                        C_E; std::cout << "  [Error] Failed to create directory.\n"; C_R; continue;
                    }
                } else { continue; }
            }
        }
        if (forcedFile.empty() && FileExists(fullPath)) {
            C_E; std::cout << "  [Warning] File already exists!\n"; C_P;
            std::cout << "    1) Replace\n    2) Rename to " << baseName << "1.sh\n    3) Retype\n  Select (1-3) [1]: "; C_I;
            std::string opt = ReadLine();
            if (opt == "2") fullPath = dir + baseName + "1.sh";
            else if (opt == "3") continue;
        }
        if (WriteScriptToFile(fullPath, script)) {
            C_S; std::cout << "  [SUCCESS] Saved to: " << fullPath << "\n"; C_R;
            outSavedPath = fullPath; return true;
        } else {
            C_E; std::cout << "  [ERROR] Failed to save file.\n"; C_R;
            if (forcedFile.empty()) {
                C_P; std::cout << "    1) Retry\n    2) Use GUI dialog\n  Select (1-2) [2]: "; C_I;
                std::string fc = ReadLine(); if (fc.empty()) fc = "2";
                if (fc == "2") {
                    std::string path = OpenFileDialogLinux(true, "sh");
                    if (!path.empty() && WriteScriptToFile(path, script)) {
                        C_S; std::cout << "  [SUCCESS] Saved to: " << path << "\n"; C_R;
                        outSavedPath = path; return true;
                    }
                    C_E; std::cout << "  [Error] GUI save failed or canceled.\n";
                }
            } else return false;
        }
    }
}

// ============================================================
// TXT config mode
// ============================================================
static bool ParseScriptConfigLines(const std::vector<std::string>& lines, std::string& outTitle, 
    std::vector<std::string>& outDesc, std::string& outPass, std::vector<OptionData>& outOptions,
    std::string& autoFilename, std::string& autoPath, bool& silentPreviewOnly,
    bool& outRequireRoot) {

    int optionCount = 0;
    int currentOption = -1;
    bool collecting = false;
    outRequireRoot = false;

    for (const auto& rawLine : lines) {
        std::string wLine = Trim(rawLine);
        if (wLine.empty() || wLine.substr(0, 2) == "//") continue;

        if (collecting && wLine == "}") { collecting = false; continue; }
        if (collecting && currentOption >= 0 && currentOption < (int)outOptions.size()) {
            if (wLine.size() >= 8 && wLine.substr(0, 8) == "runmode ") {
                int mode = std::atoi(Trim(wLine.substr(8)).c_str());
                outOptions[currentOption].runMode = (mode == 1) ? 1 : 0;
            } else {
                outOptions[currentOption].commands.push_back(wLine);
            }
            continue;
        }

        if (wLine.size() >= 6 && wLine.substr(0, 6) == "title ") outTitle = Trim(wLine.substr(6));
        else if (wLine.size() >= 12 && wLine.substr(0, 12) == "description ") { std::string d = Trim(wLine.substr(12)); if (d != "n" && d != "no") outDesc.push_back(d); }
        else if (wLine.size() >= 6 && (wLine.substr(0, 6) == "admin " || wLine.substr(0, 5) == "root ")) { 
            std::string a = Trim(wLine.substr(wLine.find(' ') + 1)); 
            outRequireRoot = (a == "y" || a == "Y" || a == "yes"); 
        }
        else if (wLine.size() >= 5 && wLine.substr(0, 5) == "pass ") { std::string p = Trim(wLine.substr(5)); if (p == "n" || p == "no" || p.empty()) outPass = ""; else outPass = p; }
        else if (wLine.size() >= 12 && wLine.substr(0, 12) == "option num: ") { optionCount = std::atoi(Trim(wLine.substr(12)).c_str()); if (optionCount < 1) optionCount = 1; if (optionCount > 20) optionCount = 20; outOptions.resize(optionCount); }
        else if (wLine.size() >= 7 && wLine.substr(0, 7) == "option ") {
            size_t lbl = wLine.find(" label "); size_t brc = wLine.find(" {");
            if (lbl != std::string::npos) { int num = std::atoi(wLine.substr(7, lbl - 7).c_str()); if (num >= 1 && num <= optionCount) outOptions[num - 1].label = Trim(wLine.substr(lbl + 7)); }
            else if (brc != std::string::npos) { int num = std::atoi(wLine.substr(7, brc - 7).c_str()); if (num >= 1 && num <= optionCount) { currentOption = num - 1; collecting = true; } }
        }
        else if (wLine.size() >= 8 && wLine.substr(0, 8) == "runmode ") { int mode = std::atoi(Trim(wLine.substr(8)).c_str()); if (currentOption >= 0 && currentOption < (int)outOptions.size()) outOptions[currentOption].runMode = (mode == 1) ? 1 : 0; }
        else if (wLine.size() >= 5 && wLine.substr(0, 5) == "save ") {
            std::string args = Trim(wLine.substr(5));
            if (args == "n") silentPreviewOnly = true;
            else {
                size_t pos1 = args.find("--");
                if (pos1 != std::string::npos) {
                    size_t pos2 = args.find("--", pos1 + 2);
                    if (pos2 != std::string::npos) { autoFilename = Trim(args.substr(pos1 + 2, pos2 - (pos1 + 2))); autoPath = Trim(args.substr(pos2 + 2)); }
                    else autoFilename = Trim(args.substr(pos1 + 2));
                }
            }
        }
    }
    return true;
}

static bool ParseScriptConfig(const std::string& filePath, std::string& outTitle, 
    std::vector<std::string>& outDesc, std::string& outPass, std::vector<OptionData>& outOptions,
    std::string& autoFilename, std::string& autoPath, bool& silentPreviewOnly,
    bool& outRequireRoot) {
    std::ifstream file(filePath);
    if (!file.is_open()) return false;
    std::string line; std::vector<std::string> lines;
    while (std::getline(file, line)) lines.push_back(line);
    file.close();
    return ParseScriptConfigLines(lines, outTitle, outDesc, outPass, outOptions, autoFilename, autoPath, silentPreviewOnly, outRequireRoot);
}

// ============================================================
// Command mode (NEEDS FIX!)
// ============================================================
static bool ConsoleTextEditor(std::vector<std::string>& lines) {
    if (lines.empty()) lines.push_back("");
    int curLine = (int)lines.size() - 1;

    auto Redraw = [&]() {
        std::cout << "\033[2J\033[1;1H";
        C_H; std::cout << " --- Text Mode (ESC for menu, Up/Down to navigate) ---\n\n";
        for (size_t i = 0; i < lines.size(); i++) {
            if ((int)i == curLine) {
                C_A; std::cout << " > " << lines[i] << "_\n";
            } else {
                C_T; std::cout << "   " << lines[i] << "\n";
            }
        }
        C_R;
    };

    Redraw();
    while (true) {
        int c = getch();
        if (c == 27) {
            struct termios oldt, newt;
            tcgetattr(STDIN_FILENO, &oldt);
            newt = oldt;
            newt.c_cc[VMIN] = 0;
            newt.c_cc[VTIME] = 1;
            tcsetattr(STDIN_FILENO, TCSANOW, &newt);
            int c2 = getchar();
            int c3 = (c2 == '[') ? getchar() : 0;
            tcsetattr(STDIN_FILENO, TCSANOW, &oldt);

            if (c2 == '[' && c3 == 'A') {
                if (curLine > 0) { curLine--; Redraw(); }
            } else if (c2 == '[' && c3 == 'B') {
                if (curLine < (int)lines.size() - 1) { curLine++; Redraw(); }
            } else {
                C_N; std::cout << "\n\n [ESC Menu] 1: Continue | 2: Compile (finish) | 3: Exit Program: "; C_R;
                while (true) {
                    int opt = getch();
                    if (opt == '1') { Redraw(); break; }
                    if (opt == '2') return true;
                    if (opt == '3') exit(0);
                }
            }
        } else if (c == 10 || c == 13) {
            if (curLine == (int)lines.size() - 1) lines.push_back("");
            else lines.insert(lines.begin() + curLine + 1, "");
            curLine++;
            Redraw();
        } else if (c == 127 || c == 8) {
            if (!lines[curLine].empty()) {
                lines[curLine].pop_back();
                Redraw();
            } else if (curLine > 0) {
                lines.erase(lines.begin() + curLine);
                curLine--;
                Redraw();
            }
        } else if (c >= 32 && c <= 126) {
            lines[curLine].push_back((char)c);
            Redraw();
        }
    }
    return false;
}

// ============================================================
// Debug Menu & Version menu
// ============================================================
static bool g_enableTheme = false;
static bool g_enableAutoSave = true; 
static bool g_enableBConfig = true; 
static bool g_enableCommandMode = false;
static bool g_debugMode = false;

static void RenderVersionLine(const std::string& line) {
    std::string baseColor = g_theme.text;
    std::string workLine = line;

    if (workLine.rfind("[cyan] ", 0) == 0 || workLine.rfind("[c] ", 0) == 0) {
        baseColor = g_theme.header;
        workLine = workLine.substr(workLine.find(' ') + 1);
    } else if (workLine.rfind("[yellow] ", 0) == 0 || workLine.rfind("[y] ", 0) == 0) {
        baseColor = g_theme.prompt;
        workLine = workLine.substr(workLine.find(' ') + 1);
    } else if (workLine.rfind("[green] ", 0) == 0 || workLine.rfind("[g] ", 0) == 0) {
        baseColor = g_theme.success;
        workLine = workLine.substr(workLine.find(' ') + 1);
    } else if (workLine.rfind("[red] ", 0) == 0 || workLine.rfind("[r] ", 0) == 0) {
        baseColor = g_theme.error;
        workLine = workLine.substr(workLine.find(' ') + 1);
    } else if (workLine.rfind("[magenta] ", 0) == 0 || workLine.rfind("[m] ", 0) == 0) {
        baseColor = g_theme.info;
        workLine = workLine.substr(workLine.find(' ') + 1);
    } else if (workLine.rfind("[white] ", 0) == 0 || workLine.rfind("[w] ", 0) == 0) {
        baseColor = g_theme.text;
        workLine = workLine.substr(workLine.find(' ') + 1);
    }

    SetColor(baseColor);

    size_t i = 0;
    std::string currentColor = baseColor;

    while (i < workLine.length()) {
        if (workLine[i] == '{') {
            size_t closeBrace = workLine.find('}', i);
            if (closeBrace != std::string::npos) {
                std::string tag = workLine.substr(i + 1, closeBrace - i - 1);
                std::transform(tag.begin(), tag.end(), tag.begin(), ::tolower);

                bool isTag = false;
                std::string newColor = currentColor;

                if (tag == "cyan" || tag == "c" || tag == "header") { newColor = g_theme.header; isTag = true; }
                else if (tag == "yellow" || tag == "y" || tag == "prompt") { newColor = g_theme.prompt; isTag = true; }
                else if (tag == "green" || tag == "g" || tag == "success") { newColor = g_theme.success; isTag = true; }
                else if (tag == "red" || tag == "r" || tag == "error") { newColor = g_theme.error; isTag = true; }
                else if (tag == "white" || tag == "w" || tag == "text") { newColor = g_theme.text; isTag = true; }
                else if (tag == "magenta" || tag == "m" || tag == "info") { newColor = g_theme.info; isTag = true; }
                else if (tag == "/" || tag == "reset") { newColor = baseColor; isTag = true; }

                if (isTag) {
                    currentColor = newColor;
                    SetColor(currentColor);
                    i = closeBrace + 1;
                    continue;
                }
            }
        }
        std::cout << workLine[i];
        i++;
    }
    std::cout << "\n";
    C_R;
}

static void RunVersionHistoryMenu() {
    system("clear");
    C_H; std::cout << "BashMenuGenerator v1.4.3\n\n"; C_R;

    std::string verPath = GetExePath() + "bmgver.txt";
    if (!FileExists(verPath)) verPath = GetExePath() + "bashmgver.txt";

    std::ifstream file(verPath);
    if (file.is_open()) {
        std::string line;
        while (std::getline(file, line)) {
            std::string trimmed = Trim(line);
            if (trimmed.length() >= 2 && trimmed.substr(0, 2) == "//") {
                continue;
            }
            RenderVersionLine(line);
        }
        file.close();
    } else {
        C_E; std::cout << "BMG version data is deleted, corrupted or missing because of portable version\n"; C_R;
    }
    std::cout << "\nPress Enter to return...";
    ReadLine();
}

// ============================================================
// AutoSave Resume
// ============================================================
static bool RunMode1Flow(std::string& title, std::vector<std::string>& description,
    std::string& password, std::vector<OptionData>& options, bool& requireRoot,
    char& borderChar, const std::string& resumeStep = "") {
    
    int resumeStage = 0;
    size_t resumeOptIndex = 0;

    if (resumeStep == "TITLE_DONE") {
        resumeStage = 1;
    } else if (resumeStep == "DESC_LINE" || resumeStep == "DESC_DONE") {
        resumeStage = 2;
    } else if (resumeStep == "PASS_DONE") {
        resumeStage = 3;
    } else if (resumeStep == "ADMIN_DONE" || resumeStep == "ROOT_DONE") {
        resumeStage = 4;
    } else if (resumeStep == "OPTCOUNT_DONE") {
        resumeStage = 5;
        resumeOptIndex = 0;
    } else if (resumeStep.find("OPT_") == 0) {
        resumeStage = 5;
        if (resumeStep.find("OPT_DONE_") == 0) {
            int idx = std::atoi(resumeStep.substr(9).c_str());
            resumeOptIndex = (idx >= 0) ? (size_t)(idx + 1) : 0;
        } else if (resumeStep.find("OPT_LABEL_") == 0) {
            int idx = std::atoi(resumeStep.substr(10).c_str());
            resumeOptIndex = (idx >= 0) ? (size_t)idx : 0;
        } else if (resumeStep.find("OPT_RUNMODE_") == 0) {
            int idx = std::atoi(resumeStep.substr(12).c_str());
            resumeOptIndex = (idx >= 0) ? (size_t)idx : 0;
        } else if (resumeStep.find("OPT_CMD_") == 0) {
            int idx = std::atoi(resumeStep.substr(8).c_str());
            resumeOptIndex = (idx >= 0) ? (size_t)idx : 0;
        }
    } else if (resumeStep == "ALL_OPTIONS_DONE") {
        resumeStage = 6;
    }
    
    std::string sessionTimestamp;
    { 
        std::time_t t = std::time(nullptr);
        std::tm* st = std::localtime(&t);
        char buf[64];
        snprintf(buf, sizeof(buf), "%02d:%02d %02d/%02d/%02d", st->tm_hour, st->tm_min, st->tm_mday, st->tm_mon + 1, (st->tm_year + 1900) % 100);
        sessionTimestamp = buf;
    }
    
    auto SaveStep = [&](const std::string& stepCode) {
        if (!g_enableAutoSave) return;
        std::vector<std::string> lines;
        lines.push_back("// Autosave from " + sessionTimestamp);
        lines.push_back("step " + stepCode);
        lines.push_back("title " + title);
        for (const auto& d : description) lines.push_back("description " + d);
        if (!password.empty()) lines.push_back("pass " + password);
        lines.push_back(requireRoot ? "admin y" : "admin n");
        lines.push_back("option num: " + std::to_string(options.size()));
        for (size_t oi = 0; oi < options.size(); oi++) {
            std::string optNum = std::to_string(oi + 1);
            if (!options[oi].label.empty()) {
                lines.push_back("option " + optNum + " label " + options[oi].label);
            }
            lines.push_back("option " + optNum + " {");
            lines.push_back("runmode " + std::to_string(options[oi].runMode));
            for (const auto& c : options[oi].commands) {
                lines.push_back(c);
            }
            lines.push_back("}");
        }
        WriteAutoSave(lines);
    };

    if (resumeStage < 1) {
        while (true) {
            C_P; std::cout << "  Enter window title (Max 57 chars): "; C_I;
            title = ReadLine(); C_R;
            if (title.empty()) { title = "Bash Menu"; break; }
            if (title.length() <= 57) break;
            else { C_E; std::cout << "  Title invalid: cannot be longer than 57 characters (including spaces)!\n"; C_R; }
        }
        SaveStep("TITLE_DONE");
    }

    if (resumeStage < 2) {
        C_P; std::cout << "  Do you want to add a description? (Y/n): "; C_I;
        std::string descAns = ReadLine(); C_R;
        if (descAns.empty() || descAns == "Y" || descAns == "y" || descAns == "yes") {
            C_P; std::cout << "  Enter description lines below.\n  Press Enter on an empty line when done.\n"; C_S; std::cout << "  Description:\n"; C_R;
            while (true) { C_I; std::cout << "    > "; std::string descLine = ReadLine(); C_R; if (descLine.empty()) break; description.push_back(descLine); SaveStep("DESC_LINE"); }
        }
        SaveStep("DESC_DONE");
    }

    if (resumeStage < 3) {
        C_P; std::cout << "\n  [Note] Password protecting a bash script is plain-text visible unless permissions are restricted.\n";
        std::cout << "  Enter password (leave empty for none): "; C_I;
        password = ReadLine(); C_R;
        SaveStep("PASS_DONE");
    }

    if (resumeStage < 4) {
        C_P; std::cout << "\n  Will this script require Root privileges? (y/N): "; C_I;
        std::string rootAns = ReadLine(); C_R;
        requireRoot = (!rootAns.empty() && (rootAns == "y" || rootAns == "Y" || rootAns == "yes"));
        if (requireRoot) { C_S; std::cout << "  [Root elevation code (sudo check) will be added to the script]\n"; C_R; }
        SaveStep("ROOT_DONE");
    }

    if (resumeStage < 5) {
        C_P; std::cout << "  Number of options (1-20) [3]: "; C_I;
        std::string numStr = ReadLine(); C_R;
        int optionCount = 3;
        if (!numStr.empty()) { int val = std::atoi(numStr.c_str()); if (val >= 1 && val <= 20) optionCount = val; }
        options.resize(optionCount);
        SaveStep("OPTCOUNT_DONE");
    }

    for (size_t i = resumeOptIndex; i < options.size(); i++) {
        C_N; std::cout << "\n  --- Option " << (i + 1) << " ---\n"; C_R;
        
        if (!options[i].label.empty()) {
            C_P; std::cout << "  Label [Option " << (i + 1) << "] [" << options[i].label << "]: "; C_I;
            std::string label = ReadLine(); C_R;
            if (!label.empty()) options[i].label = label;
        } else {
            C_P; std::cout << "  Label [Option " << (i + 1) << "]: "; C_I;
            std::string label = ReadLine(); C_R;
            options[i].label = label.empty() ? "Option" : label;
        }
        SaveStep("OPT_LABEL_" + std::to_string(i));

        C_P; std::cout << "  Run mode:\n    0 = Direct call\n    1 = bash -c wrapper\n  Select (0/1) [" << options[i].runMode << "]: "; C_I;
        std::string modeStr = ReadLine(); C_R;
        if (!modeStr.empty()) {
            options[i].runMode = (modeStr == "1") ? 1 : 0;
        }
        SaveStep("OPT_RUNMODE_" + std::to_string(i));

        if (!options[i].commands.empty()) {
            C_S; std::cout << "  Existing commands for Option " << (i + 1) << ":\n";
            for (const auto& c : options[i].commands) {
                std::cout << "    > " << c << "\n";
            }
            C_P; std::cout << "  Add more command(s), one per line (Press Enter on empty line when done):\n"; C_R;
        } else {
            C_P; std::cout << "  Enter command(s), one per line.\n  Press Enter on an empty line when done.\n"; C_S; std::cout << "  Commands:\n"; C_R;
        }
        
        while (true) {
            C_I; std::cout << "    > "; std::string cmd = ReadLine(); C_R;
            if (cmd.empty()) break;
            options[i].commands.push_back(cmd);
            SaveStep("OPT_CMD_" + std::to_string(i));
        }
        SaveStep("OPT_DONE_" + std::to_string(i));
    }
    SaveStep("ALL_OPTIONS_DONE");
    return true;
}

// ============================================================
// Tester Menu & Debug Mode
// ============================================================
struct TesterAction {
    char key;
    const char* label;
    void (*action)();
};

static void TaThemeEngine() {
    C_S; std::cout << "\n  [TEST] Toggling Theme Engine...\n"; C_R;
    g_enableTheme = !g_enableTheme;
    C_S; std::cout << "  Theme Engine is now: " << (g_enableTheme ? "ENABLED" : "disabled") << "\n"; C_R;
    std::cout << "Press Enter to continue..."; ReadLine();
}

static void TaAutoSave() {
    C_S; std::cout << "\n  [TEST] Toggling AutoSave System...\n"; C_R;
    g_enableAutoSave = !g_enableAutoSave;
    C_S; std::cout << "  AutoSave is now: " << (g_enableAutoSave ? "ENABLED" : "disabled") << "\n"; C_R;
    std::cout << "Press Enter to continue..."; ReadLine();
}

static void TaBConfig() {
    C_S; std::cout << "\n  [TEST] Toggling Settings (bconfig)...\n"; C_R;
    g_enableBConfig = !g_enableBConfig;
    if (g_enableBConfig) {
        std::vector<std::string> warnings;
        LoadSettingsFile(g_appConfig, warnings);
    }
    C_S; std::cout << "  Settings (bconfig) is now: " << (g_enableBConfig ? "ENABLED" : "disabled") << "\n"; C_R;
    std::cout << "Press Enter to continue..."; ReadLine();
}

static void TaCommandMode() {
    C_S; std::cout << "\n  [TEST] Toggling Command Mode...\n"; C_R;
    g_enableCommandMode = !g_enableCommandMode;
    C_S; std::cout << "  Command Mode is now: " << (g_enableCommandMode ? "ENABLED" : "disabled") << "\n"; C_R;
    std::cout << "Press Enter to continue..."; ReadLine();
}

static void TaOpenSettings() {
    C_S; std::cout << "\n  [TEST] Opening Settings Menu...\n"; C_R;
    RunSettingsMenu();
    C_S; std::cout << "\n  [TEST] Settings Menu closed.\n"; C_R;
    std::cout << "Press Enter to continue..."; ReadLine();
}

static void TaToggleAllOn() {
    C_S; std::cout << "\n  [TEST] Toggling ALL features ON...\n"; C_R;
    g_enableTheme = g_enableAutoSave = g_enableBConfig = g_enableCommandMode = true;
    C_S; std::cout << "  All features now: ENABLED\n"; C_R;
    std::cout << "Press Enter to continue..."; ReadLine();
}

static void TaToggleAllOff() {
    C_S; std::cout << "\n  [TEST] Toggling ALL features OFF...\n"; C_R;
    g_enableTheme = g_enableAutoSave = g_enableBConfig = g_enableCommandMode = false;
    C_S; std::cout << "  All features now: DISABLED\n"; C_R;
    std::cout << "Press Enter to continue..."; ReadLine();
}

static void TaGenBatchAll() {
    C_S; std::cout << "\n  [TEST] Generating bash script with all features...\n"; C_R;
    char bc = '=';
    std::string title = "Test Menu";
    std::vector<std::string> desc = { "Test description line 1", "Test description line 2" };
    std::vector<OptionData> opts(3);
    opts[0].label = "Run Echo Test";
    opts[0].commands.push_back("echo 'Hello World'");
    opts[1].label = "Show Directory Contents";
    opts[1].commands.push_back("ls -la");
    opts[2].label = "Show IP Info";
    opts[2].commands.push_back("ip a");
    std::string script = GenerateBash(title, desc, opts, "test123", true, bc);
    C_B; std::cout << "\n  ========== GENERATED SCRIPT ==========\n"; C_R;
    std::cout << script << "\n";
    std::cout << "Press Enter to continue..."; ReadLine();
}

static void TaGenBatchAdmin() {
    C_S; std::cout << "\n  [TEST] Generating bash script with root check only...\n"; C_R;
    char bc = '=';
    std::string title = "Root Test";
    std::vector<OptionData> opts(2);
    opts[0].label = "Run System Update";
    opts[0].commands.push_back("apt update");
    opts[1].label = "Exit";
    opts[1].commands.push_back("exit");
    std::string script = GenerateBash(title, {}, opts, "", true, bc);
    C_B; std::cout << "\n  ========== GENERATED SCRIPT ==========\n"; C_R;
    std::cout << script << "\n";
    std::cout << "Press Enter to continue..."; ReadLine();
}

static void TaGenBatchPassword() {
    C_S; std::cout << "\n  [TEST] Generating bash script with password only...\n"; C_R;
    char bc = '-';
    std::string title = "Password Protected";
    std::vector<OptionData> opts(1);
    opts[0].label = "Protected Action";
    opts[0].commands.push_back("echo 'Access Granted'");
    std::string script = GenerateBash(title, {}, opts, "secret123", false, bc);
    C_B; std::cout << "\n  ========== GENERATED SCRIPT ==========\n"; C_R;
    std::cout << script << "\n";
    std::cout << "Press Enter to continue..."; ReadLine();
}

static void TaGenBatchBorder() {
    C_S; std::cout << "\n  [TEST] Generating bash script with custom zigzag border...\n"; C_R;
    char bc = 'Z';
    std::string title = "Zigzag Border Test";
    std::vector<OptionData> opts(1);
    opts[0].label = "Run Test";
    opts[0].commands.push_back("uname -a");
    std::string script = GenerateBash(title, {}, opts, "", false, bc);
    C_B; std::cout << "\n  ========== GENERATED SCRIPT ==========\n"; C_R;
    std::cout << script << "\n";
    std::cout << "Press Enter to continue..."; ReadLine();
}

static void TaSaveToManualPath() {
    C_S; std::cout << "\n  [TEST] SaveToManualPath test...\n"; C_R;
    std::string script = "#!/usr/bin/env bash\necho 'Hello from BashMenuGenerator!'\nread -n 1 -s -r -p 'Press any key to continue . . .'\n";
    std::string savedPath;
    SaveToManualPath(script, savedPath, "BMG_TestScript", GetExePath());
    std::cout << "Press Enter to continue..."; ReadLine();
}

static void TaParseConfig() {
    C_S; std::cout << "\n  [TEST] ParseScriptConfig test...\n"; C_R;
    std::vector<std::string> testLines = {
        "title Test Config",
        "description This is a test config",
        "admin y",
        "pass mypassword",
        "option num: 2",
        "option 1 label Echo Test",
        "option 1 {",
        "echo 'Test 1'",
        "}",
        "option 2 label List Files",
        "option 2 {",
        "ls -la",
        "}"
    };
    std::string title;
    std::vector<std::string> desc;
    std::string pass;
    std::vector<OptionData> opts;
    std::string autoFn, autoPath;
    bool silent = false, root = false;
    if (ParseScriptConfigLines(testLines, title, desc, pass, opts, autoFn, autoPath, silent, root)) {
        C_S; std::cout << "  [SUCCESS] Parsed config!\n"; C_R;
        C_P; std::cout << "  Title: "; C_I; std::cout << title << "\n"; C_R;
        C_P; std::cout << "  Password: "; C_I; std::cout << pass << "\n"; C_R;
        C_P; std::cout << "  Require Root: "; C_I; std::cout << (root ? "yes" : "no") << "\n"; C_R;
        for (size_t i = 0; i < opts.size(); i++) {
            C_P; std::cout << "  Option " << (i + 1) << ": "; C_I; std::cout << opts[i].label << "\n"; C_R;
            C_P; std::cout << "    RunMode: "; C_I; std::cout << opts[i].runMode << "\n"; C_R;
            for (const auto& cmd : opts[i].commands) {
                C_P; std::cout << "    Cmd: "; C_I; std::cout << cmd << "\n"; C_R;
            }
        }
    } else {
        C_E; std::cout << "  [ERROR] Failed to parse config.\n"; C_R;
    }
    std::cout << "Press Enter to continue..."; ReadLine();
}

static void TaVersionHistory() {
    C_S; std::cout << "\n  [TEST] Opening Version History...\n"; C_R;
    RunVersionHistoryMenu();
}

static void TaThemeChange() {
    C_S; std::cout << "\n  [TEST] Cycling theme...\n"; C_R;
    const char* themes[] = { "default", "matrix", "cyberpunk", "cli", "violent" };
    for (int i = 0; i < 5; i++) {
        ApplyTheme(themes[i]);
        C_P; std::cout << "  Theme: "; C_H; std::cout << themes[i] << "\n"; C_R;
        usleep(300000);
    }
    ApplyTheme(g_appConfig.theme);
    C_S; std::cout << "  Theme restored to: " << g_appConfig.theme << "\n"; C_R;
    std::cout << "Press Enter to continue..."; ReadLine();
}

static void TaRunMode1Flow() {
    C_S; std::cout << "\n  [TEST] Running Mode 1 flow (with autosave steps)...\n"; C_R;
    std::string title = "Auto Test";
    std::vector<std::string> desc;
    std::string pass;
    std::vector<OptionData> opts;
    bool root = false;
    char bc = '=';
    bool completed = RunMode1Flow(title, desc, pass, opts, root, bc);
    C_S; std::cout << "\n  [TEST] Flow completed: " << (completed ? "yes" : "no") << "\n"; C_R;
    C_P; std::cout << "  Title: "; C_I; std::cout << title << "\n"; C_R;
    C_P; std::cout << "  Options: "; C_I; std::cout << opts.size() << "\n"; C_R;
    std::cout << "Press Enter to continue..."; ReadLine();
}

static const TesterAction g_testerActions[] = {
    { '1', "Generate bash script with ALL features", TaGenBatchAll },
    { '2', "Generate bash script with ROOT check only", TaGenBatchAdmin },
    { '3', "Generate bash script with PASSWORD only", TaGenBatchPassword },
    { '4', "Generate bash script with CUSTOM BORDER (ZigZag)", TaGenBatchBorder },
    { '5', "Run SaveToManualPath test", TaSaveToManualPath },
    { '6', "Run ParseScriptConfig test", TaParseConfig },
    { '7', "Run Mode 1 Flow (autosave steps)", TaRunMode1Flow },
    { '8', "Toggle Theme Engine", TaThemeEngine },
    { '9', "Toggle AutoSave System", TaAutoSave },
    { 'A', "Toggle Settings (bconfig)", TaBConfig },
    { 'B', "Toggle Command Mode", TaCommandMode },
    { 'C', "Toggle ALL features ON", TaToggleAllOn },
    { 'D', "Toggle ALL features OFF", TaToggleAllOff },
    { 'E', "View Version History", TaVersionHistory },
    { 'F', "Cycle through Themes", TaThemeChange },
    { 'G', "Open Settings Menu", TaOpenSettings },
};

static void RunTesterMenu() {
    while (true) {
        system("clear");
        C_B; std::cout << "  ============================================\n"; C_N;
        std::cout << "           TESTER MENU - PROGRAM ACTIONS\n"; C_B;
        std::cout << "  ============================================\n\n"; C_T;
        std::cout << "  Select an action to test/launch:\n\n";
        for (const auto& a : g_testerActions) {
            C_P; std::cout << "    " << a.key << ") ";
            if (a.key >= '0' && a.key <= '9') C_I; else C_A;
            std::cout << a.label << "\n";
        }
        C_T; std::cout << "\n    Q) Return to Debug Menu\n\n";
        C_P; std::cout << "  Select option: "; C_I;
        int tc = getch(); C_T;
        if (tc == 'Q' || tc == 'q' || tc == 27) break;
        bool found = false;
        for (const auto& a : g_testerActions) {
            if (tc == a.key) {
                found = true;
                a.action();
                break;
            }
        }
        if (!found) {
            C_E; std::cout << "\n  [Error] Invalid option.\n"; C_R;
            usleep(800000);
        }
    }
    C_R; system("clear");
}

static void RunDebugMenu() {
    system("clear");
    C_B; std::cout << "  ============================================\n"; C_N;
    std::cout << "           DEBUG MODE - TESTING MENU\n"; C_B;
    std::cout << "  ============================================\n\n"; C_T;
    std::cout << "  Feature Toggles:\n\n";
    C_P; std::cout << "    1) Theme Engine                       : "; C_I; std::cout << (g_enableTheme ? "ENABLED" : "disabled") << "\n"; C_T;
    C_P; std::cout << "    2) AutoSave System                     : "; C_I; std::cout << (g_enableAutoSave ? "ENABLED" : "disabled") << "\n"; C_T;
    C_P; std::cout << "    3) Settings (formerly known as bconfig): "; C_I; std::cout << (g_enableBConfig ? "ENABLED" : "disabled") << "\n"; C_T;
    C_P; std::cout << "    4) Command Mode                        : "; C_I; std::cout << (g_enableCommandMode ? "ENABLED" : "disabled") << "\n"; C_T;
    C_P; std::cout << "    5) Open Settings Menu\n";
    C_P; std::cout << "    6) Toggle All On\n    7) Toggle All Off\n    8) Launch Tester\n";
    C_P; std::cout << "    V) Version History\n\n"; C_T;
    C_P; std::cout << "  Select option (0 to exit): "; C_I;
    int choice = getch(); C_T;
    if (choice == '1') { g_enableTheme = !g_enableTheme; RunDebugMenu(); return; }
    if (choice == '2') { g_enableAutoSave = !g_enableAutoSave; RunDebugMenu(); return; }
    if (choice == '3') {
        g_enableBConfig = !g_enableBConfig;
        if (g_enableBConfig) {
            std::vector<std::string> warnings;
            LoadSettingsFile(g_appConfig, warnings);
        }
        RunDebugMenu(); return;
    }
    if (choice == '4') { g_enableCommandMode = !g_enableCommandMode; RunDebugMenu(); return; }
    if (choice == '5') { RunSettingsMenu(); RunDebugMenu(); return; }
    if (choice == '6') { g_enableTheme = g_enableAutoSave = g_enableBConfig = g_enableCommandMode = true; RunDebugMenu(); return; }
    if (choice == '7') { g_enableTheme = g_enableAutoSave = g_enableBConfig = g_enableCommandMode = false; RunDebugMenu(); return; }
    if (choice == '8') { RunTesterMenu(); return; }
    if (choice == 'V' || choice == 'v') { RunVersionHistoryMenu(); RunDebugMenu(); return; }
    C_R; system("clear");
}

// ============================================================
// Main Entry
// ============================================================
int main() {
    std::cout << "\033]0;Bash Menu Generator\007";

    std::vector<std::string> configWarnings;
    if (g_enableBConfig) {
        LoadSettingsFile(g_appConfig, configWarnings);
    }

    std::string autoSavePath = GetAutoSavePath();
    std::string autoSaveTimestamp = ReadAutoSaveTimestamp(autoSavePath);
    std::vector<std::string> autosaveData = ReadAutoSave(autoSavePath);
    bool hasAutosave = !autosaveData.empty();

    while (true) {
        if (!g_debugMode) system("clear");

        C_B; std::cout << "\n  ============================================\n"; C_B;
        std::cout << "            Bash Menu Generator\n";
        if (g_debugMode) { C_E; std::cout << "                ** DEBUG MODE **\n"; }
        C_B; std::cout << "  ============================================\n\n"; C_R;

        if (!configWarnings.empty()) {
            C_E; std::cout << "  [Settings Warnings]\n";
            for (const auto& warn : configWarnings) {
                std::cout << "   * " << warn << "\n";
            }
            std::cout << "\n"; C_R;
            configWarnings.clear();
        }

        if (hasAutosave) {
            C_N; std::cout << "  [Autosave found from " << autoSaveTimestamp << "]\n";
            std::cout << "  Would you like to continue generating the bash script? (Y/n): "; C_I;
            std::string asAns = ReadLine(); C_R;
            if (asAns.empty() || asAns == "Y" || asAns == "y" || asAns == "yes") {
                std::string savedTitle;
                std::vector<std::string> savedDesc;
                std::string savedPass;
                std::vector<OptionData> savedOptions;
                std::string savedAutoFilename, savedAutoPath;
                bool savedSilentPreview = false, savedRequireRoot = false;
                std::string savedStep = "";
                
                for (const auto& line : autosaveData) {
                    std::string wl = Trim(line);
                    if (wl.substr(0, 5) == "step ") savedStep = Trim(wl.substr(5));
                }
                
                if (ParseScriptConfigLines(autosaveData, savedTitle, savedDesc, savedPass, savedOptions, savedAutoFilename, savedAutoPath, savedSilentPreview, savedRequireRoot)) {
                    C_S; std::cout << "  [Autosave restored - resuming at step: " << savedStep << "]\n"; C_R;
                    sleep(1);
                    
                    char bc = '=';
                    bool completed = RunMode1Flow(savedTitle, savedDesc, savedPass, savedOptions, savedRequireRoot, bc, savedStep);
                    DeleteAutoSave(autoSavePath);
                    hasAutosave = false;
                    
                    if (completed) {
                        std::string script = GenerateBash(savedTitle, savedDesc, savedOptions, savedPass, savedRequireRoot, bc);
                        C_B; std::cout << "\n  ========== PREVIEW ==========\n"; C_R;
                        std::cout << script << "\n";
                        
                        std::string finalSavedPath = "", tempFilePath = "";
                        C_P; std::cout << "  Save to file? (Y/n): "; C_I;
                        std::string saveAns = ReadLine(); C_R;
                        if (saveAns.empty() || saveAns == "Y" || saveAns == "y" || saveAns == "yes") {
                            std::string methodChoice;
                            if (g_enableBConfig && g_appConfig.bconfig == 1 && g_appConfig.saveload > 0) {
                                methodChoice = (g_appConfig.saveload == 1) ? "2" : "1";
                            } else {
                                while (true) {
                                    C_P; std::cout << "  Choose Save Method:\n    1 = Type path manually\n    2 = Browse (GUI File Dialog)\n  Select (1/2) [2]: "; C_I;
                                    methodChoice = ReadLine(); C_R;
                                    if (methodChoice.empty()) methodChoice = "2";
                                    if (methodChoice == "1" || methodChoice == "2") break;
                                    C_E; std::cout << "  Invalid option choice.\n"; C_R;
                                }
                            }
                            if (methodChoice == "1") SaveToManualPath(script, finalSavedPath);
                            else {
                                std::string path = OpenFileDialogLinux(true, "sh");
                                if (!path.empty()) {
                                    if (!WriteScriptToFile(path, script)) { C_E; std::cout << "\n  [ERROR] GUI save failed.\n"; C_R; SaveToManualPath(script, finalSavedPath); }
                                    else { C_S; std::cout << "\n  [SUCCESS] Saved to: " << path << "\n"; C_R; finalSavedPath = path; }
                                } else { C_P; std::cout << "\n  Dialog canceled. Defaulting to manual path entry...\n"; C_R; SaveToManualPath(script, finalSavedPath); }
                            }
                        }
                        
                        while (true) {
                            C_P; std::cout << "\n  ============================================\n";
                            std::cout << "  What would you like to do next?\n    1) Main menu\n    2) Launch\n    3) Exit\n  Select [3]: "; C_I;
                            std::string endChoice = ReadLine(); C_R;
                            if (endChoice == "3" || endChoice.empty()) { DeleteTempFile(tempFilePath); break; }
                            else if (endChoice == "2") {
                                if (finalSavedPath.empty()) {
                                    tempFilePath = GenerateTempFilePath();
                                    if (!WriteScriptToFile(tempFilePath, script)) { C_E; std::cout << "  [Error] Could not create temp file!\n"; C_R; continue; }
                                    finalSavedPath = tempFilePath;
                                }
                                system(("bash \"" + finalSavedPath + "\"").c_str());
                                sleep(1); DeleteTempFile(tempFilePath); tempFilePath = "";
                            } else break;
                        }
                    }
                    system("clear"); continue;
                } else { C_E; std::cout << "  [Autosave data corrupted, starting fresh]\n"; C_R; }
            }
            DeleteAutoSave(autoSavePath);
            hasAutosave = false;
            system("clear"); continue;
        }

        C_P; std::cout << "  Select Mode:\n    1 = Make new Bash Menu\n    2 = Load TXT Config\n";
        if (g_enableCommandMode) std::cout << "    3 = Command Mode\n";
        if (g_enableTheme) std::cout << "    T = Change Theme\n";
        
        if (g_enableBConfig) {
            std::cout << "    S = Settings (bconfig)\n";
        }
        
        std::cout << "  Choice (1-3) [1]: "; C_I;
        std::string workflowChoice = ReadLine(); C_R;
        if (workflowChoice.empty()) workflowChoice = "1";

        if (workflowChoice == "S" || workflowChoice == "s") {
            if (g_enableBConfig) {
                RunSettingsMenu();
                continue;
            }
        }

        if (workflowChoice == "3" && !g_enableCommandMode) {
            C_E; std::cout << "\n  Command Mode is disabled. Enable it in Debug Menu (Enter code 2211 on main menu).\n"; C_R;
            std::cout << "Press Enter to continue..."; ReadLine(); system("clear"); continue;
        }

        if (g_enableTheme && (workflowChoice == "T" || workflowChoice == "t")) {
            system("clear");
            C_B; std::cout << "  ============================================\n"; C_N;
            std::cout << "           CHANGE THEME\n"; C_B;
            std::cout << "  ============================================\n\n"; C_T;
            const char* themes[] = { "default", "matrix", "cyberpunk", "cli", "violent" };
            for (int i = 0; i < 5; i++) { ApplyTheme(themes[i]); C_P; std::cout << "    " << (i + 1) << ") "; C_H; std::cout << "[" << themes[i] << "]\n"; }
            ApplyTheme(g_appConfig.theme);
            C_P; std::cout << "\n  Select theme (1-5, 0 to cancel): "; C_I;
            int thc = getch();
            if (thc >= '1' && thc <= '5') { int idx = thc - '1'; const char* chosen[] = { "default", "matrix", "cyberpunk", "cli", "violent" }; ApplyTheme(chosen[idx]); g_appConfig.theme = chosen[idx]; C_S; std::cout << "\n  Theme set to: " << chosen[idx] << "\n"; C_R; sleep(1); }
            C_R; system("clear"); continue;
        }

        if (workflowChoice == "2211") { g_debugMode = !g_debugMode; if (g_debugMode) RunDebugMenu(); continue; }

        std::string title;
        std::vector<std::string> description;
        std::string password = "";
        std::vector<OptionData> options;
        std::string autoFilename = "", autoPath = "";
        bool silentPreviewOnly = false, requireRoot = false;
        std::string finalSavedPath = "", tempFilePath = "";
        char borderChar = '=';

        if (workflowChoice == "2") {
            bool loaded = false; std::string loadChoice = "";
            
            if (g_enableBConfig && g_appConfig.bconfig == 1 && g_appConfig.saveload > 0) {
                loadChoice = (g_appConfig.saveload == 1) ? "1" : "2";
            }

            while (!loaded) {
                if (loadChoice.empty()) {
                    C_P; std::cout << "\n  Choose Load Method:\n    1 = Browse (GUI File Dialog)\n    2 = Type path manually\n  Select (1/2) [1]: "; C_I;
                    loadChoice = ReadLine(); C_R; if (loadChoice.empty()) loadChoice = "1";
                }
                if (loadChoice != "1" && loadChoice != "2") { C_E; std::cout << "  Invalid option choice.\n"; C_R; loadChoice = ""; continue; }
                std::string configPath = "";
                if (loadChoice == "1") {
                    configPath = OpenFileDialogLinux(false, "txt");
                    if (configPath.empty()) { C_P; std::cout << "\n  Dialog canceled. Defaulting to manual path entry...\n"; C_R; loadChoice = "2"; continue; }
                } else { C_P; std::cout << "\n  Enter config file path: "; C_I; configPath = ReadLine(); C_R; }
                if (configPath.length() >= 2 && configPath.front() == '"' && configPath.back() == '"')
                    configPath = configPath.substr(1, configPath.length() - 2);
                if (configPath.empty() || !ParseScriptConfig(configPath, title, description, password, options, autoFilename, autoPath, silentPreviewOnly, requireRoot)) {
                    C_E; std::cout << "  [Error] Failed to load config file.\n"; C_P;
                    std::cout << "    1) Try again\n    2) Browse\n    3) Cancel\n  Select (1-3) [1]: "; C_I; C_R;
                    std::string retryAns = ReadLine();
                    if (retryAns == "2") loadChoice = "1"; else if (retryAns == "3") break; else loadChoice = "2";
                } else loaded = true;
            }
            if (!loaded) { system("clear"); continue; }
        } else if (workflowChoice == "3") {
            std::vector<std::string> editorLines;
            bool compile = ConsoleTextEditor(editorLines);
            if (!compile) { system("clear"); continue; }
            if (!ParseScriptConfigLines(editorLines, title, description, password, options, autoFilename, autoPath, silentPreviewOnly, requireRoot)) {
                C_E; std::cout << "  [Error] Failed to process editor text!\n"; C_R;
                std::cout << "Press Enter to continue..."; ReadLine(); system("clear"); continue;
            }
        } else {
            if (!RunMode1Flow(title, description, password, options, requireRoot, borderChar)) {
                system("clear"); continue;
            }
            DeleteAutoSave(autoSavePath);
        }

        if (g_enableBConfig && g_appConfig.bconfig == 1 && g_appConfig.menuborder > 0) {
            if (g_appConfig.menuborder == 1) borderChar = '=';
            else if (g_appConfig.menuborder == 2) borderChar = '-';
            else if (g_appConfig.menuborder == 3) borderChar = 'Z';
        } else {
            C_P; std::cout << "\n  Select border style:\n    1) Classic  :  =========================================================\n";
            std::cout << "    2) Modern   :  ---------------------------------------------------------\n";
            std::cout << "    3) Zigzag   :  ZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZ\n";
            std::cout << "    4) Custom   :  (Enter any character you want)\n  Select (1-4) [1]: "; C_I;
            std::string borderChoice = ReadLine(); C_R;
            if (borderChoice == "2") borderChar = '-';
            else if (borderChoice == "3") borderChar = 'Z';
            else if (borderChoice == "4") { C_P; std::cout << "  Enter border character: "; C_I; std::string custBorder = ReadLine(); C_R; if (!custBorder.empty()) borderChar = custBorder[0]; }
        }

        C_H; std::cout << "\n  Generating bash script...\n"; C_R;
        std::string script = GenerateBash(title, description, options, password, requireRoot, borderChar);
        C_B; std::cout << "\n  ========== PREVIEW ==========\n"; C_R;
        std::cout << script << "\n";

        if ((workflowChoice == "2" || workflowChoice == "3") && silentPreviewOnly) {
            C_P; std::cout << "\n  Not saved! Exiting preview mode.\n"; C_R;
        } else if ((workflowChoice == "2" || workflowChoice == "3") && !autoFilename.empty()) {
            SaveToManualPath(script, finalSavedPath, autoFilename, autoPath);
        } else {
            C_P; std::cout << "  Save to file? (Y/n): "; C_I;
            std::string saveAns = ReadLine(); C_R;
            if (saveAns.empty() || saveAns == "Y" || saveAns == "y" || saveAns == "yes") {
                std::string methodChoice;
                if (g_enableBConfig && g_appConfig.bconfig == 1 && g_appConfig.saveload > 0) {
                    methodChoice = (g_appConfig.saveload == 1) ? "2" : "1";
                } else {
                    while (true) {
                        C_P; std::cout << "  Choose Save Method:\n    1 = Type path manually\n    2 = Browse (GUI File Dialog)\n  Select (1/2) [2]: "; C_I;
                        methodChoice = ReadLine(); C_R;
                        if (methodChoice.empty()) methodChoice = "2";
                        if (methodChoice == "1" || methodChoice == "2") break;
                        C_E; std::cout << "  Invalid option choice.\n"; C_R;
                    }
                }
                if (methodChoice == "1") SaveToManualPath(script, finalSavedPath);
                else {
                    std::string path = OpenFileDialogLinux(true, "sh");
                    if (!path.empty()) {
                        if (!WriteScriptToFile(path, script)) { C_E; std::cout << "\n  [ERROR] GUI save failed.\n"; C_R; SaveToManualPath(script, finalSavedPath); }
                        else { C_S; std::cout << "\n  [SUCCESS] Saved to: " << path << "\n"; C_R; finalSavedPath = path; }
                    } else { C_P; std::cout << "\n  Dialog canceled. Defaulting to manual path entry...\n"; C_R; SaveToManualPath(script, finalSavedPath); }
                }
            } else { C_P; std::cout << "\n  Not saved!\n"; C_R; }
        }

        std::string endChoice;
        if (g_enableBConfig && g_appConfig.bconfig == 1 && g_appConfig.end > 0) {
            endChoice = std::to_string(g_appConfig.end);
        } else {
            C_P; std::cout << "\n  ============================================\n";
            std::cout << "  What would you like to do next?\n    1) Go back to main menu\n    2) Launch script\n    3) Exit\n  Select [3]: "; C_I;
            endChoice = ReadLine(); C_R;
        }

        if (endChoice == "3" || endChoice.empty()) { DeleteTempFile(tempFilePath); break; }
        else if (endChoice == "2") {
            if (finalSavedPath.empty()) {
                tempFilePath = GenerateTempFilePath();
                if (!WriteScriptToFile(tempFilePath, script)) { C_E; std::cout << "  [Error] Could not create temp file!\n"; C_R; std::cout << "Press Enter to continue..."; ReadLine(); continue; }
                finalSavedPath = tempFilePath;
                C_S; std::cout << "\n  [Temp file saved to: " << tempFilePath << "]\n"; C_R;
            }
            system(("bash \"" + finalSavedPath + "\"").c_str());
            C_P; std::cout << "\n  Cleaning up temp file...\n"; C_R; sleep(1); DeleteTempFile(tempFilePath); tempFilePath = "";
        }
        system("clear");
    }
    return 0;
}
