#include <iostream>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Console Color Configuration (ANSI Escape Codes for Linux Terminals)
// ---------------------------------------------------------------------------
const std::string CC_RESET   = "\033[0m";
const std::string CC_CYAN    = "\033[1;36m";
const std::string CC_YELLOW  = "\033[1;33m";
const std::string CC_GREEN   = "\033[1;32m";
const std::string CC_RED     = "\033[1;31m";
const std::string CC_WHITE   = "\033[1;37m";
const std::string CC_MAGENTA = "\033[1;35m";

static void SetConsoleColor(const std::string& color) {
    std::cout << color;
}

static std::string ReadLine() {
    std::string line;
    std::getline(std::cin, line);
    return line;
}

// ---------------------------------------------------------------------------
// Bash Escape Helper
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// Option Data Structure
// ---------------------------------------------------------------------------
struct OptionData {
    std::string label;
    std::vector<std::string> commands; 
    int runMode; // 0 = Direct Call, 1 = bash -c Wrapper
};

// ---------------------------------------------------------------------------
// Generate the Bash Script (.sh)
// ---------------------------------------------------------------------------
static std::string GenerateBash(const std::string& title,
                                 const std::vector<std::string>& description,
                                 const std::vector<OptionData>& options) {

    std::string sh;
    sh.reserve(8192);

    sh += "#!/usr/bin/env bash\n";
    sh += "# Created with BashMenuGenerator!\n\n";

    sh += "# Function to set window title\n";
    sh += "set_title() {\n";
    sh += "    echo -ne \"\\033]0;$1\\007\"\n";
    sh += "}\n\n";

    sh += "set_title \"" + EscapeBash(title) + "\"\n\n";

    sh += "while true; do\n";
    sh += "    clear\n";
    sh += "    echo \"=========================================================\"\n";

    const int BANNER_WIDTH = 57;
    int titleLen = (int)title.length();
    int padLeft = (BANNER_WIDTH - titleLen) / 2;
    if (padLeft < 0) padLeft = 0;

    std::string spaces(padLeft, ' ');
    sh += "    echo \"" + spaces + EscapeBash(title) + "\"\n";
    sh += "    echo \"=========================================================\"\n";
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
        sh += "    echo \"=========================================================\"\n";
        sh += "    echo \"\"\n";
    }

    for (size_t i = 0; i < options.size(); i++) {
        std::string l = options[i].label.empty() ? "Option" : options[i].label;
        sh += "    echo \"  " + std::to_string(i + 1) + ") " + EscapeBash(l) + "\"\n";
    }
    sh += "    echo \"  X) Exit\"\n";
    sh += "    echo \"\"\n";
    sh += "    echo \"=========================================================\"\n";
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

// ---------------------------------------------------------------------------
// POSIX File & Directory Helpers
// ---------------------------------------------------------------------------
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

static bool WriteScriptToFile(const std::string& path, const std::string& script) {
    std::ofstream file(path, std::ios::out | std::ios::binary);
    if (!file.is_open()) return false;

    file << script;
    file.close();

    // Automatically make generated script executable (+x) on Linux
    chmod(path.c_str(), 0755);
    return true;
}

static bool SaveToManualPath(const std::string& script) {
    while (true) {
        SetConsoleColor(CC_YELLOW);
        std::cout << "\n  Enter filename: ";
        SetConsoleColor(CC_RESET);
        std::string filename = ReadLine();
        if (filename.empty()) {
            SetConsoleColor(CC_RED);
            std::cout << "  [Error] Filename cannot be empty.\n";
            SetConsoleColor(CC_RESET);
            continue;
        }

        if (filename.length() < 3 || filename.substr(filename.length() - 3) != ".sh") {
            filename += ".sh";
        }

        SetConsoleColor(CC_YELLOW);
        std::cout << "  Enter path [Press Enter for Current Directory]: ";
        SetConsoleColor(CC_RESET);
        std::string dir = ReadLine();

        if (!dir.empty() && dir.back() != '/') {
            dir += "/";
        }

        std::string fullPath = dir + filename;
        std::string baseName = filename.substr(0, filename.length() - 3);

        std::string checkDir = dir;
        if (!checkDir.empty() && checkDir.back() == '/' && checkDir.length() > 1) {
            checkDir.pop_back(); 
        }

        if (!checkDir.empty() && !DirExists(checkDir)) {
            SetConsoleColor(CC_RED);
            std::cout << "  [Error] Destination folder does not exist!\n";
            SetConsoleColor(CC_YELLOW);
            std::cout << "    1) Retype file details\n";
            std::cout << "    2) Create folder automatically\n";
            std::cout << "  Select option (1-2) [1]: ";
            SetConsoleColor(CC_RESET);
            std::string opt = ReadLine();
            
            if (opt == "2") {
                if (!CreateDirs(checkDir)) {
                    SetConsoleColor(CC_RED);
                    std::cout << "  [Error] Failed to create directory (lack of permissions).\n";
                    SetConsoleColor(CC_RESET);
                    continue;
                }
            } else {
                continue;
            }
        }

        if (FileExists(fullPath)) {
            SetConsoleColor(CC_RED);
            std::cout << "  [Warning] A file with the same name already exists!\n";
            SetConsoleColor(CC_YELLOW);
            std::cout << "    1) Replace file\n";
            std::cout << "    2) Rename file to " << baseName << "1.sh\n";
            std::cout << "    3) Retype file details\n";
            std::cout << "  Select option (1-3) [1]: ";
            SetConsoleColor(CC_RESET);
            std::string opt = ReadLine();
            
            if (opt == "2") {
                fullPath = dir + baseName + "1.sh";
            } else if (opt == "3") {
                continue;
            }
        }

        if (WriteScriptToFile(fullPath, script)) {
            SetConsoleColor(CC_GREEN);
            std::cout << "  [SUCCESS] Saved and made executable at: " << fullPath << "\n";
            SetConsoleColor(CC_RESET);
            return true;
        } else {
            SetConsoleColor(CC_RED);
            std::cout << "  [ERROR] Write operations restricted at this path.\n";
            SetConsoleColor(CC_RESET);
        }
    }
}

// ---------------------------------------------------------------------------
// Main Flow
// ---------------------------------------------------------------------------
int main() {
    SetConsoleColor(CC_CYAN);
    std::cout << "\n";
    std::cout << "  ============================================\n";
    std::cout << "        Bash Menu Generator - CLI Edition\n";
    std::cout << "  ============================================\n\n";
    SetConsoleColor(CC_RESET);

    std::string title;
    while (true) {
        SetConsoleColor(CC_YELLOW);
        std::cout << "  Enter window title (Max 57 chars): ";
        SetConsoleColor(CC_RESET);
        title = ReadLine();
        if (title.empty()) {
            title = "Bash Menu";
            break;
        }
        if (title.length() <= 57) {
            break;
        } else {
            SetConsoleColor(CC_RED);
            std::cout << "  Title invalid: cannot be longer than 57 characters!\n";
            SetConsoleColor(CC_RESET);
        }
    }

    std::vector<std::string> description;
    SetConsoleColor(CC_YELLOW);
    std::cout << "  Do you want to add a description? (Y/n): ";
    SetConsoleColor(CC_RESET);
    std::string descAns = ReadLine();

    if (descAns.empty() || descAns == "Y" || descAns == "y" || descAns == "yes") {
        SetConsoleColor(CC_YELLOW);
        std::cout << "  Enter description lines below.\n";
        std::cout << "  Press Enter on an empty line when done.\n";
        SetConsoleColor(CC_GREEN);
        std::cout << "  Description:\n";
        SetConsoleColor(CC_RESET);

        while (true) {
            std::cout << "    > ";
            std::string descLine = ReadLine();
            if (descLine.empty()) break;
            description.push_back(descLine);
        }
    }

    SetConsoleColor(CC_YELLOW);
    std::cout << "  Number of options (1-20) [3]: ";
    SetConsoleColor(CC_RESET);
    std::string numStr = ReadLine();
    int optionCount = 3;
    if (!numStr.empty()) {
        try {
            int val = std::stoi(numStr);
            if (val >= 1 && val <= 20) optionCount = val;
        } catch (...) {}
    }

    std::vector<OptionData> options(optionCount);

    for (int i = 0; i < optionCount; i++) {
        SetConsoleColor(CC_MAGENTA);
        std::cout << "\n  --- Option " << (i + 1) << " ---\n";
        SetConsoleColor(CC_RESET);

        SetConsoleColor(CC_YELLOW);
        std::cout << "  Label [Option " << (i + 1) << "]: ";
        SetConsoleColor(CC_RESET);
        std::string label = ReadLine();
        options[i].label = label.empty() ? "Option" : label;

        SetConsoleColor(CC_YELLOW);
        std::cout << "  Run mode:\n";
        std::cout << "    0 = Direct call\n";
        std::cout << "    1 = bash -c wrapper\n";
        std::cout << "  Select (0/1) [0]: ";
        SetConsoleColor(CC_RESET);
        std::string modeStr = ReadLine();
        options[i].runMode = (modeStr == "1") ? 1 : 0;

        SetConsoleColor(CC_YELLOW);
        std::cout << "  Enter command(s), one per line.\n";
        std::cout << "  Press Enter on an empty line when done.\n";
        SetConsoleColor(CC_GREEN);
        std::cout << "  Commands:\n";
        SetConsoleColor(CC_RESET);

        while (true) {
            std::cout << "    > ";
            std::string cmd = ReadLine();
            if (cmd.empty()) break;
            options[i].commands.push_back(cmd);
        }

        if (options[i].commands.empty()) {
            SetConsoleColor(CC_RED);
            std::cout << "  [No commands entered for this option!]\n";
            SetConsoleColor(CC_RESET);
        } else {
            SetConsoleColor(CC_GREEN);
            std::cout << "  [" << options[i].commands.size() << " command(s) recorded]\n";
            SetConsoleColor(CC_RESET);
        }
    }

    SetConsoleColor(CC_CYAN);
    std::cout << "\n  Generating bash script...\n";
    SetConsoleColor(CC_RESET);

    std::string script = GenerateBash(title, description, options);

    SetConsoleColor(CC_WHITE);
    std::cout << "\n  ========== PREVIEW ==========\n";
    SetConsoleColor(CC_RESET);
    std::cout << script << "\n";

    SetConsoleColor(CC_YELLOW);
    std::cout << "  Save to file? (Y/n): ";
    SetConsoleColor(CC_RESET);
    std::string saveAns = ReadLine();

    if (saveAns.empty() || saveAns == "Y" || saveAns == "y" || saveAns == "yes") {
        SaveToManualPath(script);
    } else {
        SetConsoleColor(CC_YELLOW);
        std::cout << "\n  Not saved! Exiting.\n";
        SetConsoleColor(CC_RESET);
    }

    SetConsoleColor(CC_CYAN);
    std::cout << "\n  Press Enter to exit...";
    SetConsoleColor(CC_RESET);
    ReadLine();
    return 0;
}