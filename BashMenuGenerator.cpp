//Created with God, love and electricity, BMG 1.5.0 Release-Stable
// BashMenuGenerator - the Linux / POSIX edition of BatchMenuGenerator.
//
// Build:   bash build.sh                  (stripped, fully static binary; see build.sh for the options)
//     or:  g++ -std=c++14 -O2 -o BashMenuGenerator BashMenuGenerator.cpp
// Run:     ./BashMenuGenerator            (interactive)
//          ./BashMenuGenerator -h         (command-line switches)
//
// Needs only a POSIX system and a terminal. Optional helpers that are used when present:
//   zenity / kdialog / yad / qarma (graphical file browsers), xclip / xsel / wl-clipboard (clipboard),
//   a C compiler (cc, gcc, clang, tcc) for "Compile to binary", sudo or doas inside root scripts.

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <climits>
#include <csignal>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

// ============================================================
// UTF-8 helpers
// ============================================================
// Text is kept as UTF-8 std::string everywhere. Bytes that are not valid UTF-8 are carried through
// the editor as code points U+DC80..U+DCFF and written back as the original byte, so opening and
// saving a file never corrupts it.
static void AppendUtf8(std::string& out, char32_t cp) {
    if (cp >= 0xDC80 && cp <= 0xDCFF) { out.push_back((char)(cp - 0xDC00)); return; }
    if (cp < 0x80) {
        out.push_back((char)cp);
    } else if (cp < 0x800) {
        out.push_back((char)(0xC0 | (cp >> 6)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back((char)(0xE0 | (cp >> 12)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else {
        out.push_back((char)(0xF0 | (cp >> 18)));
        out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    }
}

static std::u32string Utf8ToU32(const std::string& s) {
    std::u32string out;
    out.reserve(s.size());
    size_t i = 0;
    const size_t n = s.size();
    while (i < n) {
        const unsigned char c = (unsigned char)s[i];
        if (c < 0x80) { out.push_back(c); ++i; continue; }
        size_t len = 0;
        char32_t cp = 0;
        if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; }
        bool ok = (len != 0 && i + len <= n);
        if (ok) {
            for (size_t k = 1; k < len; ++k) {
                const unsigned char cc = (unsigned char)s[i + k];
                if ((cc & 0xC0) != 0x80) { ok = false; break; }
                cp = (cp << 6) | (cc & 0x3F);
            }
        }
        if (ok) {
            if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) ||
                cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) ok = false;
        }
        if (ok) { out.push_back(cp); i += len; }
        else { out.push_back((char32_t)(0xDC00 + c)); ++i; }
    }
    return out;
}

static std::string U32ToUtf8(const std::u32string& s) {
    std::string out;
    out.reserve(s.size());
    for (char32_t c : s) AppendUtf8(out, c);
    return out;
}

// Columns a code point occupies in a terminal. Control characters are drawn as a one-column dot.
static int CharWidth(char32_t c) {
    if (c == 0) return 0;
    if (c < 0x20 || (c >= 0x7F && c < 0xA0)) return 1;
    if (c >= 0xDC80 && c <= 0xDCFF) return 1;
    if ((c >= 0x0300 && c <= 0x036F) || (c >= 0x1AB0 && c <= 0x1AFF) || (c >= 0x1DC0 && c <= 0x1DFF) ||
        (c >= 0x20D0 && c <= 0x20FF) || (c >= 0xFE00 && c <= 0xFE0F) || (c >= 0xFE20 && c <= 0xFE2F) ||
        c == 0x200B || c == 0x200C || c == 0x200D || c == 0xFEFF) return 0;
    if ((c >= 0x1100 && c <= 0x115F) || (c >= 0x2E80 && c <= 0x303E) || (c >= 0x3041 && c <= 0x33FF) ||
        (c >= 0x3400 && c <= 0x4DBF) || (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0xA000 && c <= 0xA4CF) ||
        (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFE30 && c <= 0xFE6F) ||
        (c >= 0xFF00 && c <= 0xFF60) || (c >= 0xFFE0 && c <= 0xFFE6) || (c >= 0x1F300 && c <= 0x1F64F) ||
        (c >= 0x1F900 && c <= 0x1F9FF) || (c >= 0x20000 && c <= 0x3FFFD)) return 2;
    return 1;
}

// What is actually drawn for a code point (control characters and stray bytes become a dot).
static char32_t DisplayChar(char32_t c) {
    if (c < 0x20 || (c >= 0x7F && c < 0xA0) || (c >= 0xDC80 && c <= 0xDCFF)) return 0x00B7;
    return c;
}

static int DisplayWidth(const std::string& s) {
    int w = 0;
    for (char32_t c : Utf8ToU32(s)) w += CharWidth(c);
    return w;
}

// First code point of a string as UTF-8 (empty string stays empty).
static std::string FirstChar(const std::string& s) {
    std::u32string u = Utf8ToU32(s);
    if (u.empty()) return "";
    std::string out;
    AppendUtf8(out, u[0]);
    return out;
}

// Cuts a string so it fits in 'cols' terminal columns.
static std::string FitToWidth(const std::string& s, int cols) {
    if (cols <= 0) return "";
    std::string out;
    int w = 0;
    for (char32_t c : Utf8ToU32(s)) {
        const char32_t shown = DisplayChar(c);
        const int cw = CharWidth(shown);
        if (w + cw > cols) break;
        AppendUtf8(out, shown);
        w += cw;
    }
    return out;
}

// ============================================================
// String helpers
// ============================================================
static std::string Trim(const std::string& str) {
    size_t first = 0;
    while (first < str.size() && (unsigned char)str[first] <= 32) ++first;
    if (first == str.size()) return "";
    size_t last = str.size() - 1;
    while (last > first && (unsigned char)str[last] <= 32) --last;
    return str.substr(first, last - first + 1);
}

static bool StartsWith(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

static bool EndsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

static std::string ToLower(std::string s) {
    for (auto& ch : s) ch = (char)std::tolower((unsigned char)ch);
    return s;
}

static bool EndsWithNoCase(const std::string& s, const std::string& suffix) {
    return EndsWith(ToLower(s), ToLower(suffix));
}

static bool IsYes(const std::string& raw) {
    const std::string v = ToLower(Trim(raw));
    return v == "y" || v == "yes" || v == "true" || v == "on" || v == "1";
}

static std::string ReplaceAll(std::string s, const std::string& from, const std::string& to) {
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

static std::vector<std::string> SplitLines(const std::string& text) {
    std::vector<std::string> lines;
    size_t start = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '\r' && text[i] != '\n') continue;
        lines.push_back(text.substr(start, i - start));
        if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
        start = i + 1;
    }
    if (start < text.size()) lines.push_back(text.substr(start));
    else if (!text.empty() && (text.back() == '\r' || text.back() == '\n')) lines.push_back("");
    return lines;
}

// Wraps a string in single quotes for sh/bash. A single quote inside becomes '\''.
static std::string ShQuote(const std::string& s) {
    std::string r = "'";
    for (char ch : s) {
        if (ch == '\'') r += "'\\''";
        else r += ch;
    }
    r += "'";
    return r;
}

// Inverse of ShQuote for one quoted word starting at 'pos' (which must point at the opening quote).
// Understands the '\'' idiom. On success 'pos' is moved just past the quoted word.
static bool ShUnquoteAt(const std::string& s, size_t& pos, std::string& out) {
    out.clear();
    size_t i = pos;
    bool any = false;
    while (i < s.size()) {
        if (s[i] == '\'') {
            size_t close = s.find('\'', i + 1);
            if (close == std::string::npos) return false;
            out.append(s, i + 1, close - i - 1);
            i = close + 1;
            any = true;
        } else if (s.compare(i, 2, "\\'") == 0 && any) {
            out += '\'';
            i += 2;
        } else {
            break;
        }
    }
    if (!any) return false;
    pos = i;
    return true;
}

static std::string ToStr(long long v) { return std::to_string(v); }

static std::string HexOf(const unsigned char* data, size_t len) {
    static const char* d = "0123456789abcdef";
    std::string r;
    r.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) { r += d[data[i] >> 4]; r += d[data[i] & 15]; }
    return r;
}

// ============================================================
// File, path and environment helpers
// ============================================================
static bool PathExists(const std::string& path) {
    struct stat st;
    return !path.empty() && stat(path.c_str(), &st) == 0;
}

static bool DirExists(const std::string& path) {
    struct stat st;
    return !path.empty() && stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

static bool IsRegularFile(const std::string& path) {
    struct stat st;
    return !path.empty() && stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

static bool IsSymlink(const std::string& path) {
    struct stat st;
    return !path.empty() && lstat(path.c_str(), &st) == 0 && S_ISLNK(st.st_mode);
}

static std::string GetEnv(const char* name) {
    const char* v = getenv(name);
    return v ? std::string(v) : std::string();
}

// The current user's home folder from /etc/passwd. Read by hand instead of calling getpwuid() so that a
// statically linked build does not depend on the C library's name-service plug-ins at run time.
static std::string HomeFromPasswdFile() {
    FILE* f = fopen("/etc/passwd", "r");
    if (!f) return "";
    const std::string uid = std::to_string((long long)getuid());
    std::string home;
    char buf[2048];
    while (home.empty() && fgets(buf, sizeof(buf), f)) {
        std::string line = buf;                           // name:password:uid:gid:gecos:home:shell
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        std::vector<std::string> field;
        size_t start = 0;
        for (size_t i = 0; i <= line.size(); ++i) {
            if (i == line.size() || line[i] == ':') { field.push_back(line.substr(start, i - start)); start = i + 1; }
        }
        if (field.size() >= 6 && field[2] == uid && !field[5].empty() && field[5][0] == '/') home = field[5];
    }
    fclose(f);
    return home;
}

static std::string HomeDir() {
    std::string h = GetEnv("HOME");
    if (!h.empty()) return h;
    h = HomeFromPasswdFile();
    return h.empty() ? "/tmp" : h;
}

static std::string CurrentDir() {
    char buf[PATH_MAX];
    if (getcwd(buf, sizeof(buf))) return buf;
    return ".";
}

static std::string EnsureTrailingSlash(std::string p) {
    if (p.empty() || p.back() != '/') p += '/';
    return p;
}

static std::string TrimTrailingSlashes(std::string p) {
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    return p;
}

static std::string DirName(const std::string& path) {
    const std::string p = TrimTrailingSlashes(path);
    size_t pos = p.find_last_of('/');
    if (pos == std::string::npos) return ".";
    if (pos == 0) return "/";
    return p.substr(0, pos);
}

static std::string BaseName(const std::string& path) {
    const std::string p = TrimTrailingSlashes(path);
    size_t pos = p.find_last_of('/');
    return pos == std::string::npos ? p : p.substr(pos + 1);
}

// File name without its last extension ("menu.sh" -> "menu").
static std::string BaseNameNoExt(const std::string& path) {
    std::string b = BaseName(path);
    size_t dot = b.find_last_of('.');
    if (dot == std::string::npos || dot == 0) return b;
    return b.substr(0, dot);
}

static std::string JoinPath(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    if (!b.empty() && b[0] == '/') return b;
    return EnsureTrailingSlash(a) + b;
}

// ~ and ~/x expansion, as typed at a prompt.
static std::string ExpandTilde(const std::string& p) {
    if (p == "~") return HomeDir();
    if (StartsWith(p, "~/")) return HomeDir() + p.substr(1);
    return p;
}

static std::string AbsolutePath(const std::string& p) {
    if (p.empty()) return p;
    std::string e = ExpandTilde(p);
    char buf[PATH_MAX];
    if (realpath(e.c_str(), buf)) return buf;
    if (e[0] == '/') return e;
    return JoinPath(CurrentDir(), e);
}

// Strips one pair of surrounding quotes (a path pasted from a file manager is often quoted).
static std::string StripQuotes(std::string s) {
    s = Trim(s);
    if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') || (s.front() == '\'' && s.back() == '\'')))
        s = s.substr(1, s.size() - 2);
    return s;
}

// mkdir -p. Returns true when the directory exists afterwards.
static bool EnsureDirectoryTree(const std::string& directory, mode_t mode = 0755) {
    std::string d = TrimTrailingSlashes(directory);
    if (d.empty()) return false;
    if (DirExists(d)) return true;
    for (size_t pos = 1; pos < d.size(); ++pos) {
        if (d[pos] != '/') continue;
        const std::string partial = d.substr(0, pos);
        if (!DirExists(partial)) mkdir(partial.c_str(), mode);
    }
    mkdir(d.c_str(), mode);
    return DirExists(d);
}

static bool ReadBinaryFile(const std::string& path, std::string& data) {
    data.clear();
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
    const bool ok = !ferror(f);
    fclose(f);
    return ok;
}

static bool WriteBinaryFile(const std::string& path, const std::string& data, mode_t mode = 0644) {
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) return false;
    size_t off = 0;
    while (off < data.size()) {
        const ssize_t n = write(fd, data.data() + off, data.size() - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return false;
        }
        off += (size_t)n;
    }
    fchmod(fd, mode);
    return close(fd) == 0;
}

static std::string GetExecutablePath() {
    char buf[PATH_MAX];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) { buf[n] = '\0'; return buf; }
    return "";
}

static std::string GetExecutableDirectory() {
    const std::string exe = GetExecutablePath();
    return exe.empty() ? CurrentDir() : DirName(exe);
}

// Looks a program up on PATH. Returns its full path, or "" when it is not installed.
static std::string FindInPath(const std::string& name) {
    if (name.empty()) return "";
    if (name.find('/') != std::string::npos) {
        return (access(name.c_str(), X_OK) == 0 && IsRegularFile(name)) ? name : "";
    }
    std::string path = GetEnv("PATH");
    if (path.empty()) path = "/usr/local/bin:/usr/bin:/bin";
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find(':', start);
        if (end == std::string::npos) end = path.size();
        std::string dir = path.substr(start, end - start);
        if (dir.empty()) dir = ".";
        const std::string candidate = JoinPath(dir, name);
        if (IsRegularFile(candidate) && access(candidate.c_str(), X_OK) == 0) return candidate;
        start = end + 1;
    }
    return "";
}

static bool CommandExists(const std::string& name) { return !FindInPath(name).empty(); }

static std::string MakeTempDir(const std::string& prefix) {
    std::string base = GetEnv("TMPDIR");
    if (base.empty() || !DirExists(base)) base = "/tmp";
    std::string pattern = EnsureTrailingSlash(base) + prefix + "XXXXXX";
    std::vector<char> buf(pattern.begin(), pattern.end());
    buf.push_back('\0');
    if (!mkdtemp(buf.data())) return "";
    return std::string(buf.data());
}

static void RemoveTree(const std::string& path) {
    if (path.empty() || path == "/") return;
    DIR* d = opendir(path.c_str());
    if (!d) { unlink(path.c_str()); return; }
    struct dirent* e;
    std::vector<std::string> names;
    while ((e = readdir(d)) != nullptr) {
        const std::string n = e->d_name;
        if (n != "." && n != "..") names.push_back(n);
    }
    closedir(d);
    for (const auto& n : names) {
        const std::string child = path + "/" + n;
        struct stat st;
        if (lstat(child.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) RemoveTree(child);
        else unlink(child.c_str());
    }
    rmdir(path.c_str());
}

static std::string TimestampNow(const char* format) {
    char buf[128];
    time_t t = time(nullptr);
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(buf, sizeof(buf), format, &tmv);
    return buf;
}

static std::string RandomHex(size_t bytes) {
    std::vector<unsigned char> raw(bytes, 0);
    FILE* f = fopen("/dev/urandom", "rb");
    bool ok = false;
    if (f) { ok = fread(raw.data(), 1, bytes, f) == bytes; fclose(f); }
    if (!ok) {
        unsigned long long seed = (unsigned long long)time(nullptr) ^ ((unsigned long long)getpid() << 20) ^
            (unsigned long long)clock();
        for (size_t i = 0; i < bytes; ++i) {
            seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
            raw[i] = (unsigned char)(seed & 0xFF);
        }
    }
    return HexOf(raw.data(), raw.size());
}

// ============================================================
// Running other programs
// ============================================================
struct ProcOpts {
    std::string* out = nullptr;        // capture stdout here
    const std::string* in = nullptr;   // feed this to stdin
    bool quietErr = false;             // send stderr to /dev/null
    bool quietOut = false;             // send stdout to /dev/null (ignored when 'out' is set)
    bool detachedStdin = false;        // stdin from /dev/null
    const std::string* cwd = nullptr;  // run in this directory
};

// Runs argv[0] (looked up on PATH). Returns the exit status, 128+signal when killed, or 127 when the
// program could not be started. While it runs, Ctrl+C is left to the child.
static int RunCommand(const std::vector<std::string>& argv, const ProcOpts& o = ProcOpts()) {
    if (argv.empty()) return 127;
    fflush(stdout);
    fflush(stderr);

    int outPipe[2] = { -1, -1 };
    int inPipe[2] = { -1, -1 };
    if (o.out && pipe(outPipe) != 0) return 127;
    if (o.in && pipe(inPipe) != 0) {
        if (outPipe[0] >= 0) { close(outPipe[0]); close(outPipe[1]); }
        return 127;
    }

    struct sigaction ignore, oldInt, oldQuit;
    memset(&ignore, 0, sizeof(ignore));
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    sigaction(SIGINT, &ignore, &oldInt);
    sigaction(SIGQUIT, &ignore, &oldQuit);

    const pid_t pid = fork();
    if (pid < 0) {
        sigaction(SIGINT, &oldInt, nullptr);
        sigaction(SIGQUIT, &oldQuit, nullptr);
        for (int fd : { outPipe[0], outPipe[1], inPipe[0], inPipe[1] }) if (fd >= 0) close(fd);
        return 127;
    }
    if (pid == 0) {
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);
        if (o.cwd && !o.cwd->empty() && chdir(o.cwd->c_str()) != 0) _exit(127);
        if (o.out) { dup2(outPipe[1], 1); close(outPipe[0]); close(outPipe[1]); }
        else if (o.quietOut) { int n = open("/dev/null", O_WRONLY); if (n >= 0) { dup2(n, 1); close(n); } }
        if (o.in) { dup2(inPipe[0], 0); close(inPipe[0]); close(inPipe[1]); }
        else if (o.detachedStdin) { int n = open("/dev/null", O_RDONLY); if (n >= 0) { dup2(n, 0); close(n); } }
        if (o.quietErr) { int n = open("/dev/null", O_WRONLY); if (n >= 0) { dup2(n, 2); close(n); } }
        std::vector<char*> args;
        for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
        args.push_back(nullptr);
        execvp(args[0], args.data());
        _exit(127);
    }

    if (outPipe[1] >= 0) close(outPipe[1]);
    if (inPipe[0] >= 0) close(inPipe[0]);
    signal(SIGPIPE, SIG_IGN);

    size_t written = 0;
    bool inOpen = o.in != nullptr;
    bool outOpen = o.out != nullptr;
    if (o.out) o.out->clear();
    while (inOpen || outOpen) {
        struct pollfd fds[2];
        int n = 0;
        int inIdx = -1, outIdx = -1;
        if (inOpen) { fds[n].fd = inPipe[1]; fds[n].events = POLLOUT; fds[n].revents = 0; inIdx = n++; }
        if (outOpen) { fds[n].fd = outPipe[0]; fds[n].events = POLLIN; fds[n].revents = 0; outIdx = n++; }
        const int pr = poll(fds, (nfds_t)n, -1);
        if (pr < 0) { if (errno == EINTR) continue; break; }
        if (inIdx >= 0 && (fds[inIdx].revents & (POLLOUT | POLLERR | POLLHUP))) {
            if (written >= o.in->size()) { close(inPipe[1]); inOpen = false; }
            else {
                const ssize_t w = write(inPipe[1], o.in->data() + written, o.in->size() - written);
                if (w < 0) { if (errno != EINTR && errno != EAGAIN) { close(inPipe[1]); inOpen = false; } }
                else written += (size_t)w;
                if (inOpen && written >= o.in->size()) { close(inPipe[1]); inOpen = false; }
            }
        }
        if (outIdx >= 0 && (fds[outIdx].revents & (POLLIN | POLLERR | POLLHUP))) {
            char buf[4096];
            const ssize_t r = read(outPipe[0], buf, sizeof(buf));
            if (r > 0) o.out->append(buf, (size_t)r);
            else if (r == 0 || (r < 0 && errno != EINTR && errno != EAGAIN)) { close(outPipe[0]); outOpen = false; }
        }
    }
    if (inOpen) close(inPipe[1]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    sigaction(SIGINT, &oldInt, nullptr);
    sigaction(SIGQUIT, &oldQuit, nullptr);

    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 127;
}

static std::string FirstLine(const std::string& s) {
    size_t e = s.find_first_of("\r\n");
    return e == std::string::npos ? s : s.substr(0, e);
}

// ============================================================
// Terminal layer (raw mode, escape sequences, signals)
// ============================================================
static bool g_stdoutTty = false;
static bool g_stdinTty = false;
static bool g_ansi = false;            // a real terminal that understands escape sequences (not piped, not TERM=dumb)
static bool g_color = true;
static bool g_enableTUI = true;
static bool g_enableSound = false;
static int g_col = 0;                                  // best-effort cursor column of stdout
static volatile sig_atomic_t g_resizePending = 0;
static volatile sig_atomic_t g_altScreen = 0;
static volatile sig_atomic_t g_cursorHidden = 0;
static volatile sig_atomic_t g_pasteMode = 0;
static volatile sig_atomic_t g_titlePushed = 0;

static struct termios g_cookedTermios;
static bool g_haveCooked = false;
static volatile sig_atomic_t g_rawDepth = 0;

static void WriteRaw(const char* s) {
    // async-signal-safe on purpose: used from signal handlers and atexit
    const size_t n = strlen(s);
    size_t off = 0;
    while (off < n) {
        const ssize_t w = write(1, s + off, n - off);
        if (w <= 0) break;
        off += (size_t)w;
    }
}

static int TermCols() {
    struct winsize ws;
    if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) return ws.ws_col;
    if (ioctl(0, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) return ws.ws_col;
    const int env = atoi(GetEnv("COLUMNS").c_str());
    return env > 0 ? env : 80;
}

static int TermRows() {
    struct winsize ws;
    if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) return ws.ws_row;
    if (ioctl(0, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) return ws.ws_row;
    const int env = atoi(GetEnv("LINES").c_str());
    return env > 0 ? env : 24;
}

static void TermResetAll() {
    if (g_ansi) {
        if (g_pasteMode) { WriteRaw("\033[?2004l"); g_pasteMode = 0; }
        if (g_altScreen) { WriteRaw("\033[?1049l"); g_altScreen = 0; }
        if (g_cursorHidden) { WriteRaw("\033[?25h"); g_cursorHidden = 0; }
        if (g_titlePushed) { WriteRaw("\033[23;0t"); g_titlePushed = 0; }
        WriteRaw("\033[0m");
    }
    if (g_haveCooked && g_rawDepth > 0) {
        tcsetattr(0, TCSANOW, &g_cookedTermios);
    }
    g_rawDepth = 0;
}

static void Flush() { fflush(stdout); }

[[noreturn]] static void Quit(int code) {
    fflush(stdout);
    TermResetAll();
    exit(code);
}

static void OnFatalSignal(int sig) {
    TermResetAll();
    signal(sig, SIG_DFL);
    raise(sig);
}

static void OnWinch(int) { g_resizePending = 1; }

static void InstallTerminalHandlers() {
    g_stdoutTty = isatty(1) != 0;
    g_stdinTty = isatty(0) != 0;
    const std::string term = GetEnv("TERM");
    g_ansi = g_stdoutTty && !term.empty() && term != "dumb";
    g_color = g_ansi && GetEnv("NO_COLOR").empty();

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = OnFatalSignal;
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGHUP, &sa, nullptr);
    sigaction(SIGQUIT, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);
    sa.sa_handler = OnWinch;
    sigaction(SIGWINCH, &sa, nullptr);      // no SA_RESTART: a resize interrupts a blocking read
    signal(SIGPIPE, SIG_IGN);
    atexit(TermResetAll);
}

static void EnterRaw() {
    if (!g_stdinTty) return;
    if (g_rawDepth++ > 0) return;
    if (!g_haveCooked) {
        if (tcgetattr(0, &g_cookedTermios) != 0) { g_rawDepth = 0; return; }
        g_haveCooked = true;
    }
    struct termios t = g_cookedTermios;
    t.c_iflag &= ~(tcflag_t)(BRKINT | ICRNL | INPCK | ISTRIP | IXON | INLCR | IGNCR);
    t.c_lflag &= ~(tcflag_t)(ECHO | ICANON | IEXTEN | ISIG);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &t);
}

static void LeaveRaw() {
    if (g_rawDepth <= 0) return;
    if (--g_rawDepth > 0) return;
    if (g_haveCooked) tcsetattr(0, TCSANOW, &g_cookedTermios);
}

struct RawScope {
    RawScope() { EnterRaw(); }
    ~RawScope() { LeaveRaw(); }
};

struct CursorScope {
    bool changed;
    explicit CursorScope(bool hide) : changed(false) {
        if (hide && g_stdoutTty && !g_cursorHidden) { WriteRaw("\033[?25l"); g_cursorHidden = 1; changed = true; }
    }
    ~CursorScope() {
        if (changed && g_stdoutTty) { WriteRaw("\033[?25h"); g_cursorHidden = 0; }
    }
};

struct PasteScope {
    bool changed;
    PasteScope() : changed(false) {
        if (g_stdoutTty && g_stdinTty && !g_pasteMode) { WriteRaw("\033[?2004h"); g_pasteMode = 1; changed = true; }
    }
    ~PasteScope() {
        if (changed && g_stdoutTty) { WriteRaw("\033[?2004l"); g_pasteMode = 0; }
    }
};

struct AltScreenScope {
    bool changed;
    AltScreenScope() : changed(false) {
        if (g_stdoutTty && !g_altScreen) { WriteRaw("\033[?1049h\033[H\033[2J"); g_altScreen = 1; changed = true; }
    }
    ~AltScreenScope() {
        if (changed && g_stdoutTty) { WriteRaw("\033[?1049l"); g_altScreen = 0; }
    }
};

// ============================================================
// Output (tracks the cursor column so the line editor knows where its prompt ended)
// ============================================================
static void Out(const std::string& s) {
    if (s.empty()) return;
    fwrite(s.data(), 1, s.size(), stdout);
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = (unsigned char)s[i];
        if (c == 0x1B) {
            size_t j = i + 1;
            if (j < s.size() && s[j] == '[') {
                ++j;
                while (j < s.size() && !((unsigned char)s[j] >= 0x40 && (unsigned char)s[j] <= 0x7E)) ++j;
                ++j;
            } else if (j < s.size() && s[j] == ']') {
                while (j < s.size() && s[j] != '\007') ++j;
                ++j;
            } else {
                ++j;
            }
            i = j;
            continue;
        }
        if (c == '\n' || c == '\r') { g_col = 0; ++i; continue; }
        if (c == '\t') { g_col = (g_col / 8 + 1) * 8; ++i; continue; }
        if (c < 0x20) { ++i; continue; }
        size_t len = 1;
        if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        if (i + len > s.size()) len = s.size() - i;
        const std::u32string u = Utf8ToU32(s.substr(i, len));
        if (!u.empty()) g_col += CharWidth(u[0]);
        i += len;
    }
    const int w = TermCols();
    if (w > 0 && g_col >= w) g_col %= w;
}

static void Printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void Printf(const char* fmt, ...) {
    char stackBuf[1024];
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    const int n = vsnprintf(stackBuf, sizeof(stackBuf), fmt, ap);
    va_end(ap);
    if (n >= 0) {
        if ((size_t)n < sizeof(stackBuf)) {
            Out(std::string(stackBuf, (size_t)n));
        } else {
            std::string big((size_t)n + 1, '\0');
            vsnprintf(&big[0], big.size(), fmt, ap2);
            big.resize((size_t)n);
            Out(big);
        }
    }
    va_end(ap2);
}

static void ClearScreen() {
    if (g_ansi) Out("\033[H\033[2J");
    else Out("\n");
    g_col = 0;
}

static void SetTerminalTitle(const std::string& title) {
    if (!g_ansi) return;
    if (!g_titlePushed) { Out("\033[22;0t"); g_titlePushed = 1; }     // remember the old title, restored on exit
    Out("\033]0;" + title + "\007");
}

// ============================================================
// Theme & Color Configuration
// ============================================================
// Colors use the same 0-15 numbers as the Windows edition (bit 0 blue, bit 1 green, bit 2 red,
// bit 3 bright). They are translated to ANSI at draw time. 15 (bright white) is drawn as bold in the
// terminal's own foreground color and 7 (light gray) as "reset", so light and dark terminals both work.
enum ConsoleColor {
    CC_DEFAULT      = -1,
    CC_BLACK        = 0,
    CC_RESET        = 7,
    CC_CYAN         = 11,
    CC_YELLOW       = 14,
    CC_GREEN        = 10,
    CC_RED          = 12,
    CC_WHITE        = 15,
    CC_MAGENTA      = 13,
    CC_DARK_GREEN   = 2,
    CC_DARK_RED     = 4,
    CC_DARK_YELLOW  = 6,
    CC_DARK_WHITE   = 8,
    CC_BRIGHT_GREEN = 10,
    CC_BRIGHT_CYAN  = 11,
    CC_BRIGHT_RED   = 12,
    CC_BRIGHT_WHITE = 15,
    CC_VIOLET       = 13,
    CC_DARK_VIOLET  = 5,
    CC_GRAY         = 8,
    CC_LIGHT_GRAY   = 7,
    CC_BLUE         = 9,
    CC_DARK_BLUE    = 1
};

struct Theme {
    int header;
    int prompt;
    int input;
    int success;
    int error;
    int info;
    int banner;
    int text;
    int accent;
};

static const Theme kDefaultTheme = { CC_CYAN, CC_YELLOW, CC_WHITE, CC_GREEN, CC_RED, CC_MAGENTA, CC_CYAN, CC_DEFAULT, CC_YELLOW };
static Theme g_theme = kDefaultTheme;

static int AnsiIndex(int w) { return ((w & 1) << 2) | (w & 2) | ((w & 4) >> 2); }

static std::string ColorEsc(int color) {
    if (!g_color) return "";
    if (color == CC_RESET) return "\033[0m";
    if (color < 0) return "\033[0m";
    if (color == CC_WHITE) return "\033[0;1m";
    const int base = (color & 8) ? 90 : 30;
    return "\033[0;" + ToStr(base + AnsiIndex(color & 7)) + "m";
}

static std::string InvertedEsc(int bg, int fg) {
    if (!g_color) return "\033[0;7m";
    if (bg < 0) return "\033[0;7m";
    const int bgBase = (bg & 8) ? 100 : 40;
    const int fgBase = (fg & 8) ? 90 : 30;
    return "\033[0;" + ToStr(fgBase + AnsiIndex(fg & 7)) + ";" + ToStr(bgBase + AnsiIndex(bg & 7)) + "m";
}

static void SetColor(int color) { Out(ColorEsc(color)); }

static void ApplyTheme(const std::string& themeName) {
    if (themeName == "matrix") {
        g_theme = { CC_DARK_GREEN, CC_GREEN, CC_BRIGHT_GREEN, CC_GREEN, CC_DARK_GREEN, CC_GREEN, CC_DARK_GREEN, CC_GREEN, CC_BRIGHT_GREEN };
    } else if (themeName == "cyberpunk") {
        g_theme = { CC_CYAN, CC_YELLOW, CC_MAGENTA, CC_GREEN, CC_RED, CC_MAGENTA, CC_CYAN, CC_DEFAULT, CC_YELLOW };
    } else if (themeName == "cli") {
        g_theme = { CC_LIGHT_GRAY, CC_GRAY, CC_BRIGHT_WHITE, CC_DEFAULT, CC_DARK_WHITE, CC_LIGHT_GRAY, CC_GRAY, CC_BRIGHT_WHITE, CC_DEFAULT };
    } else if (themeName == "violent") {
        g_theme = { CC_RED, CC_BRIGHT_RED, CC_MAGENTA, CC_GREEN, CC_DARK_RED, CC_VIOLET, CC_DARK_RED, CC_DEFAULT, CC_BRIGHT_RED };
    } else {
        g_theme = kDefaultTheme;
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
#define C_R SetColor(CC_RESET)

static void Sleep(int ms) {
    Flush();
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}

// ============================================================
// Menu sounds
// ============================================================
// Order of preference: SoX "play" (exact tones, like the Windows beeps), the freedesktop sound theme
// through canberra-gtk-play or paplay, and finally the terminal bell.
static void SpawnDetached(const std::vector<std::string>& argv) {
    if (argv.empty()) return;
    const pid_t first = fork();
    if (first < 0) return;
    if (first == 0) {
        const pid_t second = fork();
        if (second != 0) _exit(0);
        setsid();
        const int n = open("/dev/null", O_RDWR);
        if (n >= 0) { dup2(n, 0); dup2(n, 1); dup2(n, 2); if (n > 2) close(n); }
        std::vector<char*> args;
        for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
        args.push_back(nullptr);
        execvp(args[0], args.data());
        _exit(127);
    }
    int status = 0;
    while (waitpid(first, &status, 0) < 0 && errno == EINTR) {}
}

static void PlayMenuSound(int type) {
    if (!g_enableSound) return;
    static int backend = -1;      // 0 bell, 1 sox, 2 canberra, 3 paplay
    static std::string themeDir = "/usr/share/sounds/freedesktop/stereo/";
    if (backend < 0) {
        if (CommandExists("play")) backend = 1;
        else if (CommandExists("canberra-gtk-play")) backend = 2;
        else if (CommandExists("paplay") && DirExists(themeDir)) backend = 3;
        else backend = 0;
    }
    if (backend == 1) {
        SpawnDetached({ "play", "-nq", "synth", type == 0 ? "0.02" : "0.045", "sine", type == 0 ? "750" : "1150" });
    } else if (backend == 2) {
        SpawnDetached({ "canberra-gtk-play", "-i", type == 0 ? "audio-volume-change" : "message" });
    } else if (backend == 3) {
        SpawnDetached({ "paplay", themeDir + (type == 0 ? "audio-volume-change.oga" : "message.oga") });
    } else if (g_stdoutTty) {
        WriteRaw("\a");
    }
}

// ============================================================
// Keyboard input
// ============================================================
enum KeyCode {
    K_NONE = 0, K_CHAR, K_ENTER, K_ESC, K_TAB, K_BACKTAB, K_BACKSPACE, K_DELETE, K_INSERT,
    K_UP, K_DOWN, K_LEFT, K_RIGHT, K_HOME, K_END, K_PGUP, K_PGDN, K_F3, K_PASTE, K_RESIZE, K_EOF
};

struct Key {
    int code = K_NONE;
    char32_t ch = 0;
    bool ctrl = false, shift = false, alt = false;
    std::string text;      // payload of K_PASTE
};

// -1 timeout, -2 end of input / error, -3 interrupted by a signal
static int ReadByte(int timeoutMs) {
    struct pollfd p;
    p.fd = 0;
    p.events = POLLIN;
    p.revents = 0;
    const int r = poll(&p, 1, timeoutMs);
    if (r < 0) return errno == EINTR ? -3 : -2;
    if (r == 0) return -1;
    unsigned char c = 0;
    const ssize_t n = read(0, &c, 1);
    if (n == 0) return -2;
    if (n < 0) return (errno == EINTR || errno == EAGAIN) ? -3 : -2;
    return c;
}

static void ApplyModifier(Key& k, int m) {
    if (m < 2) return;
    const int bits = m - 1;
    k.shift = (bits & 1) != 0;
    k.alt = (bits & 2) != 0;
    k.ctrl = (bits & 4) != 0;
}

static Key ParseEscapeSequence() {
    Key k;
    int b = ReadByte(60);
    if (b < 0) { k.code = K_ESC; return k; }

    if (b == '[') {
        std::string params;
        int fin = 0;
        while (true) {
            const int c = ReadByte(60);
            if (c < 0) { k.code = K_ESC; return k; }
            if (c >= 0x40 && c <= 0x7E) { fin = c; break; }
            params.push_back((char)c);
            if (params.size() > 40) { k.code = K_NONE; return k; }
        }
        std::vector<int> nums;
        {
            std::string cur;
            for (char ch : params) {
                if (ch == ';') { nums.push_back(atoi(cur.c_str())); cur.clear(); }
                else cur.push_back(ch);
            }
            nums.push_back(atoi(cur.c_str()));
        }
        const int first = nums.empty() ? 0 : nums[0];
        ApplyModifier(k, nums.size() >= 2 ? nums[1] : 0);
        switch (fin) {
            case 'A': k.code = K_UP; return k;
            case 'B': k.code = K_DOWN; return k;
            case 'C': k.code = K_RIGHT; return k;
            case 'D': k.code = K_LEFT; return k;
            case 'H': k.code = K_HOME; return k;
            case 'F': k.code = K_END; return k;
            case 'Z': k.code = K_BACKTAB; k.shift = true; return k;
            case 'R': k.code = K_F3; return k;       // ESC [ 1 ; m R is F3 with modifiers
            case '~':
                switch (first) {
                    case 1: case 7: k.code = K_HOME; return k;
                    case 2: k.code = K_INSERT; return k;
                    case 3: k.code = K_DELETE; return k;
                    case 4: case 8: k.code = K_END; return k;
                    case 5: k.code = K_PGUP; return k;
                    case 6: k.code = K_PGDN; return k;
                    case 13: k.code = K_F3; return k;
                    case 200: {
                        std::string text;
                        const std::string endMark = "\033[201~";
                        while (true) {
                            const int c = ReadByte(1500);
                            if (c < 0) break;
                            text.push_back((char)c);
                            if (text.size() >= endMark.size() &&
                                text.compare(text.size() - endMark.size(), endMark.size(), endMark) == 0) {
                                text.resize(text.size() - endMark.size());
                                break;
                            }
                        }
                        k.code = K_PASTE;
                        k.text = text;
                        return k;
                    }
                    default: break;
                }
                break;
            default: break;
        }
        k.code = K_NONE;
        return k;
    }

    if (b == 'O') {
        const int c = ReadByte(60);
        if (c < 0) { k.code = K_NONE; return k; }
        switch (c) {
            case 'A': k.code = K_UP; return k;
            case 'B': k.code = K_DOWN; return k;
            case 'C': k.code = K_RIGHT; return k;
            case 'D': k.code = K_LEFT; return k;
            case 'H': k.code = K_HOME; return k;
            case 'F': k.code = K_END; return k;
            case 'R': k.code = K_F3; return k;
            default: k.code = K_NONE; return k;
        }
    }

    if (b == 27) { k.code = K_ESC; return k; }
    k.alt = true;
    if (b == 127 || b == 8) { k.code = K_BACKSPACE; return k; }
    if (b == 13 || b == 10) { k.code = K_ENTER; return k; }
    if (b >= 0x20) { k.code = K_CHAR; k.ch = (char32_t)b; return k; }
    k.code = K_NONE;
    return k;
}

static Key ReadKey() {
    Flush();
    Key k;
    while (true) {
        if (g_resizePending) { g_resizePending = 0; k.code = K_RESIZE; return k; }
        const int b = ReadByte(-1);
        if (b == -3) continue;
        if (b == -2) { k.code = K_EOF; return k; }
        if (b < 0) continue;
        if (b == 27) {
            k = ParseEscapeSequence();
            if (k.code == K_NONE) continue;
            return k;
        }
        if (b == 13 || b == 10) { k.code = K_ENTER; return k; }
        if (b == 9) { k.code = K_TAB; return k; }
        if (b == 127 || b == 8) { k.code = K_BACKSPACE; return k; }
        if (b == 0) continue;
        if (b < 0x20) {
            k.code = K_CHAR;
            k.ctrl = true;
            k.ch = (char32_t)('a' + b - 1);
            return k;
        }
        std::string bytes(1, (char)b);
        size_t need = 0;
        if ((b & 0xE0) == 0xC0) need = 1;
        else if ((b & 0xF0) == 0xE0) need = 2;
        else if ((b & 0xF8) == 0xF0) need = 3;
        for (size_t i = 0; i < need; ++i) {
            const int c = ReadByte(30);
            if (c < 0) break;
            bytes.push_back((char)c);
        }
        const std::u32string u = Utf8ToU32(bytes);
        k.code = K_CHAR;
        k.ch = u.empty() ? (char32_t)'?' : u[0];
        return k;
    }
}

// One line from stdin read byte by byte, so a program started later still sees the rest of a pipe.
static bool ReadRawLine(std::string& line) {
    line.clear();
    bool got = false;
    while (true) {
        char c = 0;
        const ssize_t n = read(0, &c, 1);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        got = true;
        if (c == '\n') break;
        line.push_back(c);
    }
    return got;
}

static void PressAnyKey(const std::string& message = "Press any key to continue . . .") {
    C_P; Out("\n  " + message); C_R;
    Flush();
    if (g_stdinTty) {
        RawScope raw;
        while (true) {
            const Key k = ReadKey();
            if (k.code == K_RESIZE) continue;
            if (k.code == K_EOF) break;
            if (k.code == K_PASTE) continue;
            if (k.code == K_CHAR && k.ctrl && k.ch == 'c') { Out("\n"); Quit(130); }
            break;
        }
    } else {
        std::string ignored;
        ReadRawLine(ignored);
    }
    Out("\n");
}

// ============================================================
// Line input
// ============================================================
[[noreturn]] static void HandleInputEof() {
    Out("\n");
    Quit(0);
}

static std::string ReadLinePlain() {
    Flush();
    std::string line;
    if (!ReadRawLine(line)) HandleInputEof();
    while (!line.empty() && line.back() == '\r') line.pop_back();
    if (!g_stdinTty) Out(line + "\n");     // piped input: echo it so a transcript reads naturally
    return line;
}

// Completes the path before the cursor (Tab). Only the unique or the common prefix is inserted.
static void CompletePathAtCursor(std::u32string& buf, size_t& cur) {
    size_t start = cur;
    while (start > 0 && buf[start - 1] != U' ') --start;
    const std::string typed = U32ToUtf8(buf.substr(start, cur - start));
    const std::string expanded = ExpandTilde(typed);
    std::string dir, prefix;
    const size_t slash = expanded.find_last_of('/');
    if (slash == std::string::npos) { dir = "."; prefix = expanded; }
    else { dir = slash == 0 ? "/" : expanded.substr(0, slash); prefix = expanded.substr(slash + 1); }
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    std::vector<std::string> matches;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        const std::string n = e->d_name;
        if (n == "." || n == "..") continue;
        if (n.compare(0, prefix.size(), prefix) != 0) continue;
        if (prefix.empty() && n[0] == '.') continue;
        matches.push_back(n);
    }
    closedir(d);
    if (matches.empty()) return;
    std::string common = matches[0];
    for (const auto& m : matches) {
        size_t i = 0;
        while (i < common.size() && i < m.size() && common[i] == m[i]) ++i;
        common.resize(i);
    }
    std::string completed = (slash == std::string::npos ? "" : expanded.substr(0, slash + 1)) + common;
    if (matches.size() == 1 && DirExists(completed)) completed += "/";
    if (typed.size() > 0 && typed[0] == '~' && StartsWith(completed, HomeDir()))
        completed = "~" + completed.substr(HomeDir().size());
    if (completed.size() <= typed.size() && matches.size() > 1) return;
    buf = buf.substr(0, start) + Utf8ToU32(completed) + buf.substr(cur);
    cur = start + Utf8ToU32(completed).size();
}

struct ReadLineOptions {
    bool secret = false;       // echo * instead of the typed characters
    bool pathMode = false;     // Tab completes file names
    bool* cancelled = nullptr; // when set, Esc cancels and sets this to true
};

// A small readline-style editor: arrows, Home/End, Ctrl+A/E/U/K/W, Delete, paste, UTF-8.
static std::string ReadLineEx(const std::string& initial, const ReadLineOptions& opt) {
    Flush();
    if (opt.cancelled) *opt.cancelled = false;
    if (!g_stdinTty || !g_ansi) return ReadLinePlain();     // pipes and dumb terminals: the tty's own line editing

    RawScope raw;
    PasteScope paste;
    std::u32string buf = Utf8ToU32(initial);
    size_t cur = buf.size();
    const int startColRaw = g_col;
    int lastRow = 0;

    auto glyph = [&](char32_t c) -> char32_t { return opt.secret ? (char32_t)'*' : DisplayChar(c); };
    auto place = [&](size_t upto, int W, int& row, int& col) {
        row = 0;
        col = startColRaw % W;
        for (size_t i = 0; i < upto && i < buf.size(); ++i) {
            const int w = CharWidth(glyph(buf[i]));
            if (col + w > W) { ++row; col = 0; }
            col += w;
            if (col >= W) { ++row; col = 0; }
        }
    };
    auto redraw = [&]() {
        const int W = std::max(8, TermCols());
        std::string o;
        if (lastRow > 0) o += "\033[" + ToStr(lastRow) + "A";
        o += "\r";
        const int col0 = startColRaw % W;
        if (col0 > 0) o += "\033[" + ToStr(col0) + "C";
        int row = 0, col = col0;
        for (char32_t c : buf) {
            const char32_t g = glyph(c);
            const int w = CharWidth(g);
            if (col + w > W) { o += "\r\n"; ++row; col = 0; }
            AppendUtf8(o, g);
            col += w;
            if (col >= W) { o += "\r\n"; ++row; col = 0; }
        }
        o += "\033[J";
        int curRow = 0, curCol = 0;
        place(cur, W, curRow, curCol);
        if (row > curRow) o += "\033[" + ToStr(row - curRow) + "A";
        o += "\r";
        if (curCol > 0) o += "\033[" + ToStr(curCol) + "C";
        lastRow = curRow;
        Out(o);
        Flush();
    };
    auto finish = [&]() {
        const int W = std::max(8, TermCols());
        int endRow = 0, endCol = 0;
        place(buf.size(), W, endRow, endCol);
        std::string o;
        if (endRow > lastRow) o += "\033[" + ToStr(endRow - lastRow) + "B";
        o += "\r";
        if (endCol > 0) o += "\033[" + ToStr(endCol) + "C";
        o += "\n";
        Out(o);
        g_col = 0;
        Flush();
    };
    auto isWordChar = [](char32_t c) { return !(c == U' ' || c == U'\t'); };
    auto wordLeft = [&](size_t from) {
        while (from > 0 && !isWordChar(buf[from - 1])) --from;
        while (from > 0 && isWordChar(buf[from - 1])) --from;
        return from;
    };
    auto wordRight = [&](size_t from) {
        while (from < buf.size() && isWordChar(buf[from])) ++from;
        while (from < buf.size() && !isWordChar(buf[from])) ++from;
        return from;
    };

    redraw();
    while (true) {
        const Key k = ReadKey();
        bool changed = true;
        switch (k.code) {
            case K_RESIZE: break;
            case K_EOF: HandleInputEof();
            case K_ENTER: finish(); return U32ToUtf8(buf);
            case K_ESC:
                if (opt.cancelled) { *opt.cancelled = true; finish(); return ""; }
                changed = false;
                break;
            case K_TAB:
                if (opt.pathMode) CompletePathAtCursor(buf, cur);
                else changed = false;
                break;
            case K_BACKSPACE:
                if (k.alt) { const size_t to = wordLeft(cur); buf.erase(to, cur - to); cur = to; }
                else if (cur > 0) { buf.erase(cur - 1, 1); --cur; }
                else changed = false;
                break;
            case K_DELETE:
                if (cur < buf.size()) buf.erase(cur, 1); else changed = false;
                break;
            case K_LEFT:
                if (k.ctrl || k.alt) cur = wordLeft(cur); else if (cur > 0) --cur; else changed = false;
                break;
            case K_RIGHT:
                if (k.ctrl || k.alt) cur = wordRight(cur); else if (cur < buf.size()) ++cur; else changed = false;
                break;
            case K_HOME: cur = 0; break;
            case K_END: cur = buf.size(); break;
            case K_PASTE: {
                std::u32string add;
                for (char32_t c : Utf8ToU32(k.text)) {
                    if (c == U'\r' || c == U'\n' || c == U'\t') add.push_back(U' ');
                    else if (c >= 0x20 && c != 0x7F) add.push_back(c);
                }
                buf.insert(cur, add);
                cur += add.size();
                break;
            }
            case K_CHAR:
                if (k.ctrl) {
                    switch (k.ch) {
                        case 'a': cur = 0; break;
                        case 'e': cur = buf.size(); break;
                        case 'u': buf.erase(0, cur); cur = 0; break;
                        case 'k': buf.erase(cur); break;
                        case 'w': { const size_t to = wordLeft(cur); buf.erase(to, cur - to); cur = to; break; }
                        case 'd':
                            if (buf.empty()) { finish(); HandleInputEof(); }
                            else if (cur < buf.size()) buf.erase(cur, 1);
                            else changed = false;
                            break;
                        case 'c': Out("^C"); finish(); Quit(130);
                        default: changed = false; break;
                    }
                } else if (k.alt) {
                    if (k.ch == 'b') cur = wordLeft(cur);
                    else if (k.ch == 'f') cur = wordRight(cur);
                    else if (k.ch == 'd') { const size_t to = wordRight(cur); buf.erase(cur, to - cur); }
                    else changed = false;
                } else {
                    buf.insert(buf.begin() + (std::ptrdiff_t)cur, k.ch);
                    ++cur;
                }
                break;
            default: changed = false; break;
        }
        if (changed) redraw();
    }
}

static std::string ReadLine() { return ReadLineEx("", ReadLineOptions()); }

static std::string ReadLineWithDefault(const std::string& initial) { return ReadLineEx(initial, ReadLineOptions()); }

static std::string ReadPathLine() {
    ReadLineOptions o;
    o.pathMode = true;
    return ReadLineEx("", o);
}

static std::string ReadSecret() {
    ReadLineOptions o;
    o.secret = true;
    return ReadLineEx("", o);
}

// ============================================================
// TUI menu
// ============================================================
// "  ============================================" sized to fit the terminal (44 columns when there is room).
static std::string BannerRule() {
    const int w = std::max(8, std::min(44, TermCols() - 3));
    return "  " + std::string((size_t)w, '=');
}

static std::string BannerTitle(const std::string& title) {
    const int w = std::max(8, std::min(44, TermCols() - 3));
    const int tw = DisplayWidth(title);
    int pad = (w - tw) / 2;
    if (pad < 0) pad = 0;
    return "  " + std::string((size_t)pad, ' ') + FitToWidth(title, w);
}

// Up/Down (or k/j), Home/End, PageUp/PageDown, Enter to choose, Esc to go back (returns -1).
// Menus taller than the terminal scroll inside a window.
static int RunNumberedMenu(const std::string& menuTitle, const std::vector<std::string>& options, int defaultIdx, int confirmedIdx) {
    C_B; Out("\n" + BannerRule() + "\n"); C_H;
    Out(BannerTitle(menuTitle) + "\n"); C_B;
    Out(BannerRule() + "\n\n"); C_R;
    for (size_t i = 0; i < options.size(); ++i) {
        C_P; Printf("    %zu) ", i + 1); C_T;
        Out(options[i] + (confirmedIdx == (int)i ? "  (Selected)" : "") + "\n");
    }
    C_P; Printf("\n  Select [%d]: ", defaultIdx + 1); C_I;
    const std::string a = Trim(ReadLine());
    C_R;
    if (a.empty()) return defaultIdx;
    const int n = atoi(a.c_str());
    if (n >= 1 && n <= (int)options.size()) return n - 1;
    return -1;
}

static int RunTUIMenu(const std::string& menuTitle, const std::vector<std::string>& options,
    int defaultIdx = 0, int confirmedIdx = -1, bool clearScreen = true) {
    const int count = (int)options.size();
    if (count == 0) return -1;
    if (!g_stdinTty || !g_stdoutTty) {
        return RunNumberedMenu(menuTitle, options, std::max(0, std::min(defaultIdx, count - 1)), confirmedIdx);
    }

    int cur = std::max(0, std::min(defaultIdx, count - 1));
    RawScope raw;
    CursorScope hideCursor(true);

    int top = 0;
    int visible = count;
    auto computeVisible = [&]() {
        const int headerRows = 4 + (clearScreen ? 0 : 1);
        visible = std::max(3, std::min(count, TermRows() - headerRows - 3));
        if (visible > count) visible = count;
    };
    auto drawHeader = [&]() {
        if (clearScreen) ClearScreen(); else Out("\n");
        C_B; Out(BannerRule() + "\n"); C_H;
        Out(BannerTitle(menuTitle) + "\n"); C_B;
        Out(BannerRule() + "\n\n"); C_R;
    };
    auto rowText = [&](int i, bool selected) {
        const int cols = std::max(10, TermCols() - 1);
        std::string row;
        if (i == confirmedIdx) {
            row = ColorEsc(g_theme.success) + FitToWidth("    [X] " + options[(size_t)i] + "  (Selected)", cols) + ColorEsc(CC_RESET);
        } else if (selected) {
            row = ColorEsc(CC_RESET) + "    " + InvertedEsc(g_theme.accent, CC_BLACK) +
                FitToWidth(" > " + options[(size_t)i] + " ", cols - 4) + ColorEsc(CC_RESET);
        } else {
            row = ColorEsc(g_theme.text) + FitToWidth("        " + options[(size_t)i], cols) + ColorEsc(CC_RESET);
        }
        return row;
    };
    auto drawRows = [&](bool first) {
        if (cur < top) top = cur;
        if (cur >= top + visible) top = cur - visible + 1;
        top = std::max(0, std::min(top, count - visible));
        std::string o;
        if (!first) o += "\033[" + ToStr(visible + 1) + "A\r";
        for (int r = 0; r < visible; ++r) {
            const int i = top + r;
            o += "\033[2K" + rowText(i, i == cur) + "\n";
        }
        o += "\033[2K\n";
        Out(o);
        g_col = 0;
        Flush();
    };

    computeVisible();
    drawHeader();
    drawRows(true);

    while (true) {
        const Key k = ReadKey();
        const int old = cur;
        switch (k.code) {
            case K_RESIZE:
                if (clearScreen) { computeVisible(); drawHeader(); drawRows(true); }
                continue;
            case K_EOF: Quit(0);
            case K_UP: cur = (cur - 1 + count) % count; break;
            case K_DOWN: cur = (cur + 1) % count; break;
            case K_HOME: cur = 0; break;
            case K_END: cur = count - 1; break;
            case K_PGUP: cur = std::max(0, cur - visible); break;
            case K_PGDN: cur = std::min(count - 1, cur + visible); break;
            case K_ENTER: PlayMenuSound(1); return cur;
            case K_ESC: return -1;
            case K_CHAR:
                if (k.ctrl && k.ch == 'c') { Out("\n"); Quit(130); }
                if (k.ctrl && k.ch == 'd') return -1;
                if (!k.ctrl && !k.alt && k.ch == 'k') cur = (cur - 1 + count) % count;
                else if (!k.ctrl && !k.alt && k.ch == 'j') cur = (cur + 1) % count;
                break;
            default: break;
        }
        if (cur != old) {
            PlayMenuSound(0);
            drawRows(false);
        }
    }
}

// ============================================================
// Data model
// ============================================================
struct OptionData {
    std::string label;
    std::vector<std::string> commands;
    int runMode = 0;              // 0=direct call, 1=bash -c wrapper, 2=submenu
    std::vector<OptionData> subOptions;
};

static const int kBannerWidth = 57;     // width of the border / banner of generated menus
static const int kMaxOptions = 20;      // options per menu
static const int kMaxDepth = 6;         // submenu nesting depth

static bool ConsoleTextEditor(std::vector<std::string>& lines, const std::string& savePath = "", const std::string& finishLabel = "");
static void EditLinesWithConsoleEditor(std::vector<std::string>& lines, bool removeEmptyLines = false);
static bool ReadConsoleEditorTextFile(const std::string& path, std::vector<std::string>& lines);

static std::string MakeBorder(const std::string& ch) {
    std::string unit = FirstChar(ch);
    if (unit.empty()) unit = "=";
    std::string r;
    for (int i = 0; i < kBannerWidth; ++i) r += unit;
    return r;
}

// ============================================================
// Folders (XDG): settings live in the config dir, everything else in the data dir
// ============================================================
static std::string GetConfigDir(bool create = true) {
    std::string base = GetEnv("XDG_CONFIG_HOME");
    if (base.empty() || base[0] != '/') base = HomeDir() + "/.config";
    const std::string dir = EnsureTrailingSlash(base) + "BashMenuGenerator/";
    if (create) EnsureDirectoryTree(dir, 0755);
    return dir;
}

static std::string GetAppDataDir(bool create = true) {
    std::string base = GetEnv("XDG_DATA_HOME");
    if (base.empty() || base[0] != '/') base = HomeDir() + "/.local/share";
    const std::string dir = EnsureTrailingSlash(base) + "BashMenuGenerator/";
    if (create) EnsureDirectoryTree(dir, 0755);
    return dir;
}

// Folder for the user-level "bmg" command and desktop entries.
static std::string GetUserBinDir() { return HomeDir() + "/.local/bin/"; }
static std::string GetApplicationsDir() {
    std::string base = GetEnv("XDG_DATA_HOME");
    if (base.empty() || base[0] != '/') base = HomeDir() + "/.local/share";
    return EnsureTrailingSlash(base) + "applications/";
}

static std::string GetDesktopFolder() {
    std::string out;
    if (CommandExists("xdg-user-dir")) {
        ProcOpts o;
        o.out = &out;
        o.quietErr = true;
        if (RunCommand({ "xdg-user-dir", "DESKTOP" }, o) == 0) {
            out = Trim(FirstLine(out));
            if (!out.empty() && out != HomeDir() && DirExists(out)) return out;
        }
    }
    const std::string fallback = HomeDir() + "/Desktop";
    if (DirExists(fallback)) return fallback;
    return "";
}

// ============================================================
// Settings / bconfig Engine
// ============================================================
struct AppConfig {
    bool theme_engine = false;
    std::string theme_preset = "default";
    bool autosave = true;
    int end = 0;        // 0=prompt, 1=main menu, 2=launch script, 3=exit
    int saveload = 0;   // 0=prompt, 1=file browser, 2=manual path
    int menuborder = 0; // 0=prompt, 1=Classic, 2=Modern, 3=ZigZag
    int bconfig = 1;    // 0=disabled, 1=enabled
    int tui = 1;        // 0=disabled, 1=enabled
    int sound = 0;      // 0=disabled, 1=enabled
    std::string filebrowser = "auto";   // auto | zenity | kdialog | yad | qarma | builtin
    std::string editor = "auto";        // auto | a command name (nano, vim, kate, ...)
};

static AppConfig g_appConfig;

static std::string GetSettingsFilePath() {
    const std::string target = GetConfigDir() + "settings.txt";

    // Settings used to sit next to the program. Move an old file into the config folder once.
    if (!PathExists(target)) {
        const std::string exeDir = EnsureTrailingSlash(GetExecutableDirectory());
        const std::string legacyOne = exeDir + "settings.txt";
        const std::string legacyTwo = exeDir + "bconfig.txt";
        std::string source;
        if (IsRegularFile(legacyOne)) source = legacyOne;
        else if (IsRegularFile(legacyTwo)) source = legacyTwo;
        if (!source.empty()) {
            std::string data;
            if (ReadBinaryFile(source, data) && WriteBinaryFile(target, data, 0644)) unlink(source.c_str());
        }
    }
    return target;
}

static bool SaveSettingsFile(const AppConfig& cfg) {
    std::string content;
    content += "theme: " + std::string(cfg.theme_engine ? "true" : "false") + "\n";
    content += "themepreset: " + cfg.theme_preset + "\n";
    content += "autosave: " + std::string(cfg.autosave ? "true" : "false") + "\n";
    content += "end: " + ToStr(cfg.end) + "\n";
    content += "saveload: " + ToStr(cfg.saveload) + "\n";
    content += "menuborder: " + ToStr(cfg.menuborder) + "\n";
    content += "bconfig: " + ToStr(cfg.bconfig) + "\n";
    content += "tui: " + ToStr(cfg.tui) + "\n";
    content += "sound: " + ToStr(cfg.sound) + "\n";
    content += "filebrowser: " + cfg.filebrowser + "\n";
    content += "editor: " + cfg.editor + "\n";
    return WriteBinaryFile(GetSettingsFilePath(), content, 0644);
}

static bool IsKnownFileBrowser(const std::string& v) {
    return v == "auto" || v == "zenity" || v == "kdialog" || v == "yad" || v == "qarma" || v == "builtin";
}

static void LoadSettingsFile(AppConfig& cfg, std::vector<std::string>& warnings) {
    const std::string path = GetSettingsFilePath();
    if (!PathExists(path)) {
        SaveSettingsFile(cfg);
        return;
    }
    std::string raw;
    if (!ReadBinaryFile(path, raw)) return;

    for (const std::string& rawLine : SplitLines(raw)) {
        const std::string line = Trim(rawLine);
        if (line.empty() || line.compare(0, 2, "//") == 0 || line[0] == '#') continue;
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;

        const std::string key = ToLower(Trim(line.substr(0, colon)));
        const std::string val = Trim(line.substr(colon + 1));
        const std::string lval = ToLower(val);
        const bool truthy = (lval == "true" || lval == "1" || lval == "yes" || lval == "on");
        const bool isPreset = (lval == "default" || lval == "matrix" || lval == "cyberpunk" || lval == "cli" || lval == "violent");

        if (key == "theme" || key == "theme_engine") {
            if (isPreset) cfg.theme_preset = lval;          // spelling used by the first Linux builds
            else cfg.theme_engine = truthy;
        } else if (key == "themepreset" || key == "theme_preset") {
            if (isPreset) cfg.theme_preset = lval;
            else warnings.push_back("\"" + val + "\" is not a recognized theme preset. Valid options are: default, matrix, cyberpunk, cli, violent");
        } else if (key == "autosave") {
            cfg.autosave = truthy;
        } else if (key == "end") {
            const int v = atoi(val.c_str());
            if (v >= 0 && v <= 3) cfg.end = v;
            else warnings.push_back("\"" + val + "\" is not a recognized end option. Valid options are: 0 (Prompt), 1 (Main Menu), 2 (Launch), 3 (Exit)");
        } else if (key == "saveload" || key == "save/load" || key == "load") {
            const int v = atoi(val.c_str());
            if (v >= 0 && v <= 2) cfg.saveload = v;
            else warnings.push_back("\"" + val + "\" is not a recognized save/load option. Valid options are: 0 (Prompt), 1 (File browser), 2 (Manual)");
        } else if (key == "menuborder") {
            const int v = atoi(val.c_str());
            if (v >= 0 && v <= 3) cfg.menuborder = v;
            else warnings.push_back("\"" + val + "\" is not a recognized menuborder option. Valid options are: 0 (Prompt), 1 (Classic), 2 (Modern), 3 (ZigZag)");
        } else if (key == "bconfig") {
            const int v = atoi(val.c_str());
            if (v == 0 || v == 1) cfg.bconfig = v;
            else warnings.push_back("\"" + val + "\" is not a recognized bconfig option. Valid options are: 0 (Disabled), 1 (Enabled)");
        } else if (key == "tui") {
            const int v = atoi(val.c_str());
            if (v == 0 || v == 1) cfg.tui = v;
        } else if (key == "sound") {
            const int v = atoi(val.c_str());
            if (v == 0 || v == 1) cfg.sound = v;
        } else if (key == "filebrowser" || key == "browser") {
            if (IsKnownFileBrowser(lval)) cfg.filebrowser = lval;
            else warnings.push_back("\"" + val + "\" is not a recognized file browser. Valid options are: auto, zenity, kdialog, yad, qarma, builtin");
        } else if (key == "editor") {
            if (!val.empty()) cfg.editor = val;
        }
    }
}

// Pushes the loaded settings into the live program state (theme, TUI, sound).
static void ApplyConfigToProgram(const AppConfig& cfg) {
    const std::string term = GetEnv("TERM");
    g_enableTUI = (cfg.tui == 1) && g_stdinTty && g_stdoutTty && !term.empty() && term != "dumb";
    g_enableSound = (cfg.sound == 1);
    if (cfg.theme_engine) ApplyTheme(cfg.theme_preset);
    else ApplyTheme("default");
}

static void ReloadSettingsFromDisk(std::vector<std::string>& warnings) {
    AppConfig fresh;
    std::vector<std::string> localWarnings;
    LoadSettingsFile(fresh, localWarnings);
    g_appConfig = fresh;
    ApplyConfigToProgram(g_appConfig);
    warnings.insert(warnings.end(), localWarnings.begin(), localWarnings.end());
}

// ============================================================
// Small interactive input helpers
// ============================================================
// Digits only. Enter keeps the default (returns ""), Esc clears the input and returns "".
static std::string ReadOptionCountInput() {
    if (!g_enableTUI || !g_stdinTty) return ReadLine();
    Flush();
    RawScope raw;
    const int startCol = g_col;
    std::string typed;
    auto redraw = [&]() {
        std::string o = "\r";
        if (startCol > 0) o += "\033[" + ToStr(startCol) + "C";
        o += typed + "\033[K";
        Out(o);
        Flush();
    };
    while (true) {
        const Key k = ReadKey();
        if (k.code == K_ENTER) { Out("\n"); g_col = 0; return typed; }
        if (k.code == K_ESC) { typed.clear(); redraw(); Out("\n"); g_col = 0; return ""; }
        if (k.code == K_EOF) HandleInputEof();
        if (k.code == K_BACKSPACE) { if (!typed.empty()) typed.pop_back(); redraw(); continue; }
        if (k.code == K_CHAR && k.ctrl && k.ch == 'c') { Out("\n"); Quit(130); }
        if (k.code == K_CHAR && !k.ctrl && !k.alt && k.ch >= '0' && k.ch <= '9' && typed.size() < 4) {
            typed.push_back((char)k.ch);
            redraw();
        }
    }
}

static bool AskYesNo(const std::string& prompt, bool defaultYes) {
    C_P; Out("  " + prompt + (defaultYes ? " (Y/n): " : " (y/N): ")); C_I;
    const std::string a = Trim(ReadLine());
    C_R;
    if (a.empty()) return defaultYes;
    return IsYes(a);
}

// Title entry with a live preview of the centered banner. Returns false when cancelled with Esc.
static bool ReadTitleWithPreview(std::string& title) {
    const size_t LIMIT = (size_t)kBannerWidth;
    std::u32string buf = Utf8ToU32(title);
    if (buf.size() > LIMIT) buf.resize(LIMIT);

    if (!g_enableTUI || !g_stdinTty || !g_stdoutTty) {
        C_P; Out("  Enter title: "); C_I;
        std::u32string t = Utf8ToU32(ReadLineWithDefault(title));
        C_R;
        if (t.size() > LIMIT) t.resize(LIMIT);
        title = U32ToUtf8(t);
        return true;
    }

    RawScope raw;
    PasteScope paste;
    size_t cur = buf.size();
    const std::string prompt = "  Enter title: ";
    const int promptW = (int)prompt.size();
    const int blockRows = 7;
    bool first = true;
    int viewStart = 0;

    auto render = [&]() {
        const int cols = std::max(20, TermCols());
        const int bw = std::min(kBannerWidth, cols - 1);
        std::string o;
        if (first) {
            o += ColorEsc(g_theme.prompt) + prompt + ColorEsc(g_theme.input);
            for (int i = 0; i < blockRows; ++i) o += "\n";
            o += "\033[" + ToStr(blockRows) + "A";
            first = false;
        }
        // horizontal scrolling of the input line on narrow terminals
        const int vw = std::max(5, cols - promptW - 1);
        int widthBefore = 0;
        for (size_t i = (size_t)viewStart; i < cur && i < buf.size(); ++i) widthBefore += CharWidth(DisplayChar(buf[i]));
        while (viewStart < (int)cur && widthBefore >= vw) {
            widthBefore -= CharWidth(DisplayChar(buf[(size_t)viewStart]));
            ++viewStart;
        }
        while (viewStart > 0 && viewStart > (int)cur) --viewStart;
        std::string shown;
        int used = 0;
        for (size_t i = (size_t)viewStart; i < buf.size(); ++i) {
            const char32_t g = DisplayChar(buf[i]);
            const int w = CharWidth(g);
            if (used + w > vw) break;
            AppendUtf8(shown, g);
            used += w;
        }
        o += "\r\033[" + ToStr(promptW) + "C" + ColorEsc(g_theme.input) + shown + "\033[K";

        // preview block: blank, border, centered title, border
        std::string titleText;
        for (char32_t c : buf) AppendUtf8(titleText, DisplayChar(c));
        int tw = 0;
        for (char32_t c : buf) tw += CharWidth(DisplayChar(c));
        int left = (bw - tw) / 2;
        if (left < 0) left = 0;
        const std::string border = std::string((size_t)bw, '=');
        o += "\033[2B\r\033[2K" + ColorEsc(g_theme.banner) + border;
        o += "\033[1B\r\033[2K" + ColorEsc(g_theme.header) + std::string((size_t)left, ' ') + FitToWidth(titleText, bw);
        o += "\033[1B\r\033[2K" + ColorEsc(g_theme.banner) + border + ColorEsc(CC_RESET);
        o += "\033[4A\r\033[" + ToStr(promptW + widthBefore) + "C" + ColorEsc(g_theme.input);
        Out(o);
        Flush();
    };
    auto leave = [&]() {
        Out(ColorEsc(CC_RESET) + "\033[" + ToStr(blockRows) + "B\r");
        g_col = 0;
        Flush();
    };

    CursorScope showCursor(false);
    render();
    while (true) {
        const Key k = ReadKey();
        bool changed = true;
        switch (k.code) {
            case K_RESIZE: break;
            case K_EOF: HandleInputEof();
            case K_ENTER: leave(); title = U32ToUtf8(buf); return true;
            case K_ESC: leave(); return false;
            case K_BACKSPACE: if (cur > 0) { buf.erase(cur - 1, 1); --cur; } else changed = false; break;
            case K_DELETE: if (cur < buf.size()) buf.erase(cur, 1); else changed = false; break;
            case K_LEFT: if (cur > 0) --cur; else changed = false; break;
            case K_RIGHT: if (cur < buf.size()) ++cur; else changed = false; break;
            case K_HOME: cur = 0; break;
            case K_END: cur = buf.size(); break;
            case K_PASTE: {
                for (char32_t c : Utf8ToU32(k.text)) {
                    if (c < 0x20 || c == 0x7F) continue;
                    if (buf.size() >= LIMIT) { WriteRaw("\a"); break; }
                    buf.insert(buf.begin() + (std::ptrdiff_t)cur, c);
                    ++cur;
                }
                break;
            }
            case K_CHAR:
                if (k.ctrl) {
                    if (k.ch == 'a') cur = 0;
                    else if (k.ch == 'e') cur = buf.size();
                    else if (k.ch == 'u') { buf.erase(0, cur); cur = 0; }
                    else if (k.ch == 'k') buf.erase(cur);
                    else if (k.ch == 'c') { leave(); Quit(130); }
                    else changed = false;
                } else if (k.alt) {
                    changed = false;
                } else if (buf.size() < LIMIT) {
                    buf.insert(buf.begin() + (std::ptrdiff_t)cur, k.ch);
                    ++cur;
                } else {
                    WriteRaw("\a");
                    changed = false;
                }
                break;
            default: changed = false; break;
        }
        if (changed) render();
    }
}

static std::string OptionFallbackLabel(size_t index) { return "Option " + ToStr((long long)index + 1); }

static void EditDescriptionTUI(std::vector<std::string>& description) {
    EditLinesWithConsoleEditor(description);
}

static void EditCommandsTUI(std::vector<std::string>& commands) {
    EditLinesWithConsoleEditor(commands, true);
}

static int PromptRunMode(int currentMode, bool allowSubmenu = true) {
    if (currentMode < 0 || currentMode > 2) currentMode = 0;

    if (g_enableTUI) {
        std::vector<std::string> choices = {
            "Direct call (runs inside the menu)",
            "bash -c wrapper (RunMode 1)",
        };
        if (allowSubmenu) choices.push_back("Sub menu");
        const int sel = RunTUIMenu("Select Run Mode", choices, currentMode, -1, false);
        if (sel < 0) return currentMode;
        return sel;
    }

    while (true) {
        C_P; Out("  Run mode:\n");
        Out("    0 = Direct call (runs inside the menu)\n");
        Out("    1 = bash -c wrapper\n");
        if (allowSubmenu) Out("    2 = Sub menu\n");
        Printf("  Select [%d]: ", currentMode); C_I;
        const std::string s = Trim(ReadLine()); C_R;
        if (s.empty()) return currentMode;
        const int v = atoi(s.c_str());
        if (s == "0" || s == "1" || (allowSubmenu && s == "2")) return v;
        C_E; Out("  [Error] Invalid run mode.\n"); C_R;
    }
}

static void ConfigureSubmenuInteractive(std::vector<OptionData>& subOptions, int depth = 0) {
    if (depth >= kMaxDepth) {
        C_E; Out("  [Error] Maximum submenu depth reached.\n"); C_R;
        return;
    }

    int optionCount = subOptions.empty() ? 2 : (int)subOptions.size();
    while (true) {
        C_P; Printf("  Number of submenu options (1-%d) [%d]: ", kMaxOptions, optionCount); C_I;
        const std::string s = Trim(ReadOptionCountInput()); C_R;
        if (s.empty()) break;
        const int v = atoi(s.c_str());
        if (v >= 1 && v <= kMaxOptions) { optionCount = v; break; }
        C_E; Printf("  [Error] Enter a value between 1 and %d.\n", kMaxOptions); C_R;
    }

    subOptions.resize((size_t)optionCount);

    for (size_t i = 0; i < subOptions.size(); ++i) {
        C_N; Printf("\n  --- Submenu Option %zu ---\n", i + 1); C_R;

        C_P;
        if (!subOptions[i].label.empty()) Out("  Label [" + subOptions[i].label + "]: ");
        else Out("  Label [" + OptionFallbackLabel(i) + "]: ");
        C_I;
        const std::string label = ReadLine(); C_R;
        if (!label.empty()) subOptions[i].label = label;
        if (subOptions[i].label.empty()) subOptions[i].label = OptionFallbackLabel(i);

        const int newMode = PromptRunMode(subOptions[i].runMode, true);
        subOptions[i].runMode = newMode;

        if (newMode == 2) {
            subOptions[i].commands.clear();
            ConfigureSubmenuInteractive(subOptions[i].subOptions, depth + 1);
            continue;
        }

        subOptions[i].subOptions.clear();
        if (!subOptions[i].commands.empty()) {
            C_S; Out("  Existing command(s):\n");
            for (const auto& cmd : subOptions[i].commands) Out("    > " + cmd + "\n");
            C_R;
        }

        C_P; Out("  Enter command(s), one per line. Empty line when done.\n"); C_R;
        while (true) {
            C_I; Out("    > "); const std::string cmd = ReadLine(); C_R;
            if (cmd.empty()) break;
            subOptions[i].commands.push_back(cmd);
        }
    }
}

// ============================================================
// File browsers: the desktop's own dialog (GNOME / KDE / Xfce / ...) or a built-in terminal browser
// ============================================================
enum DialogKind { DK_OPEN_FILE, DK_SAVE_FILE, DK_DIRECTORY };

static std::string g_lastBrowseDir;

static bool HasGraphicalSession() {
    return !GetEnv("DISPLAY").empty() || !GetEnv("WAYLAND_DISPLAY").empty();
}

// "GNOME", "KDE", "XFCE", ... from XDG_CURRENT_DESKTOP (first entry), or "" when unknown.
static std::string DesktopName() {
    std::string d = GetEnv("XDG_CURRENT_DESKTOP");
    if (d.empty()) d = GetEnv("DESKTOP_SESSION");
    if (d.empty()) return "";
    const size_t colon = d.find(':');
    if (colon != std::string::npos) d = d.substr(0, colon);
    return d;
}

static bool DesktopIs(const std::string& needle) {
    std::string all = ToLower(GetEnv("XDG_CURRENT_DESKTOP") + ":" + GetEnv("DESKTOP_SESSION"));
    return all.find(ToLower(needle)) != std::string::npos;
}

struct BrowserInfo {
    std::string id;          // value stored in settings.txt
    std::string label;       // shown in the Settings menu
    bool installed;
};

static std::vector<BrowserInfo> ListFileBrowsers() {
    std::vector<BrowserInfo> list;
    list.push_back({ "zenity", "Zenity (GNOME, Xfce, Cinnamon, MATE - GTK)", CommandExists("zenity") });
    list.push_back({ "kdialog", "KDialog (KDE Plasma)", CommandExists("kdialog") });
    list.push_back({ "yad", "YAD (GTK)", CommandExists("yad") });
    list.push_back({ "qarma", "Qarma (Qt, LXQt)", CommandExists("qarma") });
    return list;
}

static bool BrowserInstalled(const std::string& id) {
    for (const auto& b : ListFileBrowsers()) if (b.id == id) return b.installed;
    return false;
}

// Decides which file browser to use right now: "zenity", "kdialog", "yad", "qarma" or "builtin".
static std::string ResolveFileBrowser() {
    const std::string wanted = g_appConfig.filebrowser;
    if (wanted == "builtin") return "builtin";
    if (!HasGraphicalSession() || !g_stdinTty) return "builtin";
    if (wanted != "auto" && BrowserInstalled(wanted)) return wanted;

    std::vector<std::string> order;
    if (DesktopIs("kde") || DesktopIs("plasma")) order = { "kdialog", "zenity", "yad", "qarma" };
    else if (DesktopIs("lxqt")) order = { "qarma", "kdialog", "zenity", "yad" };
    else order = { "zenity", "yad", "kdialog", "qarma" };
    for (const auto& id : order) if (BrowserInstalled(id)) return id;
    return "builtin";
}

// ---- built-in terminal browser ----
struct BrowseRow {
    std::string text;
    int type;            // 0 parent, 1 new file name, 2 use this folder, 3 folder, 4 file
    std::string name;
};

static bool CaseInsensitiveLess(const std::string& a, const std::string& b) {
    const std::string la = ToLower(a), lb = ToLower(b);
    if (la != lb) return la < lb;
    return a < b;
}

static std::string ShortenLeft(const std::string& s, int cols) {
    if (cols < 6) return FitToWidth(s, cols);
    if (DisplayWidth(s) <= cols) return s;
    std::u32string u = Utf8ToU32(s);
    std::u32string out;
    int w = 0;
    for (size_t i = u.size(); i > 0; --i) {
        const int cw = CharWidth(DisplayChar(u[i - 1]));
        if (w + cw > cols - 3) break;
        out.insert(out.begin(), u[i - 1]);
        w += cw;
    }
    return "..." + U32ToUtf8(out);
}

static std::string PromptPathManually(const std::string& prompt) {
    C_P; Out(prompt); C_I;
    std::string p = StripQuotes(ReadPathLine());
    C_R;
    return ExpandTilde(p);
}

static std::string BrowseTUI(DialogKind kind, const std::string& title, const std::string& ext,
    const std::string& startDir, const std::string& defaultName) {

    if (!g_stdinTty || !g_stdoutTty) {
        return PromptPathManually("\n  Enter path: ");
    }

    std::string dir = !startDir.empty() && DirExists(startDir) ? AbsolutePath(startDir)
        : (!g_lastBrowseDir.empty() && DirExists(g_lastBrowseDir) ? g_lastBrowseDir : CurrentDir());
    if (dir.empty()) dir = HomeDir();
    dir = TrimTrailingSlashes(dir);

    bool showHidden = false;
    bool showAll = ext.empty();
    std::vector<BrowseRow> rows;
    int cur = 0, top = 0;
    std::string status;

    auto matchesFilter = [&](const std::string& n) {
        if (showAll || ext.empty()) return true;
        return EndsWithNoCase(n, "." + ext);
    };
    auto rebuild = [&](const std::string& selectName) {
        rows.clear();
        if (dir != "/") rows.push_back({ "..  (parent folder)", 0, ".." });
        if (kind == DK_SAVE_FILE) rows.push_back({ "+  New file name...", 1, "" });
        if (kind == DK_DIRECTORY) rows.push_back({ "*  Use this folder", 2, "" });
        std::vector<std::string> dirs, files;
        DIR* d = opendir(dir.c_str());
        if (!d) {
            status = "Cannot read this folder.";
        } else {
            struct dirent* e;
            while ((e = readdir(d)) != nullptr) {
                const std::string n = e->d_name;
                if (n == "." || n == "..") continue;
                if (!showHidden && n[0] == '.') continue;
                if (DirExists(dir + "/" + n)) dirs.push_back(n);
                else if (kind != DK_DIRECTORY && matchesFilter(n)) files.push_back(n);
            }
            closedir(d);
        }
        std::sort(dirs.begin(), dirs.end(), CaseInsensitiveLess);
        std::sort(files.begin(), files.end(), CaseInsensitiveLess);
        for (const auto& n : dirs) rows.push_back({ n + "/", 3, n });
        for (const auto& n : files) rows.push_back({ n, 4, n });
        cur = 0;
        if (!selectName.empty()) {
            for (size_t i = 0; i < rows.size(); ++i) if (rows[i].name == selectName) { cur = (int)i; break; }
        } else {
            // start on the first real entry when there is one
            for (size_t i = 0; i < rows.size(); ++i) if (rows[i].type >= 3) { cur = (int)i; break; }
        }
        top = 0;
    };

    RawScope raw;
    AltScreenScope alt;
    CursorScope hideCursor(true);
    PasteScope paste;
    rebuild("");

    auto draw = [&]() {
        const int cols = std::max(30, TermCols());
        const int rowsAvail = std::max(3, TermRows() - 10);
        if (cur < top) top = cur;
        if (cur >= top + rowsAvail) top = cur - rowsAvail + 1;
        top = std::max(0, std::min(top, std::max(0, (int)rows.size() - rowsAvail)));
        std::string o = "\033[H";
        auto line = [&](const std::string& s) { o += "\033[2K" + s + "\r\n"; };
        const int bw = std::max(8, std::min(44, cols - 3));
        line(ColorEsc(g_theme.banner) + "  " + std::string((size_t)bw, '=') + ColorEsc(CC_RESET));
        {
            const int tw = DisplayWidth(title);
            const int pad = std::max(0, (bw - tw) / 2);
            line(ColorEsc(g_theme.header) + "  " + std::string((size_t)pad, ' ') + FitToWidth(title, bw) + ColorEsc(CC_RESET));
        }
        line(ColorEsc(g_theme.banner) + "  " + std::string((size_t)bw, '=') + ColorEsc(CC_RESET));
        line(ColorEsc(g_theme.prompt) + "  Folder: " + ColorEsc(g_theme.input) + ShortenLeft(dir, cols - 12) + ColorEsc(CC_RESET));
        std::string filt;
        if (kind == DK_DIRECTORY) filt = "folders only";
        else if (ext.empty() || showAll) filt = "all files";
        else filt = "*." + ext + " files";
        line(ColorEsc(g_theme.info) + "  Showing: " + filt + (showHidden ? ", hidden items" : "") + ColorEsc(CC_RESET));
        line("");
        for (int r = 0; r < rowsAvail; ++r) {
            const int i = top + r;
            if (i >= (int)rows.size()) { line(""); continue; }
            const BrowseRow& br = rows[(size_t)i];
            const std::string text = FitToWidth(" " + br.text + " ", cols - 6);
            if (i == cur) {
                line(ColorEsc(CC_RESET) + "    " + InvertedEsc(g_theme.accent, CC_BLACK) + text + ColorEsc(CC_RESET));
            } else {
                int color = g_theme.text;
                if (br.type == 3) color = g_theme.header;
                else if (br.type < 3) color = g_theme.accent;
                line(ColorEsc(color) + "    " + text + ColorEsc(CC_RESET));
            }
        }
        line("");
        line(ColorEsc(g_theme.error) + "  " + FitToWidth(status, cols - 3) + ColorEsc(CC_RESET));
        o += "\033[2K" + ColorEsc(g_theme.text) +
            FitToWidth("  Enter select  Bksp up  / path  ~ home  . hidden  A all  Esc cancel", cols - 1) +
            ColorEsc(CC_RESET) + "\033[J";
        Out(o);
        Flush();
    };

    auto askBottomLine = [&](const std::string& prompt, const std::string& initial, bool pathMode) -> std::string {
        Out("\033[" + ToStr(TermRows()) + ";1H\033[2K" + ColorEsc(g_theme.prompt) + prompt + ColorEsc(g_theme.input) + "\033[?25h");
        g_col = DisplayWidth(prompt);
        ReadLineOptions o;
        o.pathMode = pathMode;
        bool cancelled = false;
        o.cancelled = &cancelled;
        std::string s = ReadLineEx(initial, o);
        Out(ColorEsc(CC_RESET) + "\033[?25l");
        return cancelled ? "" : s;
    };

    auto goUp = [&]() {
        if (dir == "/") return;
        const std::string leaving = BaseName(dir);
        dir = DirName(dir);
        rebuild(leaving);
    };

    while (true) {
        draw();
        const Key k = ReadKey();
        status.clear();
        switch (k.code) {
            case K_RESIZE: break;
            case K_EOF: Quit(0);
            case K_ESC: g_lastBrowseDir = dir; return "";
            case K_UP: if (!rows.empty()) cur = (cur - 1 + (int)rows.size()) % (int)rows.size(); break;
            case K_DOWN: if (!rows.empty()) cur = (cur + 1) % (int)rows.size(); break;
            case K_HOME: cur = 0; break;
            case K_END: cur = std::max(0, (int)rows.size() - 1); break;
            case K_PGUP: cur = std::max(0, cur - std::max(3, TermRows() - 10)); break;
            case K_PGDN: cur = std::min(std::max(0, (int)rows.size() - 1), cur + std::max(3, TermRows() - 10)); break;
            case K_BACKSPACE: goUp(); break;
            case K_ENTER: {
                if (rows.empty()) break;
                const BrowseRow& br = rows[(size_t)cur];
                if (br.type == 0) { goUp(); }
                else if (br.type == 1) {
                    const std::string name = Trim(askBottomLine("  New file name: ", defaultName, false));
                    if (!name.empty()) {
                        g_lastBrowseDir = dir;
                        return name[0] == '/' ? name : JoinPath(dir, name);
                    }
                } else if (br.type == 2) {
                    g_lastBrowseDir = dir;
                    return dir;
                } else if (br.type == 3) {
                    dir = JoinPath(dir, br.name);
                    rebuild("");
                } else {
                    g_lastBrowseDir = dir;
                    return JoinPath(dir, br.name);
                }
                break;
            }
            case K_CHAR:
                if (k.ctrl) {
                    if (k.ch == 'c') { Out("\n"); Quit(130); }
                    break;
                }
                if (k.alt) break;
                if (k.ch == '/') {
                    const std::string typed = StripQuotes(askBottomLine("  Go to / path: ", dir + "/", true));
                    if (typed.empty()) break;
                    const std::string p = ExpandTilde(typed);
                    const std::string full = p[0] == '/' ? p : JoinPath(dir, p);
                    if (DirExists(full)) { dir = TrimTrailingSlashes(AbsolutePath(full)); rebuild(""); }
                    else if (kind == DK_OPEN_FILE && IsRegularFile(full)) { g_lastBrowseDir = DirName(AbsolutePath(full)); return AbsolutePath(full); }
                    else if (kind == DK_SAVE_FILE && DirExists(DirName(full))) { g_lastBrowseDir = DirName(full); return full; }
                    else status = "Not found: " + typed;
                } else if (k.ch == '~') {
                    dir = TrimTrailingSlashes(HomeDir());
                    rebuild("");
                } else if (k.ch == '.') {
                    showHidden = !showHidden;
                    rebuild("");
                } else if (k.ch == 'a' || k.ch == 'A') {
                    if (!ext.empty() && kind != DK_DIRECTORY) { showAll = !showAll; rebuild(""); }
                } else if (k.ch == 'j' && !rows.empty()) {
                    cur = (cur + 1) % (int)rows.size();
                } else if (k.ch == 'k' && !rows.empty()) {
                    cur = (cur - 1 + (int)rows.size()) % (int)rows.size();
                }
                break;
            default: break;
        }
    }
}

// "*.txt *.TXT": GTK and KDE file filters are case-sensitive.
static std::string GlobPatterns(const std::string& ext) {
    const std::string lower = ToLower(ext);
    std::string upper = ext;
    for (auto& ch : upper) ch = (char)std::toupper((unsigned char)ch);
    std::string out = "*." + lower;
    if (upper != lower) out += " *." + upper;
    return out;
}

// ---- desktop dialogs ----
// Returns 0 = chosen (path in 'result'), 1 = cancelled, 2 = the dialog could not be shown.
static int RunGuiDialog(const std::string& tool, DialogKind kind, const std::string& title,
    const std::string& ext, const std::string& startDir, const std::string& defaultName, std::string& result) {

    result.clear();
    const std::string dirSlash = EnsureTrailingSlash(startDir.empty() ? CurrentDir() : startDir);
    std::vector<std::string> argv;

    if (tool == "kdialog") {
        argv = { "kdialog", "--title", title };
        const std::string startPath = kind == DK_SAVE_FILE && !defaultName.empty() ? dirSlash + defaultName : dirSlash;
        std::string filter;
        if (!ext.empty()) filter = GlobPatterns(ext) + "|" + ext + " files (*." + ext + ")\n*|All files";
        if (kind == DK_DIRECTORY) { argv.push_back("--getexistingdirectory"); argv.push_back(dirSlash); }
        else {
            argv.push_back(kind == DK_SAVE_FILE ? "--getsavefilename" : "--getopenfilename");
            argv.push_back(startPath);
            if (!filter.empty()) argv.push_back(filter);
        }
    } else {
        // zenity, yad and qarma share the same command-line syntax
        argv = { tool };
        if (tool == "yad") argv.push_back("--file"); else argv.push_back("--file-selection");
        argv.push_back("--title=" + title);
        if (kind == DK_DIRECTORY) {
            argv.push_back("--directory");
            argv.push_back("--filename=" + dirSlash);
        } else {
            if (kind == DK_SAVE_FILE) {
                argv.push_back("--save");
                argv.push_back("--confirm-overwrite");
                argv.push_back("--filename=" + dirSlash + defaultName);
            } else {
                argv.push_back("--filename=" + dirSlash);
            }
            if (!ext.empty()) {
                argv.push_back("--file-filter=" + ext + " files | " + GlobPatterns(ext));
                argv.push_back("--file-filter=All files | *");
            }
        }
    }

    std::string out;
    ProcOpts po;
    po.out = &out;
    po.quietErr = true;
    po.detachedStdin = true;
    const int rc = RunCommand(argv, po);
    out = Trim(FirstLine(out));
    if (rc == 0 && !out.empty()) {
        // yad can append a separator to the path
        if (tool == "yad" && out.back() == '|') out.pop_back();
        result = out;
        return 0;
    }
    if (rc == 1) return 1;
    return 2;
}

// Asks for one path using the configured file browser. Returns "" when the user cancels.
static std::string ChooseFile(DialogKind kind, const std::string& title, const std::string& ext,
    const std::string& defaultName = "", const std::string& startDir = "") {

    std::string start = startDir;
    if (start.empty()) start = g_lastBrowseDir;
    if (start.empty() || !DirExists(start)) start = CurrentDir();
    if (start == "/") start = HomeDir();

    const std::string tool = ResolveFileBrowser();
    if (tool != "builtin") {
        std::string path;
        const int rc = RunGuiDialog(tool, kind, title, ext, start, defaultName, path);
        if (rc == 0) {
            g_lastBrowseDir = kind == DK_DIRECTORY ? path : DirName(path);
            return path;
        }
        if (rc == 1) return "";
        C_E; Out("\n  [Note] The " + tool + " dialog could not be opened. Using the built-in browser instead.\n"); C_R;
        Sleep(900);
    }
    return BrowseTUI(kind, title, ext, start, defaultName);
}

// ============================================================
// Text editor lookup (used by Text Mode > "Edit in <editor>")
// ============================================================
struct EditorChoice {
    std::string id;                   // value stored in settings.txt
    std::string label;
    std::vector<std::string> argv;    // command + arguments placed before the file name
    bool installed;
};

static std::vector<std::string> SplitWords(const std::string& s) {
    std::vector<std::string> words;
    std::string cur;
    for (char ch : s) {
        if (ch == ' ' || ch == '\t') { if (!cur.empty()) { words.push_back(cur); cur.clear(); } }
        else cur.push_back(ch);
    }
    if (!cur.empty()) words.push_back(cur);
    return words;
}

static std::vector<EditorChoice> ListKnownEditors() {
    std::vector<EditorChoice> list;
    auto add = [&](const std::string& id, const std::string& label, std::vector<std::string> argv) {
        list.push_back({ id, label, argv, CommandExists(argv[0]) });
    };
    add("nano", "nano", { "nano" });
    add("micro", "micro", { "micro" });
    add("vim", "vim", { "vim" });
    add("nvim", "neovim", { "nvim" });
    add("vi", "vi", { "vi" });
    add("emacs", "emacs (terminal)", { "emacs", "-nw" });
    add("kate", "Kate (KDE)", { "kate", "--block" });
    add("mousepad", "Mousepad (Xfce)", { "mousepad", "--disable-server" });
    add("gedit", "gedit (GNOME)", { "gedit", "--wait" });
    add("code", "Visual Studio Code", { "code", "--wait" });
    return list;
}

// Picks the editor to launch. Returns false when none is available.
static bool ResolveEditor(std::vector<std::string>& argv, std::string& displayName) {
    argv.clear();
    const std::string wanted = g_appConfig.editor;
    if (wanted != "auto" && !wanted.empty()) {
        for (const auto& e : ListKnownEditors()) {
            if (e.id == wanted && e.installed) { argv = e.argv; displayName = e.id; return true; }
        }
        const std::vector<std::string> words = SplitWords(wanted);
        if (!words.empty() && CommandExists(words[0])) { argv = words; displayName = BaseName(words[0]); return true; }
    }
    const char* envNames[] = { "VISUAL", "EDITOR" };
    for (const char* name : envNames) {
        const std::vector<std::string> words = SplitWords(GetEnv(name));
        if (!words.empty() && CommandExists(words[0])) { argv = words; displayName = BaseName(words[0]); return true; }
    }
    const char* terminalEditors[] = { "sensible-editor", "editor", "nano", "micro", "vim", "nvim", "vi" };
    for (const char* name : terminalEditors) {
        if (CommandExists(name)) { argv = { name }; displayName = name; return true; }
    }
    if (HasGraphicalSession()) {
        for (const auto& e : ListKnownEditors()) {
            if (e.installed && (e.id == "kate" || e.id == "mousepad" || e.id == "gedit" || e.id == "code")) {
                argv = e.argv; displayName = e.id; return true;
            }
        }
    }
    return false;
}

static std::string AutoEditorName() {
    AppConfig keep = g_appConfig;
    g_appConfig.editor = "auto";
    std::vector<std::string> argv;
    std::string name;
    const bool found = ResolveEditor(argv, name);
    g_appConfig = keep;
    return found ? name : "none found";
}

static std::string EditorSettingText() {
    if (g_appConfig.editor == "auto" || g_appConfig.editor.empty()) return "auto [" + AutoEditorName() + "]";
    return g_appConfig.editor;
}

// Opens a file in the configured editor and waits until it is closed. Returns false when it could not run.
static bool OpenInEditorAndWait(const std::string& path) {
    std::vector<std::string> argv;
    std::string name;
    if (!ResolveEditor(argv, name)) {
        C_E; Out("\n  [Error] No text editor found. Install nano or vim, set $EDITOR, or pick one in Settings.\n"); C_R;
        return false;
    }
    argv.push_back(path);
    C_N; Out("\n  Opening " + name + "... save and close the editor to continue.\n"); C_R;
    Flush();
    const int rc = RunCommand(argv);
    return rc != 127;
}

// ============================================================
// Settings value pickers
// ============================================================
static void RunFileBrowserPicker() {
    std::vector<std::string> ids;
    std::vector<std::string> labels;
    const std::string desktop = DesktopName();
    {
        AppConfig keep = g_appConfig;
        g_appConfig.filebrowser = "auto";
        const std::string autoPick = ResolveFileBrowser();
        g_appConfig = keep;
        ids.push_back("auto");
        labels.push_back("Auto (recommended)  ->  " + (autoPick == "builtin" ? std::string("terminal browser") : autoPick));
    }
    for (const auto& b : ListFileBrowsers()) {
        ids.push_back(b.id);
        labels.push_back(b.label + (b.installed ? "" : "  [not installed]"));
    }
    ids.push_back("builtin");
    labels.push_back("Built-in terminal browser (no desktop needed)");

    int current = 0;
    for (size_t i = 0; i < ids.size(); ++i) if (ids[i] == g_appConfig.filebrowser) current = (int)i;

    std::string title = "File Browser";
    if (!desktop.empty()) title += " (desktop: " + desktop + ")";
    int sel = -1;
    if (g_enableTUI) {
        sel = RunTUIMenu(title, labels, current, current, true);
    } else {
        ClearScreen();
        C_B; Out(BannerRule() + "\n"); C_N; Out(BannerTitle(title) + "\n"); C_B; Out(BannerRule() + "\n\n"); C_R;
        for (size_t i = 0; i < labels.size(); ++i) { C_P; Printf("    %zu) ", i + 1); C_T; Out(labels[i] + (ids[i] == g_appConfig.filebrowser ? "  (Selected)" : "") + "\n"); }
        C_P; Printf("\n  Select [%d]: ", current + 1); C_I;
        const std::string a = Trim(ReadLine()); C_R;
        if (a.empty()) sel = current;
        else { const int n = atoi(a.c_str()); if (n >= 1 && n <= (int)ids.size()) sel = n - 1; }
    }
    if (sel < 0) return;
    if (ids[(size_t)sel] != "auto" && ids[(size_t)sel] != "builtin" && !BrowserInstalled(ids[(size_t)sel])) {
        C_E; Out("\n  That file browser is not installed on this system. Install it first (for example with your package manager).\n"); C_R;
        PressAnyKey();
        return;
    }
    g_appConfig.filebrowser = ids[(size_t)sel];
}

static void RunEditorPicker() {
    std::vector<std::string> ids;
    std::vector<std::string> labels;
    ids.push_back("auto");
    labels.push_back("Auto  ->  " + AutoEditorName() + "  ($VISUAL, $EDITOR, nano, vim, ...)");
    for (const auto& e : ListKnownEditors()) {
        if (!e.installed) continue;
        ids.push_back(e.id);
        labels.push_back(e.label);
    }
    ids.push_back("custom");
    labels.push_back("Custom command...");

    int current = 0;
    for (size_t i = 0; i < ids.size(); ++i) if (ids[i] == g_appConfig.editor) current = (int)i;
    if (current == 0 && g_appConfig.editor != "auto") current = (int)ids.size() - 1;

    int sel = -1;
    if (g_enableTUI) {
        sel = RunTUIMenu("Text Editor", labels, current, current, true);
    } else {
        ClearScreen();
        C_B; Out(BannerRule() + "\n"); C_N; Out(BannerTitle("Text Editor") + "\n"); C_B; Out(BannerRule() + "\n\n"); C_R;
        for (size_t i = 0; i < labels.size(); ++i) { C_P; Printf("    %zu) ", i + 1); C_T; Out(labels[i] + "\n"); }
        C_P; Printf("\n  Select [%d]: ", current + 1); C_I;
        const std::string a = Trim(ReadLine()); C_R;
        if (a.empty()) sel = current;
        else { const int n = atoi(a.c_str()); if (n >= 1 && n <= (int)ids.size()) sel = n - 1; }
    }
    if (sel < 0) return;
    if (ids[(size_t)sel] == "custom") {
        C_P; Out("\n  Editor command (for example: nano -l): "); C_I;
        const std::string cmd = Trim(ReadLine()); C_R;
        const std::vector<std::string> words = SplitWords(cmd);
        if (words.empty()) return;
        if (!CommandExists(words[0])) {
            C_E; Out("  [Error] Command not found: " + words[0] + "\n"); C_R;
            PressAnyKey();
            return;
        }
        g_appConfig.editor = cmd;
    } else {
        g_appConfig.editor = ids[(size_t)sel];
    }
}

// ============================================================
// Program setup: move / shortcuts / the "bmg" command
// ============================================================
// The icon is stored as base64 PNG data at the end of this file (see "Embedded program icon").
static std::vector<std::pair<int, std::string>> DecodeEmbeddedIcons();

static const char* kDesktopMarker = "X-BashMenuGenerator-Managed=true";
static const char* kProgramIconName = "BashMenuGenerator";
static const char* kPathBlockStart = "# >>> BashMenuGenerator (managed) >>>";
static const char* kPathBlockEnd = "# <<< BashMenuGenerator (managed) <<<";

static std::string InstalledProgramPath() { return GetAppDataDir() + "BashMenuGenerator"; }

static bool ContainsText(const std::string& path, const std::string& needle) {
    std::string data;
    return ReadBinaryFile(path, data) && data.find(needle) != std::string::npos;
}

static bool DesktopSafePath(const std::string& p) {
    return p.find_first_of("\"`$\\%\n") == std::string::npos;
}

static std::string GetIconThemeDir() {
    std::string base = GetEnv("XDG_DATA_HOME");
    if (base.empty() || base[0] != '/') base = HomeDir() + "/.local/share";
    return EnsureTrailingSlash(base) + "icons/hicolor/";
}

// Installs the embedded program icon (every size) into the user's icon theme, the way any desktop
// application does, so launchers can refer to it by name. Returns that name, or "" when nothing
// could be installed (the launcher then falls back to a generic terminal icon).
static std::string EnsureProgramIconInstalled() {
    const std::string themeDir = GetIconThemeDir();
    int available = 0;
    bool changed = false;
    for (const auto& icon : DecodeEmbeddedIcons()) {
        const std::string dir = themeDir + ToStr(icon.first) + "x" + ToStr(icon.first) + "/apps/";
        const std::string path = dir + kProgramIconName + ".png";
        std::string existing;
        if (ReadBinaryFile(path, existing) && existing == icon.second) { ++available; continue; }
        if (!EnsureDirectoryTree(dir, 0755)) continue;
        if (WriteBinaryFile(path, icon.second, 0644)) { ++available; changed = true; }
    }
    if (available == 0) return "";
    // GTK keeps an optional cache; it can only be refreshed when the theme has an index.theme.
    if (changed && CommandExists("gtk-update-icon-cache") && PathExists(themeDir + "index.theme")) {
        ProcOpts o;
        o.quietErr = true;
        o.quietOut = true;
        RunCommand({ "gtk-update-icon-cache", "-f", "-t", "-q", TrimTrailingSlashes(themeDir) }, o);
    }
    return kProgramIconName;
}

static std::string BuildDesktopEntry(const std::string& target, const std::string& icon) {
    return std::string("[Desktop Entry]\n") +
        "Type=Application\n" +
        "Version=1.0\n" +
        "Name=BashMenuGenerator\n" +
        "Comment=Create bash menu scripts\n" +
        "Exec=\"" + target + "\"\n" +
        "Icon=" + (icon.empty() ? std::string("utilities-terminal") : icon) + "\n" +
        "Terminal=true\n" +
        "Categories=Utility;Development;\n" +
        "Keywords=bash;menu;generator;script;\n" +
        kDesktopMarker + "\n";
}

static std::string GetShortcutPath(bool desktop) {
    if (desktop) {
        const std::string folder = GetDesktopFolder();
        return folder.empty() ? "" : EnsureTrailingSlash(folder) + "BashMenuGenerator.desktop";
    }
    return GetApplicationsDir() + "BashMenuGenerator.desktop";
}

static bool CreateOrUpdateShortcut(const std::string& shortcutPath, const std::string& target) {
    if (shortcutPath.empty() || target.empty() || !DesktopSafePath(target)) return false;
    if (PathExists(shortcutPath) && !ContainsText(shortcutPath, kDesktopMarker)) return false;   // not ours: leave it
    EnsureDirectoryTree(DirName(shortcutPath), 0755);
    if (!WriteBinaryFile(shortcutPath, BuildDesktopEntry(target, EnsureProgramIconInstalled()), 0755)) return false;
    if (CommandExists("gio")) {
        // GNOME only launches a desktop file from the desktop when it is marked as trusted.
        ProcOpts o;
        o.quietErr = true;
        o.quietOut = true;
        RunCommand({ "gio", "set", shortcutPath, "metadata::trusted", "true" }, o);
    }
    return true;
}

static bool CreateProgramShortcut(bool desktop) {
    const std::string path = GetShortcutPath(desktop);
    if (!CreateOrUpdateShortcut(path, GetExecutablePath())) return false;
    if (!desktop && CommandExists("update-desktop-database")) {
        ProcOpts o;
        o.quietErr = true;
        o.quietOut = true;
        RunCommand({ "update-desktop-database", GetApplicationsDir() }, o);
    }
    return true;
}

static bool UpdateShortcutIfPresent(bool desktop, const std::string& targetPath) {
    const std::string path = GetShortcutPath(desktop);
    if (path.empty() || !PathExists(path)) return true;
    if (!ContainsText(path, kDesktopMarker)) return true;
    return CreateOrUpdateShortcut(path, targetPath);
}

// ---- the "bmg" command: ~/.local/bin/bmg -> the program ----
static std::string BmgCommandPath() { return GetUserBinDir() + "bmg"; }

static std::string ReadLinkTarget(const std::string& path) {
    char buf[PATH_MAX];
    const ssize_t n = readlink(path.c_str(), buf, sizeof(buf) - 1);
    if (n < 0) return "";
    buf[n] = '\0';
    return buf;
}

// A bmg link is "ours" when it points at this program, at the installed copy, at something called
// *MenuGenerator*, or at a program that no longer exists.
static bool IsManagedBmgLink(const std::string& path) {
    if (!IsSymlink(path)) return false;
    const std::string target = ReadLinkTarget(path);
    if (!PathExists(target)) return true;
    const std::string resolved = AbsolutePath(target);
    if (resolved == AbsolutePath(GetExecutablePath())) return true;
    if (resolved == AbsolutePath(InstalledProgramPath())) return true;
    return ToLower(BaseName(target)).find("menugenerator") != std::string::npos;
}

static bool BmgCommandsAreInstalled() { return IsSymlink(BmgCommandPath()); }

static bool PathHasUserBin() {
    const std::string want = TrimTrailingSlashes(GetUserBinDir());
    size_t start = 0;
    const std::string path = GetEnv("PATH");
    while (start <= path.size()) {
        size_t end = path.find(':', start);
        if (end == std::string::npos) end = path.size();
        if (TrimTrailingSlashes(path.substr(start, end - start)) == want) return true;
        start = end + 1;
    }
    return false;
}

static std::vector<std::string> ShellStartupFiles(bool forWriting) {
    std::vector<std::string> files;
    const std::string home = EnsureTrailingSlash(HomeDir());
    const bool hasBashProfile = PathExists(home + ".bash_profile") || PathExists(home + ".bash_login");
    if (forWriting) {
        if (PathExists(home + ".profile") || !hasBashProfile) files.push_back(home + ".profile");
    } else if (PathExists(home + ".profile")) {
        files.push_back(home + ".profile");
    }
    if (PathExists(home + ".bash_profile")) files.push_back(home + ".bash_profile");
    if (PathExists(home + ".bashrc")) files.push_back(home + ".bashrc");
    if (PathExists(home + ".zshrc")) files.push_back(home + ".zshrc");
    if (PathExists(home + ".zprofile")) files.push_back(home + ".zprofile");
    return files;
}

static std::string FishConfigPath() { return HomeDir() + "/.config/fish/conf.d/BashMenuGenerator.fish"; }

static std::string PathBlockText(bool fish) {
    if (fish) {
        return std::string(kPathBlockStart) + "\n" +
            "contains $HOME/.local/bin $PATH; or set -gx PATH $HOME/.local/bin $PATH\n" +
            kPathBlockEnd + "\n";
    }
    return std::string(kPathBlockStart) + "\n" +
        "case \":$PATH:\" in *\":$HOME/.local/bin:\"*) ;; *) export PATH=\"$HOME/.local/bin:$PATH\" ;; esac\n" +
        kPathBlockEnd + "\n";
}

static bool FileHasPathBlock(const std::string& path) { return ContainsText(path, kPathBlockStart); }

static bool AddPathBlock(const std::string& path, bool fish) {
    if (FileHasPathBlock(path)) return true;
    std::string data;
    ReadBinaryFile(path, data);
    if (!data.empty() && data.back() != '\n') data += "\n";
    data += "\n" + PathBlockText(fish);
    EnsureDirectoryTree(DirName(path), 0755);
    struct stat st;
    mode_t mode = 0644;
    if (stat(path.c_str(), &st) == 0) mode = st.st_mode & 0777;
    return WriteBinaryFile(path, data, mode);
}

static bool RemovePathBlock(const std::string& path) {
    std::string data;
    if (!ReadBinaryFile(path, data)) return true;
    const size_t s = data.find(kPathBlockStart);
    if (s == std::string::npos) return true;
    size_t e = data.find(kPathBlockEnd, s);
    if (e == std::string::npos) return false;
    e += strlen(kPathBlockEnd);
    if (e < data.size() && data[e] == '\n') ++e;
    size_t from = s;
    if (from > 0 && data[from - 1] == '\n' && (from == 1 || data[from - 2] == '\n')) --from;   // the blank line we added
    data.erase(from, e - from);
    struct stat st;
    mode_t mode = 0644;
    if (stat(path.c_str(), &st) == 0) mode = st.st_mode & 0777;
    return WriteBinaryFile(path, data, mode);
}

static bool InstallOrUpdateBmgCommands(const std::string& targetPath, std::string& message) {
    if (targetPath.empty()) { message = "Could not resolve the current program location."; return false; }
    const std::string link = BmgCommandPath();
    if (!EnsureDirectoryTree(GetUserBinDir(), 0755)) {
        message = "Could not create " + GetUserBinDir() + ".";
        return false;
    }
    if (PathExists(link) || IsSymlink(link)) {
        if (!IsManagedBmgLink(link)) {
            message = "A different bmg command already exists in " + GetUserBinDir() + "; it was left untouched.";
            return false;
        }
        unlink(link.c_str());
    }
    if (symlink(targetPath.c_str(), link.c_str()) != 0) {
        message = "Could not create the bmg link in " + GetUserBinDir() + ".";
        return false;
    }

    bool pathChanged = false;
    if (!PathHasUserBin()) {
        for (const auto& f : ShellStartupFiles(true)) {
            if (!FileHasPathBlock(f) && AddPathBlock(f, false)) pathChanged = true;
        }
        if (DirExists(HomeDir() + "/.config/fish")) {
            if (!FileHasPathBlock(FishConfigPath()) && AddPathBlock(FishConfigPath(), true)) pathChanged = true;
        }
    }
    if (PathHasUserBin()) message = "The bmg command is installed. Type bmg in any terminal.";
    else if (pathChanged) message = "The bmg command is installed and ~/.local/bin was added to your PATH. Open a new terminal to use bmg.";
    else message = "The bmg link was created in ~/.local/bin, but that folder is not on your PATH. Add it to your shell profile to use bmg.";
    return true;
}

static bool UninstallBmgCommands(std::string& message) {
    const std::string link = BmgCommandPath();
    bool ok = true;
    if (IsSymlink(link)) {
        if (IsManagedBmgLink(link)) { if (unlink(link.c_str()) != 0) ok = false; }
        else { message = "A bmg link that was not created by BashMenuGenerator was found and left untouched."; return false; }
    } else if (PathExists(link)) {
        message = "A bmg file that was not created by BashMenuGenerator was found and left untouched.";
        return false;
    }
    for (const auto& f : ShellStartupFiles(false)) if (!RemovePathBlock(f)) ok = false;
    if (PathExists(FishConfigPath()) && FileHasPathBlock(FishConfigPath())) {
        if (unlink(FishConfigPath().c_str()) != 0) ok = false;
    }
    message = ok ? "The bmg command link and the PATH entry added by BashMenuGenerator were removed."
                 : "Some parts of the bmg command could not be removed.";
    return ok;
}

// Copies the running program into the data folder, restarts from there and removes the original.
static bool MoveProgramToDataDir(std::string& message) {
    const std::string source = GetExecutablePath();
    const std::string target = InstalledProgramPath();
    if (source.empty()) { message = "Could not resolve the current program location."; return false; }
    if (AbsolutePath(source) == AbsolutePath(target)) {
        message = "BashMenuGenerator is already in its data folder.";
        return true;
    }
    std::string data;
    if (!ReadBinaryFile(source, data)) { message = "Could not read the running program."; return false; }
    const std::string temp = target + ".move." + ToStr((long long)getpid()) + ".tmp";
    if (!WriteBinaryFile(temp, data, 0755) || rename(temp.c_str(), target.c_str()) != 0) {
        unlink(temp.c_str());
        message = "Could not place the program in " + GetAppDataDir() + ".";
        return false;
    }
    if (!UpdateShortcutIfPresent(true, target) || !UpdateShortcutIfPresent(false, target)) {
        message = "The new copy was created, but an existing shortcut could not be updated. The original program remains in place.";
        return false;
    }
    if (BmgCommandsAreInstalled() && IsManagedBmgLink(BmgCommandPath())) {
        std::string updateMessage;
        if (!InstallOrUpdateBmgCommands(target, updateMessage)) {
            message = "The new copy was created, but the existing bmg command link could not be updated. The original program remains in place.";
            return false;
        }
    }
    const bool removed = unlink(source.c_str()) == 0;
    setenv("BMG_FINISH_MOVE", removed ? "removed" : source.c_str(), 1);
    Flush();
    TermResetAll();
    char* args[] = { const_cast<char*>(target.c_str()), nullptr };
    execv(target.c_str(), args);
    // exec failed: nothing was lost, the original is gone only if it could be unlinked
    unsetenv("BMG_FINISH_MOVE");
    message = removed ? "The program was copied to " + target + " but could not be restarted. Run it from there."
                      : "The program was copied to " + target + " but could not be restarted.";
    return false;
}

static void ShowProgramSetupTips() {
    ClearScreen();
    C_H; Out("  Program Setup Tips\n\n"); C_R;
    C_T; Out("  BashMenuGenerator is portable. If you move it after creating launchers or\n");
    Out("  installing the bmg command, they can break and stop working.\n");
    Out("  Keep the program in one location, or use \"Move program\" first so that\n");
    Out("  everything points at the final place.\n\n");
    Out("  The bmg command is a link in ~/.local/bin. Settings live in\n");
    Out("  ~/.config/BashMenuGenerator and working files in ~/.local/share/BashMenuGenerator.\n"); C_R;
    PressAnyKey("Press any key to return...");
}

static void ShowProgramSetupMessage(const std::string& message, bool success) {
    ClearScreen();
    if (success) { C_S; } else { C_E; }
    Out("  " + message + "\n"); C_R;
    PressAnyKey("Press any key to return...");
}

static void RunProgramSetupMenu() {
    const std::vector<std::string> actions = {
        "Move program to the data folder",
        "Create desktop shortcut",
        "Add program to the applications menu (Search)",
        "Install BMG commands (symlink)",
        "Update BMG commands (symlink)",
        "Uninstall BMG commands (symlink)",
        "Tips",
        "Back"
    };
    int lastChoice = 0;
    while (true) {
        int choice = -1;
        if (g_enableTUI) {
            choice = RunTUIMenu("Set Up Program", actions, lastChoice);
        } else {
            ClearScreen();
            C_B; Out(BannerRule() + "\n"); C_N;
            Out(BannerTitle("SET UP PROGRAM") + "\n"); C_B;
            Out(BannerRule() + "\n\n"); C_R;
            for (size_t i = 0; i + 1 < actions.size(); ++i) { C_P; Printf("    %zu) ", i + 1); C_T; Out(actions[i] + "\n"); }
            C_P; Out("    0) Back\n\n  Select option: "); C_I;
            const int typed = atoi(Trim(ReadLine()).c_str()); C_R;
            if (typed == 0) return;
            choice = typed - 1;
        }
        if (choice < 0 || choice >= 7) return;
        lastChoice = choice;

        if (choice == 6) { ShowProgramSetupTips(); continue; }

        std::string message;
        bool success = false;
        if (choice == 0) {
            success = MoveProgramToDataDir(message);
        } else if (choice == 1) {
            if (GetShortcutPath(true).empty()) message = "No desktop folder was found.";
            else {
                success = CreateProgramShortcut(true);
                message = success ? "Desktop shortcut created:\n  " + GetShortcutPath(true)
                                  : "Could not create the desktop shortcut (a different file with that name may exist, or the program path has special characters).";
            }
        } else if (choice == 2) {
            success = CreateProgramShortcut(false);
            message = success ? "Added to the applications menu. Search for BashMenuGenerator."
                              : "Could not add the program to the applications menu.";
        } else if (choice == 3 || choice == 4) {
            success = InstallOrUpdateBmgCommands(GetExecutablePath(), message);
        } else if (choice == 5) {
            success = UninstallBmgCommands(message);
        }
        if (message.empty()) message = success ? "Setup action completed." : "Setup action failed.";
        ShowProgramSetupMessage(message, success);
    }
}

// ============================================================
// Settings menu (bconfig)
// ============================================================
static const char* const kThemePresets[] = { "default", "matrix", "cyberpunk", "cli", "violent" };

static std::string OnOffText(bool v) { return v ? "1 [Enabled]" : "0 [Disabled]"; }

static std::string FileBrowserSettingText() {
    if (g_appConfig.filebrowser == "auto") {
        const std::string pick = ResolveFileBrowser();
        return "auto [" + (pick == "builtin" ? std::string("terminal") : pick) + "]";
    }
    if (g_appConfig.filebrowser == "builtin") return "builtin [terminal]";
    return g_appConfig.filebrowser + (BrowserInstalled(g_appConfig.filebrowser) ? "" : " [not installed]");
}

static void RunSettingsMenu() {
    const AppConfig originalConfig = g_appConfig;
    const bool originalTUI = g_enableTUI;
    const bool originalSound = g_enableSound;
    bool settingsSaved = false;

    const char* endStr[] = { "0 [Prompt]", "1 [Main Menu]", "2 [Launch Script]", "3 [Exit]" };
    const char* saveloadStr[] = { "0 [Prompt]", "1 [File Browser]", "2 [Manual Path]" };
    const char* borderStr[] = { "0 [Prompt]", "1 [Classic =]", "2 [Modern -]", "3 [ZigZag Z]" };

    auto cyclePreset = [&]() {
        for (int i = 0; i < 5; ++i) {
            if (g_appConfig.theme_preset == kThemePresets[i]) {
                g_appConfig.theme_preset = kThemePresets[(i + 1) % 5];
                ApplyTheme(g_appConfig.theme_preset);
                break;
            }
        }
    };
    auto toggleEngine = [&]() {
        g_appConfig.theme_engine = !g_appConfig.theme_engine;
        if (!g_appConfig.theme_engine) ApplyTheme("default");
        else ApplyTheme(g_appConfig.theme_preset);
    };
    auto saveNow = [&]() -> bool {
        if (SaveSettingsFile(g_appConfig)) {
            settingsSaved = true;
            C_S; Out("\n\n  [SUCCESS] Settings saved!\n"); C_R;
            Sleep(800);
            return true;
        }
        C_E; Out("\n\n  [ERROR] Could not save settings to disk. Changes were not committed.\n"); C_R;
        Sleep(1000);
        return false;
    };

    if (g_enableTUI) {
        int cursor = 0;
        while (true) {
            std::vector<std::string> opts;
            std::vector<int> mapping;
            opts.push_back("Theme Engine : " + OnOffText(g_appConfig.theme_engine)); mapping.push_back(0);
            if (g_appConfig.theme_engine) { opts.push_back("Theme Preset : " + g_appConfig.theme_preset); mapping.push_back(1); }
            opts.push_back("AutoSave     : " + OnOffText(g_appConfig.autosave)); mapping.push_back(2);
            opts.push_back("TUI Mode     : " + OnOffText(g_enableTUI)); mapping.push_back(3);
            opts.push_back("Menu Sounds  : " + OnOffText(g_enableSound)); mapping.push_back(4);
            opts.push_back(std::string("End Action   : ") + endStr[g_appConfig.end]); mapping.push_back(5);
            opts.push_back(std::string("Saveload     : ") + saveloadStr[g_appConfig.saveload]); mapping.push_back(6);
            opts.push_back(std::string("Menuborder   : ") + borderStr[g_appConfig.menuborder]); mapping.push_back(7);
            opts.push_back("File Browser : " + FileBrowserSettingText()); mapping.push_back(8);
            opts.push_back("Text Editor  : " + EditorSettingText()); mapping.push_back(9);
            opts.push_back("Set Up Program"); mapping.push_back(10);
            opts.push_back("[ Save & Exit ]"); mapping.push_back(11);
            opts.push_back("[ Cancel ]"); mapping.push_back(12);

            cursor = RunTUIMenu("SETTINGS (bconfig)", opts, cursor);
            if (cursor == -1) break;
            const int action = mapping[(size_t)cursor];

            if (action == 12) break;
            else if (action == 0) toggleEngine();
            else if (action == 1) cyclePreset();
            else if (action == 2) g_appConfig.autosave = !g_appConfig.autosave;
            else if (action == 3) { g_enableTUI = !g_enableTUI; g_appConfig.tui = g_enableTUI ? 1 : 0; }
            else if (action == 4) { g_enableSound = !g_enableSound; g_appConfig.sound = g_enableSound ? 1 : 0; }
            else if (action == 5) g_appConfig.end = (g_appConfig.end + 1) % 4;
            else if (action == 6) g_appConfig.saveload = (g_appConfig.saveload + 1) % 3;
            else if (action == 7) g_appConfig.menuborder = (g_appConfig.menuborder + 1) % 4;
            else if (action == 8) RunFileBrowserPicker();
            else if (action == 9) RunEditorPicker();
            else if (action == 10) RunProgramSetupMenu();
            else if (action == 11) { if (saveNow()) break; }
        }
    } else {
        while (true) {
            ClearScreen();
            C_B; Out(BannerRule() + "\n"); C_N;
            Out(BannerTitle("SETTINGS (bconfig)") + "\n"); C_B;
            Out(BannerRule() + "\n\n"); C_T;

            C_P; Out("    1) Theme Engine : "); C_I; Out(OnOffText(g_appConfig.theme_engine) + "\n");
            if (g_appConfig.theme_engine) { C_P; Out("    2) Theme Preset : "); C_I; Out(g_appConfig.theme_preset + "\n"); }
            else { C_T; Out("       (Theme Preset hidden while Theme Engine is disabled)\n"); }
            C_P; Out("    3) AutoSave     : "); C_I; Out(OnOffText(g_appConfig.autosave) + "\n");
            C_P; Out("    4) TUI Mode     : "); C_I; Out(OnOffText(g_enableTUI) + "\n");
            C_P; Out("    5) Menu Sounds  : "); C_I; Out(OnOffText(g_enableSound) + "\n");
            C_P; Out("    6) End Action   : "); C_I; Out(std::string(endStr[g_appConfig.end]) + "\n");
            C_P; Out("    7) Saveload     : "); C_I; Out(std::string(saveloadStr[g_appConfig.saveload]) + "\n");
            C_P; Out("    8) Menuborder   : "); C_I; Out(std::string(borderStr[g_appConfig.menuborder]) + "\n");
            C_P; Out("    9) File Browser : "); C_I; Out(FileBrowserSettingText() + "\n");
            C_P; Out("    E) Text Editor  : "); C_I; Out(EditorSettingText() + "\n");
            C_P; Out("\n    P) Set Up Program\n");
            C_P; Out("\n    S) Save & Exit\n    0) Cancel\n\n  Select option: "); C_I;
            Flush();

            Key k;
            {
                RawScope raw;
                do { k = ReadKey(); } while (k.code == K_RESIZE || k.code == K_PASTE);
            }
            C_T;
            if (k.code == K_EOF) HandleInputEof();
            if (k.code == K_ESC) break;
            if (k.code != K_CHAR) continue;
            if (k.ctrl && k.ch == 'c') { Out("\n"); Quit(130); }
            const char ch = (char)std::tolower((int)k.ch);
            if (ch == '1') toggleEngine();
            else if (ch == '2' && g_appConfig.theme_engine) cyclePreset();
            else if (ch == '3') g_appConfig.autosave = !g_appConfig.autosave;
            else if (ch == '4') { g_enableTUI = !g_enableTUI; g_appConfig.tui = g_enableTUI ? 1 : 0; }
            else if (ch == '5') { g_enableSound = !g_enableSound; g_appConfig.sound = g_enableSound ? 1 : 0; }
            else if (ch == '6') g_appConfig.end = (g_appConfig.end + 1) % 4;
            else if (ch == '7') g_appConfig.saveload = (g_appConfig.saveload + 1) % 3;
            else if (ch == '8') g_appConfig.menuborder = (g_appConfig.menuborder + 1) % 4;
            else if (ch == '9') RunFileBrowserPicker();
            else if (ch == 'e') RunEditorPicker();
            else if (ch == 'p') RunProgramSetupMenu();
            else if (ch == 's') { if (saveNow()) break; }
            else if (ch == '0') break;
        }
    }

    // Cancel never leaves temporary setting changes active in this process.
    if (!settingsSaved) {
        g_appConfig = originalConfig;
        g_enableTUI = originalTUI;
        g_enableSound = originalSound;
        if (g_appConfig.theme_engine) ApplyTheme(g_appConfig.theme_preset);
        else ApplyTheme("default");
    }
    ClearScreen();
}

// ============================================================
// AutoSave Engine
// ============================================================
static std::string GetAutoSavePath() { return GetAppDataDir() + "autosave.txt"; }

static std::string ReadAutoSaveTimestamp(const std::string& path) {
    std::string data;
    if (!PathExists(path)) return "";
    if (!ReadBinaryFile(path, data)) return "unknown date";
    const std::vector<std::string> lines = SplitLines(data);
    if (!lines.empty()) {
        const std::string prefix = "// Autosave from ";
        if (lines[0].compare(0, prefix.size(), prefix) == 0) return Trim(lines[0].substr(prefix.size()));
    }
    return "unknown date";
}

static void WriteAutoSave(const std::vector<std::string>& lines) {
    std::string data;
    for (const auto& line : lines) data += line + "\n";
    WriteBinaryFile(GetAutoSavePath(), data, 0600);
}

static std::vector<std::string> ReadAutoSave(const std::string& path) {
    std::vector<std::string> result;
    if (path.empty()) return result;
    std::string data;
    if (!ReadBinaryFile(path, data)) return result;
    return SplitLines(data);
}

static void DeleteAutoSave(const std::string& path) {
    if (!path.empty()) unlink(path.c_str());
}

// ============================================================
// Password records (salted SHA-256, never stored as plain text)
// ============================================================
struct Sha256Ctx {
    uint32_t h[8];
    unsigned char buf[64];
    size_t bufLen;
    uint64_t total;
};

static uint32_t Rotr32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void Sha256Block(Sha256Ctx& c, const unsigned char* p) {
    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) | ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = Rotr32(w[i - 15], 7) ^ Rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = Rotr32(w[i - 2], 17) ^ Rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = c.h[0], b = c.h[1], cc = c.h[2], d = c.h[3], e = c.h[4], f = c.h[5], g = c.h[6], h = c.h[7];
    for (int i = 0; i < 64; ++i) {
        const uint32_t S1 = Rotr32(e, 6) ^ Rotr32(e, 11) ^ Rotr32(e, 25);
        const uint32_t ch = (e & f) ^ (~e & g);
        const uint32_t t1 = h + S1 + ch + K[i] + w[i];
        const uint32_t S0 = Rotr32(a, 2) ^ Rotr32(a, 13) ^ Rotr32(a, 22);
        const uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        const uint32_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c.h[0] += a; c.h[1] += b; c.h[2] += cc; c.h[3] += d; c.h[4] += e; c.h[5] += f; c.h[6] += g; c.h[7] += h;
}

static std::string Sha256Hex(const std::string& data) {
    Sha256Ctx c;
    const uint32_t init[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    memcpy(c.h, init, sizeof(init));
    c.bufLen = 0;
    c.total = 0;
    const unsigned char* p = (const unsigned char*)data.data();
    size_t n = data.size();
    c.total = n;
    while (n >= 64) { Sha256Block(c, p); p += 64; n -= 64; }
    memcpy(c.buf, p, n);
    c.bufLen = n;
    c.buf[c.bufLen++] = 0x80;
    if (c.bufLen > 56) {
        while (c.bufLen < 64) c.buf[c.bufLen++] = 0;
        Sha256Block(c, c.buf);
        c.bufLen = 0;
    }
    while (c.bufLen < 56) c.buf[c.bufLen++] = 0;
    const uint64_t bits = c.total * 8;
    for (int i = 7; i >= 0; --i) c.buf[c.bufLen++] = (unsigned char)((bits >> (i * 8)) & 0xFF);
    Sha256Block(c, c.buf);
    unsigned char out[32];
    for (int i = 0; i < 8; ++i) {
        out[i * 4] = (unsigned char)(c.h[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(c.h[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(c.h[i] >> 8);
        out[i * 4 + 3] = (unsigned char)c.h[i];
    }
    return HexOf(out, 32);
}

static bool IsHexString(const std::string& s) {
    if (s.empty()) return false;
    for (char ch : s) if (!std::isxdigit((unsigned char)ch)) return false;
    return true;
}

// "sha256:<32 hex salt>:<64 hex hash>"
static bool IsPasswordRecord(const std::string& s) {
    if (s.size() != 7 + 32 + 1 + 64 || s.compare(0, 7, "sha256:") != 0 || s[39] != ':') return false;
    return IsHexString(s.substr(7, 32)) && IsHexString(s.substr(40));
}

static std::string MakePasswordRecord(const std::string& plain) {
    const std::string salt = RandomHex(16);
    return "sha256:" + salt + ":" + Sha256Hex(salt + plain);
}

// Empty stays empty, a record is kept, anything else is hashed with a fresh salt.
static std::string NormalizePassword(const std::string& p) {
    if (p.empty()) return "";
    return IsPasswordRecord(p) ? p : MakePasswordRecord(p);
}

static std::string PasswordDisplay(const std::string& p) { return p.empty() ? "(None)" : "***** [Protected]"; }

// ============================================================
// Bash Script Generator
// ============================================================
static std::string BashRuntimeHelpers() {
    std::string s;
    s += R"BMG(# ---------------------------------------------------------------------------
# Runtime - the same in every script made by BashMenuGenerator
# ---------------------------------------------------------------------------
bmg_clear() { if [ -t 1 ]; then printf '\033[H\033[2J'; fi; }
bmg_hr() { printf '%s\n' "$BMG_BORDER"; }
bmg_center() {
    local text="$1" pad
    pad=$(( (BMG_WIDTH - ${#text}) / 2 ))
    if [ "$pad" -lt 0 ]; then pad=0; fi
    printf '%*s%s\n' "$pad" '' "$text"
}
bmg_item() { printf ' %s) %s\n' "$1" "$2"; }
bmg_pause() {
    echo
    if [ -t 0 ]; then
        read -r -n 1 -s -p 'Press any key to continue . . . ' _bmg_key
        echo
    else
        read -r _bmg_key
    fi
}
bmg_banner() {
    local line
    bmg_hr
    if [ -n "$BMG_TITLE" ]; then bmg_center "$BMG_TITLE"; fi
    bmg_hr
    echo
    if [ "${#BMG_DESC[@]}" -gt 0 ]; then
        for line in "${BMG_DESC[@]}"; do
            if [ -z "$line" ]; then echo; else printf ' %s\n' "$line"; fi
        done
        echo
    fi
    bmg_hr
    echo
}
bmg_heading() { bmg_hr; bmg_center "$1"; bmg_hr; echo; }
bmg_prompt() {
    BMG_CHOICE=''
    printf 'Select Option (1-%s): ' "$1"
    IFS= read -r BMG_CHOICE || return 1
}
bmg_run() {
    # Ctrl+C stops the running option (not the whole menu) and goes on to the pause below.
    trap 'printf "\n"; return 130 2>/dev/null' INT
    "$@"
    bmg_pause
    trap - INT
}
)BMG";
    s += R"BMG(
bmg_hash() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum | cut -d ' ' -f 1
    elif command -v shasum >/dev/null 2>&1; then shasum -a 256 | cut -d ' ' -f 1
    elif command -v openssl >/dev/null 2>&1; then openssl dgst -sha256 | sed 's/^.* //'
    else return 1
    fi
}
bmg_check_password() {
    [ -n "$BMG_PASS" ] || return 0
    local rest salt want entered got
    rest=${BMG_PASS#sha256:}
    salt=${rest%%:*}
    want=${rest#*:}
    while true; do
        bmg_clear
        printf 'Enter password: '
        entered=''
        IFS= read -r -s entered || { echo; exit 1; }
        echo
        got=$(printf '%s%s' "$salt" "$entered" | bmg_hash) || { echo '[!] No SHA-256 tool (sha256sum, shasum or openssl) was found.'; exit 1; }
        if [ "$got" = "$want" ]; then return 0; fi
        echo '[!] Incorrect password.'
        bmg_pause
    done
}
bmg_require_root() {
    [ "$BMG_ROOT" = "1" ] || return 0
    [ "$(id -u)" -eq 0 ] && return 0
    local self="${BMG_SELF:-$0}"
    case "$self" in /*) ;; *) self="$PWD/$self" ;; esac
    bmg_hr
    echo ' Requires root privileges!'
    echo ' Relaunching with elevated rights...'
    bmg_hr
    sleep 1
    if command -v sudo >/dev/null 2>&1; then
        if [ -n "${BMG_SELF:-}" ]; then exec sudo -- "$self" "$@"; fi
        exec sudo -- bash "$self" "$@"
    elif command -v doas >/dev/null 2>&1; then
        if [ -n "${BMG_SELF:-}" ]; then exec doas -- "$self" "$@"; fi
        exec doas -- bash "$self" "$@"
    fi
    echo ' [Error] sudo or doas was not found. Run this script as root instead.'
    exit 1
}
)BMG";
    return s;
}

// Padding that centers 'text' in the banner, mirrored from the runtime (code points, not bytes).
static std::string QuoteForScript(const std::string& s) { return ShQuote(s); }

static std::string OptionChildId(const std::string& parentId, size_t index) {
    return parentId.empty() ? ToStr((long long)index + 1) : parentId + "_" + ToStr((long long)index + 1);
}

static std::string GenerateBash(const std::string& title,
    const std::vector<std::string>& description,
    const std::vector<OptionData>& options,
    const std::string& passwordIn,
    bool requireRoot,
    const std::string& borderChar) {

    const std::string password = NormalizePassword(passwordIn);
    const std::string border = MakeBorder(borderChar);

    std::string sh;
    sh.reserve(16384);
    sh += "#!/usr/bin/env bash\n";
    sh += "# Created with BashMenuGenerator!\n";
    sh += "# Open this file again with BashMenuGenerator (Edit existing generated script) to change it,\n";
    sh += "# or edit the texts, BMG_ROOT and the command blocks by hand.\n\n";
    sh += "BMG_TITLE=" + QuoteForScript(title) + "\n";
    sh += "BMG_BORDER=" + QuoteForScript(border) + "\n";
    sh += "BMG_WIDTH=" + ToStr(kBannerWidth) + "\n";
    sh += std::string("BMG_ROOT=") + (requireRoot ? "1" : "0") + "\n";
    sh += "BMG_PASS=" + QuoteForScript(password) + "\n";
    if (description.empty()) {
        sh += "BMG_DESC=()\n\n";
    } else {
        sh += "BMG_DESC=(\n";
        for (const auto& line : description) sh += "    " + QuoteForScript(line) + "\n";
        sh += ")\n\n";
    }

    sh += BashRuntimeHelpers();

    // One bmg_menu_<id> function per menu and one bmg_run_<id> function per option that runs commands.
    std::function<void(const std::vector<OptionData>&, const std::string&, const std::string&)> emitMenu;
    emitMenu = [&](const std::vector<OptionData>& menuOptions, const std::string& menuId, const std::string& heading) {
        const bool isMain = menuId.empty();
        const std::string fn = "bmg_menu_" + (isMain ? std::string("main") : menuId);
        sh += "\n" + fn + "() {\n";
        sh += "    while true; do\n";
        sh += "        bmg_clear\n";
        if (isMain) sh += "        bmg_banner\n";
        else sh += "        bmg_heading " + QuoteForScript(heading) + "\n";
        for (size_t i = 0; i < menuOptions.size(); ++i) {
            const std::string label = menuOptions[i].label.empty() ? OptionFallbackLabel(i) : menuOptions[i].label;
            sh += "        bmg_item " + ToStr((long long)i + 1) + " " + QuoteForScript(label) + "\n";
        }
        sh += std::string("        bmg_item X ") + (isMain ? "'Exit'" : "'Back'") + "\n";
        sh += "        echo\n        bmg_hr\n        echo\n";
        sh += "        bmg_prompt " + ToStr((long long)menuOptions.size()) + " || return 0\n";
        sh += "        case \"$BMG_CHOICE\" in\n";
        for (size_t i = 0; i < menuOptions.size(); ++i) {
            const OptionData& opt = menuOptions[i];
            const std::string id = OptionChildId(menuId, i);
            if (opt.runMode == 2 && !opt.subOptions.empty())
                sh += "            " + ToStr((long long)i + 1) + ") bmg_menu_" + id + " ;;\n";
            else
                sh += "            " + ToStr((long long)i + 1) + ") bmg_run bmg_run_" + id + " ;;\n";
        }
        sh += "            [Xx]) return 0 ;;\n";
        sh += "        esac\n    done\n}\n";

        for (size_t i = 0; i < menuOptions.size(); ++i) {
            const OptionData& opt = menuOptions[i];
            const std::string id = OptionChildId(menuId, i);
            if (opt.runMode == 2 && !opt.subOptions.empty()) continue;   // reached through bmg_menu_<id>
            sh += "\nbmg_run_" + id + "() {\n";
            sh += "    bmg_clear\n";
            if (opt.runMode == 2) {
                sh += "    # >>> submenu (no options defined)\n";
                sh += "    echo 'No submenu options defined.'\n}\n";
                continue;
            }
            // Commands are written exactly as typed (no indentation) so multi-line constructs such as
            // here-documents keep working.
            sh += "# >>> commands runmode=" + ToStr(opt.runMode == 1 ? 1 : 0) + "\n";
            bool any = false;
            for (const auto& rawCmd : opt.commands) {
                if (Trim(rawCmd).empty()) continue;
                any = true;
                if (opt.runMode == 1) sh += "bash -c " + QuoteForScript(rawCmd) + "\n";
                else sh += rawCmd + "\n";
            }
            sh += "# <<< commands\n";
            if (!any) sh += "    echo 'No command specified.'\n";
            sh += "}\n";
        }
        for (size_t i = 0; i < menuOptions.size(); ++i) {
            const OptionData& opt = menuOptions[i];
            if (opt.runMode != 2 || opt.subOptions.empty()) continue;
            emitMenu(opt.subOptions, OptionChildId(menuId, i), opt.label.empty() ? OptionFallbackLabel(i) : opt.label);
        }
    };
    emitMenu(options, "", "");

    sh += R"BMG(
bmg_main() {
    local pushed=0
    bmg_require_root "$@"
    if [ -n "$BMG_TITLE" ] && [ -t 1 ]; then
        printf '\033[22;0t\033]0;%s\007' "$BMG_TITLE"
        pushed=1
    fi
    bmg_check_password
    bmg_menu_main
    bmg_clear
    if [ "$pushed" = "1" ]; then printf '\033[23;0t'; fi
}

bmg_main "$@"
exit 0
)BMG";
    return sh;
}

// ============================================================
// Writing scripts
// ============================================================
static bool WriteScriptToFile(const std::string& path, const std::string& script, bool executable = true) {
    return WriteBinaryFile(path, script, executable ? 0755 : 0644);
}

static std::string GenerateTempFilePath() {
    return EnsureTrailingSlash(GetEnv("TMPDIR").empty() ? "/tmp" : GetEnv("TMPDIR")) +
        "BMGtempScript_" + TimestampNow("%Y%m%d_%H%M%S") + "_" + ToStr((long long)getpid()) + ".sh";
}

static void DeleteTempFile(const std::string& path) {
    if (path.empty()) return;
    if (path.find("BMGtempScript_") != std::string::npos) unlink(path.c_str());
}

// Turns whatever was typed into a script file name: .bat/.cmd become .sh, a missing extension gets .sh.
static std::string NormalizeScriptFileName(std::string name) {
    name = Trim(name);
    if (EndsWithNoCase(name, ".bat") || EndsWithNoCase(name, ".cmd")) name.resize(name.size() - 4);
    if (!EndsWithNoCase(name, ".sh") && !EndsWithNoCase(name, ".bash")) name += ".sh";
    return name;
}

// A path typed on Windows (C:\Users\...) means nothing here.
static bool LooksLikeWindowsPath(const std::string& p) {
    if (p.size() >= 3 && std::isalpha((unsigned char)p[0]) && p[1] == ':' && (p[2] == '\\' || p[2] == '/')) return true;
    return p.find('\\') != std::string::npos;
}

static bool CreateDirectoryChain(const std::string& dir) { return EnsureDirectoryTree(dir, 0755); }

// Interactive "where should it go" flow. forcedFile / forcedDir come from the "save --name--path" directive.
static bool SaveToManualPath(const std::string& script, std::string& outSavedPath,
    std::string forcedFile = "", std::string forcedDir = "") {
    while (true) {
        std::string filename = forcedFile;
        if (filename.empty()) {
            C_P; Out("\n  Enter filename: "); C_I;
            filename = StripQuotes(ReadLine());
            C_R;
            if (filename.empty()) { C_E; Out("  [Error] Filename cannot be empty.\n"); C_R; continue; }
            if (filename.find('/') != std::string::npos || filename.find('\\') != std::string::npos) {
                C_E; Out("  [Error] Invalid filename.\n"); C_R; continue;
            }
        }
        filename = NormalizeScriptFileName(filename);

        std::string dir = forcedDir;
        if (dir.empty()) {
            C_P; Out("  Enter path [Press Enter for Current Directory]: "); C_I;
            dir = StripQuotes(ReadPathLine());
            C_R;
        }
        if (dir.empty()) dir = CurrentDir();
        dir = ExpandTilde(dir);
        if (dir[0] != '/') dir = JoinPath(CurrentDir(), dir);
        dir = EnsureTrailingSlash(dir);
        std::string fullPath = dir + filename;
        const std::string baseName = filename.substr(0, filename.size() - (EndsWithNoCase(filename, ".bash") ? 5 : 3));
        const std::string checkDir = TrimTrailingSlashes(dir);

        if (!DirExists(checkDir)) {
            if (!forcedDir.empty()) {
                CreateDirectoryChain(checkDir);
            } else {
                C_E; Out("  [Error] Destination folder does not exist!\n"); C_P;
                int r;
                if (g_enableTUI) {
                    r = RunTUIMenu("Destination folder does not exist!", { "Retype file details", "Create folder automatically", "Exit & Save" });
                    if (r == 2 || r == -1) Quit(0);
                } else {
                    Out("    1) Retype file details\n    2) Create folder automatically\n    X) Exit & Save\n  Select option [1]: "); C_I;
                    const std::string opt = Trim(ReadLine());
                    if (opt == "X" || opt == "x") Quit(0);
                    r = (opt == "2") ? 1 : 0;
                }
                if (r == 1) {
                    if (!CreateDirectoryChain(checkDir)) { C_E; Out("  [Error] Failed to create directory.\n"); C_R; continue; }
                } else {
                    continue;
                }
            }
        }

        if (forcedFile.empty() && PathExists(fullPath) && !DirExists(fullPath)) {
            C_E; Out("  [Warning] File already exists!\n"); C_P;
            const std::string renamed = baseName + "1.sh";
            if (g_enableTUI) {
                const int r = RunTUIMenu("File already exists!", { "Replace", "Rename to " + renamed, "Retype", "Exit & Save" });
                if (r == 3 || r == -1) Quit(0);
                if (r == 1) fullPath = dir + renamed;
                else if (r == 2) continue;
            } else {
                Out("    1) Replace\n    2) Rename to " + renamed + "\n    3) Retype\n    X) Exit & Save\n  Select [1]: "); C_I;
                const std::string opt = Trim(ReadLine());
                if (opt == "X" || opt == "x") Quit(0);
                if (opt == "2") fullPath = dir + renamed;
                else if (opt == "3") continue;
            }
        }

        if (WriteScriptToFile(fullPath, script)) {
            C_S; Out("  [SUCCESS] Saved to: " + fullPath + "\n"); C_R;
            outSavedPath = fullPath;
            return true;
        }
        const int err = errno;
        C_E; Printf("  [ERROR] Failed to save. (%s)\n", strerror(err)); C_R;
        if (!forcedFile.empty()) return false;

        int r;
        if (g_enableTUI) {
            r = RunTUIMenu("Failed to save script", { "Retry", "Use file browser", "Exit & Save" }, 1);
            if (r == 2 || r == -1) Quit(0);
        } else {
            C_P; Out("    1) Retry\n    2) Use file browser\n    X) Exit & Save\n  Select [2]: "); C_I;
            std::string fc = Trim(ReadLine());
            if (fc == "X" || fc == "x") Quit(0);
            r = (fc.empty() || fc == "2") ? 1 : 0;
        }
        if (r == 1) {
            const std::string picked = ChooseFile(DK_SAVE_FILE, "Save Bash Menu Script", "sh", filename);
            if (!picked.empty() && WriteScriptToFile(picked, script)) {
                C_S; Out("  [SUCCESS] Saved to: " + picked + "\n"); C_R;
                outSavedPath = picked;
                return true;
            }
            C_E; Out("  [Error] The file browser save failed.\n"); C_R;
        }
    }
}

// ============================================================
// Clipboard: an internal buffer, backed by the system clipboard when a tool is available
// ============================================================
static std::string g_clipboard;

static std::string DetectClipboardTool() {
    if (!GetEnv("WAYLAND_DISPLAY").empty() && CommandExists("wl-copy") && CommandExists("wl-paste")) return "wayland";
    if (!GetEnv("DISPLAY").empty() && CommandExists("xclip")) return "xclip";
    if (!GetEnv("DISPLAY").empty() && CommandExists("xsel")) return "xsel";
    return "";
}

static void ClipboardCopy(const std::string& text) {
    g_clipboard = text;
    static const std::string tool = DetectClipboardTool();
    ProcOpts o;
    o.in = &text;
    o.quietErr = true;
    o.quietOut = true;
    if (tool == "wayland") RunCommand({ "wl-copy" }, o);
    else if (tool == "xclip") RunCommand({ "xclip", "-selection", "clipboard" }, o);
    else if (tool == "xsel") RunCommand({ "xsel", "--clipboard", "--input" }, o);
}

// Returns the system clipboard's text when a tool is available and it differs from our own last
// copy's tool state; falls back to the internal buffer otherwise.
static std::string ClipboardPaste() {
    static const std::string tool = DetectClipboardTool();
    std::string out;
    ProcOpts o;
    o.out = &out;
    o.quietErr = true;
    bool ok = false;
    if (tool == "wayland") ok = RunCommand({ "wl-paste", "--no-newline" }, o) == 0;
    else if (tool == "xclip") ok = RunCommand({ "xclip", "-selection", "clipboard", "-o" }, o) == 0;
    else if (tool == "xsel") ok = RunCommand({ "xsel", "--clipboard", "--output" }, o) == 0;
    if (ok) return out;
    return g_clipboard;
}

// ============================================================
// Full-screen console text editor
// ============================================================
// Used for: Text Mode's "Edit in CLI", the Description and Commands fields of the wizard/script
// editor, and `bmg -edit` on a plain text file. Selection with Shift+Arrows or Ctrl+A, clipboard
// with Ctrl+X/Ctrl+C/Ctrl+V (also Ctrl+Insert / Shift+Delete / Shift+Insert), undo/redo with
// Ctrl+Z/Ctrl+Y, word movement with Ctrl+Arrows (and Ctrl+W to delete a word back), find with
// Ctrl+F and F3 to repeat, Ctrl+Home/End for the whole buffer, Tab inserts four spaces, Ctrl+S
// saves. Esc opens a small menu to finish up.
struct ConsoleEditor {
    std::vector<std::u32string> lines{ U"" };
    int curRow = 0, curCol = 0;
    bool hasSel = false;
    int selRow = 0, selCol = 0;
    int top = 0;          // first visible buffer row
    int leftCol = 0;      // horizontal scroll, shared by all visible rows
    bool modified = false;
    std::string path;                 // non-empty: FILE mode: Ctrl+S writes here
    bool fieldMode = false;
    std::string statusMsg;
    bool wantExit = false;
    bool keepChanges = false;         // result: commit (save, or keep the field) vs discard

    enum EditKind { EK_NONE, EK_INSERT, EK_DELETE_FWD, EK_DELETE_BACK, EK_NEWLINE, EK_PASTE, EK_BULK };
    EditKind lastKind = EK_NONE;
    struct Snapshot { std::vector<std::u32string> lines; int row, col; };
    std::vector<Snapshot> undoStack, redoStack;
    static const size_t kMaxUndo = 200;

    std::u32string lastFind;

    // ---- selection / position helpers ----
    static bool Before(int r1, int c1, int r2, int c2) { return r1 < r2 || (r1 == r2 && c1 < c2); }
    void NormalizedSel(int& r1, int& c1, int& r2, int& c2) const {
        if (Before(selRow, selCol, curRow, curCol)) { r1 = selRow; c1 = selCol; r2 = curRow; c2 = curCol; }
        else { r1 = curRow; c1 = curCol; r2 = selRow; c2 = selCol; }
    }
    void ClearSelection() { hasSel = false; }
    void StartSelectionIfNeeded() { if (!hasSel) { hasSel = true; selRow = curRow; selCol = curCol; } }

    std::string SelectionText() const {
        if (!hasSel) return "";
        int r1, c1, r2, c2;
        NormalizedSel(r1, c1, r2, c2);
        if (r1 == r2) return U32ToUtf8(lines[(size_t)r1].substr((size_t)c1, (size_t)(c2 - c1)));
        std::string out = U32ToUtf8(lines[(size_t)r1].substr((size_t)c1)) + "\n";
        for (int r = r1 + 1; r < r2; ++r) out += U32ToUtf8(lines[(size_t)r]) + "\n";
        out += U32ToUtf8(lines[(size_t)r2].substr(0, (size_t)c2));
        return out;
    }

    void PushUndo(EditKind kind) {
        if (kind == lastKind && kind != EK_NEWLINE && kind != EK_BULK && kind != EK_PASTE && !undoStack.empty()) return;
        Snapshot s;
        s.lines = lines;
        s.row = curRow;
        s.col = curCol;
        undoStack.push_back(std::move(s));
        if (undoStack.size() > kMaxUndo) undoStack.erase(undoStack.begin());
        redoStack.clear();
        lastKind = kind;
    }

    void DeleteSelectionNoUndo() {
        int r1, c1, r2, c2;
        NormalizedSel(r1, c1, r2, c2);
        if (r1 == r2) {
            lines[(size_t)r1].erase((size_t)c1, (size_t)(c2 - c1));
        } else {
            lines[(size_t)r1] = lines[(size_t)r1].substr(0, (size_t)c1) + lines[(size_t)r2].substr((size_t)c2);
            lines.erase(lines.begin() + r1 + 1, lines.begin() + r2 + 1);
        }
        curRow = r1;
        curCol = c1;
        ClearSelection();
        modified = true;
    }

    void DeleteSelection(EditKind kind) {
        PushUndo(kind);
        DeleteSelectionNoUndo();
    }

    void InsertText(const std::u32string& text) {
        if (hasSel) DeleteSelectionNoUndo();
        std::u32string cur = text;
        size_t start = 0;
        bool first = true;
        while (true) {
            const size_t nl = cur.find(U'\n', start);
            const std::u32string piece = cur.substr(start, nl == std::u32string::npos ? std::u32string::npos : nl - start);
            if (first) {
                lines[(size_t)curRow].insert((size_t)curCol, piece);
                curCol += (int)piece.size();
                first = false;
            } else {
                const std::u32string rest = lines[(size_t)curRow].substr((size_t)curCol);
                lines[(size_t)curRow].resize((size_t)curCol);
                lines.insert(lines.begin() + curRow + 1, piece + rest);
                ++curRow;
                curCol = (int)piece.size();
            }
            if (nl == std::u32string::npos) break;
            start = nl + 1;
        }
        modified = true;
    }

    void TypeChar(char32_t c) {
        PushUndo(EK_INSERT);
        if (hasSel) DeleteSelectionNoUndo();
        lines[(size_t)curRow].insert((size_t)curCol, 1, c);
        ++curCol;
        modified = true;
    }

    void Paste() {
        const std::string text = ClipboardPaste();
        if (text.empty()) return;
        PushUndo(EK_PASTE);
        InsertText(Utf8ToU32(text));
    }

    void Copy() { if (hasSel) ClipboardCopy(SelectionText()); }
    void Cut() { if (hasSel) { ClipboardCopy(SelectionText()); DeleteSelection(EK_DELETE_FWD); } }

    void Undo() {
        if (undoStack.empty()) return;
        Snapshot cur{ lines, curRow, curCol };
        redoStack.push_back(std::move(cur));
        Snapshot s = undoStack.back();
        undoStack.pop_back();
        lines = s.lines;
        curRow = s.row;
        curCol = s.col;
        ClearSelection();
        lastKind = EK_NONE;
        modified = true;
    }
    void Redo() {
        if (redoStack.empty()) return;
        Snapshot cur{ lines, curRow, curCol };
        undoStack.push_back(std::move(cur));
        Snapshot s = redoStack.back();
        redoStack.pop_back();
        lines = s.lines;
        curRow = s.row;
        curCol = s.col;
        ClearSelection();
        lastKind = EK_NONE;
        modified = true;
    }

    bool IsWordChar(char32_t c) const { return c == U'_' || (CharWidth(c) > 0 && c > 32 && c != U' ' && c != U'\t'); }

    void MoveWordLeft(bool extend) {
        if (extend) StartSelectionIfNeeded(); else ClearSelection();
        if (curCol == 0) { if (curRow > 0) { --curRow; curCol = (int)lines[(size_t)curRow].size(); } return; }
        const std::u32string& l = lines[(size_t)curRow];
        int c = curCol;
        while (c > 0 && !IsWordChar(l[(size_t)c - 1])) --c;
        while (c > 0 && IsWordChar(l[(size_t)c - 1])) --c;
        curCol = c;
    }
    void MoveWordRight(bool extend) {
        if (extend) StartSelectionIfNeeded(); else ClearSelection();
        const std::u32string* l = &lines[(size_t)curRow];
        if (curCol >= (int)l->size()) { if (curRow + 1 < (int)lines.size()) { ++curRow; curCol = 0; } return; }
        int c = curCol;
        while (c < (int)l->size() && !IsWordChar((*l)[(size_t)c])) ++c;
        while (c < (int)l->size() && IsWordChar((*l)[(size_t)c])) ++c;
        curCol = c;
    }

    void DeleteWordLeft() {
        PushUndo(EK_DELETE_BACK);
        const int startRow = curRow, startCol = curCol;
        MoveWordLeft(false);
        selRow = curRow; selCol = curCol;
        const int endRow = startRow, endCol = startCol;
        curRow = endRow; curCol = endCol;
        hasSel = true;
        DeleteSelectionNoUndo();
    }
    void DeleteWordRight() {
        PushUndo(EK_DELETE_FWD);
        selRow = curRow; selCol = curCol;
        MoveWordRight(false);
        hasSel = true;
        DeleteSelectionNoUndo();
    }

    void MoveLeft(bool extend) {
        if (extend) StartSelectionIfNeeded(); else ClearSelection();
        if (curCol > 0) --curCol;
        else if (curRow > 0) { --curRow; curCol = (int)lines[(size_t)curRow].size(); }
    }
    void MoveRight(bool extend) {
        if (extend) StartSelectionIfNeeded(); else ClearSelection();
        if (curCol < (int)lines[(size_t)curRow].size()) ++curCol;
        else if (curRow + 1 < (int)lines.size()) { ++curRow; curCol = 0; }
    }
    void MoveUp(bool extend) {
        if (extend) StartSelectionIfNeeded(); else ClearSelection();
        if (curRow > 0) { --curRow; curCol = std::min(curCol, (int)lines[(size_t)curRow].size()); }
        else curCol = 0;
    }
    void MoveDown(bool extend) {
        if (extend) StartSelectionIfNeeded(); else ClearSelection();
        if (curRow + 1 < (int)lines.size()) { ++curRow; curCol = std::min(curCol, (int)lines[(size_t)curRow].size()); }
        else curCol = (int)lines[(size_t)curRow].size();
    }
    void MoveHome(bool extend) { if (extend) StartSelectionIfNeeded(); else ClearSelection(); curCol = 0; }
    void MoveEnd(bool extend) { if (extend) StartSelectionIfNeeded(); else ClearSelection(); curCol = (int)lines[(size_t)curRow].size(); }
    void MoveBufferHome(bool extend) { if (extend) StartSelectionIfNeeded(); else ClearSelection(); curRow = 0; curCol = 0; }
    void MoveBufferEnd(bool extend) {
        if (extend) StartSelectionIfNeeded(); else ClearSelection();
        curRow = (int)lines.size() - 1;
        curCol = (int)lines[(size_t)curRow].size();
    }
    void SelectAll() {
        selRow = 0; selCol = 0;
        hasSel = true;
        curRow = (int)lines.size() - 1;
        curCol = (int)lines[(size_t)curRow].size();
    }

    void Backspace() {
        if (hasSel) { DeleteSelection(EK_DELETE_BACK); return; }
        if (curCol == 0 && curRow == 0) return;
        PushUndo(curCol == 0 ? EK_NEWLINE : EK_DELETE_BACK);
        if (curCol == 0) {
            const std::u32string cur = lines[(size_t)curRow];
            lines.erase(lines.begin() + curRow);
            --curRow;
            curCol = (int)lines[(size_t)curRow].size();
            lines[(size_t)curRow] += cur;
        } else {
            lines[(size_t)curRow].erase((size_t)curCol - 1, 1);
            --curCol;
        }
        modified = true;
    }
    void DeleteForward() {
        if (hasSel) { DeleteSelection(EK_DELETE_FWD); return; }
        if (curCol == (int)lines[(size_t)curRow].size() && curRow + 1 >= (int)lines.size()) return;
        PushUndo(curCol == (int)lines[(size_t)curRow].size() ? EK_NEWLINE : EK_DELETE_FWD);
        if (curCol == (int)lines[(size_t)curRow].size()) {
            lines[(size_t)curRow] += lines[(size_t)curRow + 1];
            lines.erase(lines.begin() + curRow + 1);
        } else {
            lines[(size_t)curRow].erase((size_t)curCol, 1);
        }
        modified = true;
    }
    void NewLine() {
        PushUndo(EK_NEWLINE);
        if (hasSel) DeleteSelectionNoUndo();
        const std::u32string rest = lines[(size_t)curRow].substr((size_t)curCol);
        lines[(size_t)curRow].resize((size_t)curCol);
        lines.insert(lines.begin() + curRow + 1, rest);
        ++curRow;
        curCol = 0;
        modified = true;
    }

    bool FindNext(const std::u32string& needle, bool fromCursorStart) {
        if (needle.empty() || lines.empty()) return false;
        const int n = (int)lines.size();
        int r = curRow;
        int c = fromCursorStart ? curCol : curCol + 1;
        for (int steps = 0; steps <= n; ++steps) {
            const std::u32string& l = lines[(size_t)r];
            if (c <= (int)l.size()) {
                const size_t pos = l.find(needle, (size_t)std::max(0, c));
                if (pos != std::u32string::npos) {
                    curRow = r;
                    curCol = (int)pos;
                    ClearSelection();
                    selRow = r; selCol = (int)pos;
                    curCol = (int)(pos + needle.size());
                    hasSel = true;
                    return true;
                }
            }
            r = (r + 1) % n;
            c = 0;
        }
        return false;
    }
};

static bool DecodeTextFile(const std::string& raw, std::vector<std::string>& outLines) {
    std::string data = raw;
    if (data.size() >= 3 && (unsigned char)data[0] == 0xEF && (unsigned char)data[1] == 0xBB && (unsigned char)data[2] == 0xBF)
        data.erase(0, 3);
    else if (data.size() >= 2 && ((unsigned char)data[0] == 0xFF && (unsigned char)data[1] == 0xFE)) {
        // UTF-16 LE, as old Windows configs sometimes are
        std::u32string u;
        for (size_t i = 2; i + 1 < data.size(); i += 2) {
            const uint16_t lo = (unsigned char)data[i] | ((unsigned char)data[i + 1] << 8);
            if (lo >= 0xD800 && lo <= 0xDBFF && i + 3 < data.size()) {
                const uint16_t hi = (unsigned char)data[i + 2] | ((unsigned char)data[i + 3] << 8);
                if (hi >= 0xDC00 && hi <= 0xDFFF) {
                    u.push_back((char32_t)(0x10000 + ((lo - 0xD800) << 10) + (hi - 0xDC00)));
                    i += 2;
                    continue;
                }
            }
            u.push_back((char32_t)lo);
        }
        data = U32ToUtf8(u);
    }
    outLines = SplitLines(data);
    // the newline that ends the last line is a terminator, not an extra empty line
    if (!outLines.empty() && outLines.back().empty() && !data.empty() && (data.back() == '\n' || data.back() == '\r')) outLines.pop_back();
    return true;
}

static bool ReadConsoleEditorTextFile(const std::string& path, std::vector<std::string>& lines) {
    std::string raw;
    if (!ReadBinaryFile(path, raw)) return false;
    return DecodeTextFile(raw, lines);
}

// Runs the full-screen editor. 'lines' is the starting content and, when the result is kept
// (true), the edited content; 'savePath' non-empty puts the editor in FILE mode (Ctrl+S writes
// there for real). Returns whether the edits should be kept.
static bool ConsoleTextEditor(std::vector<std::string>& lines, const std::string& savePath, const std::string& finishLabel) {
    if (!g_stdinTty || !g_stdoutTty) {
        C_N; Out("\n  (No interactive terminal: paste the text, then an empty line to finish.)\n"); C_R;
        std::vector<std::string> collected;
        while (true) {
            const std::string l = ReadLinePlain();
            if (l.empty()) break;
            collected.push_back(l);
        }
        lines = collected;
        return true;
    }

    ConsoleEditor ed;
    ed.path = savePath;
    ed.fieldMode = savePath.empty();
    ed.lines.clear();
    if (lines.empty()) ed.lines.push_back(U"");
    else for (const auto& l : lines) ed.lines.push_back(Utf8ToU32(l));

    RawScope raw;
    AltScreenScope alt;
    CursorScope hideCursor(false);
    PasteScope paste;

    auto textRows = [&]() { return std::max(3, TermRows() - 5); };    // title, rule, rows, rule, status, help

    auto ensureVisible = [&]() {
        const int rows = textRows();
        if (ed.curRow < ed.top) ed.top = ed.curRow;
        if (ed.curRow >= ed.top + rows) ed.top = ed.curRow - rows + 1;
        ed.top = std::max(0, std::min(ed.top, std::max(0, (int)ed.lines.size() - 1)));
        const int cols = std::max(10, TermCols());
        int before = 0;
        for (int i = ed.leftCol; i < ed.curCol; ++i) before += CharWidth(DisplayChar(ed.lines[(size_t)ed.curRow][(size_t)i]));
        while (ed.leftCol < ed.curCol && before >= cols - 1) {
            before -= CharWidth(DisplayChar(ed.lines[(size_t)ed.curRow][(size_t)ed.leftCol]));
            ++ed.leftCol;
        }
        while (ed.leftCol > 0 && ed.leftCol > ed.curCol) --ed.leftCol;
        if (ed.leftCol > ed.curCol) ed.leftCol = ed.curCol;
    };

    auto render = [&]() {
        ensureVisible();
        const int cols = std::max(10, TermCols());
        const int rows = textRows();
        std::string o = "\033[H";
        int selR1 = 0, selC1 = 0, selR2 = 0, selC2 = 0;
        if (ed.hasSel) ed.NormalizedSel(selR1, selC1, selR2, selC2);

        std::string title = ed.fieldMode ? "Text Editor" : ("Editing: " + (ed.path.empty() ? std::string("(untitled)") : BaseName(ed.path)));
        if (ed.modified) title += " [modified]";
        o += "\033[2K" + ColorEsc(g_theme.header) + " " + FitToWidth(title, cols - 2) + ColorEsc(CC_RESET) + "\r\n";
        o += "\033[2K" + ColorEsc(g_theme.banner) + std::string((size_t)std::min(cols, kBannerWidth + 23), '-') + ColorEsc(CC_RESET) + "\r\n";

        for (int r = 0; r < rows; ++r) {
            const int row = ed.top + r;
            o += "\033[2K";
            if (row < (int)ed.lines.size()) {
                const std::u32string& l = ed.lines[(size_t)row];
                int col = 0;
                for (int i = ed.leftCol; i < (int)l.size(); ++i) {
                    const bool sel = ed.hasSel && ((row > selR1 || (row == selR1 && i >= selC1)) && (row < selR2 || (row == selR2 && i < selC2)));
                    const char32_t g = DisplayChar(l[(size_t)i]);
                    const int w = CharWidth(g);
                    if (col + w > cols) break;
                    if (sel) o += InvertedEsc(CC_WHITE, CC_BLACK);
                    AppendUtf8(o, g);
                    if (sel) o += ColorEsc(CC_RESET);
                    col += w;
                }
                if (ed.hasSel && row > selR1 && row < selR2 && col < cols) {
                    o += InvertedEsc(CC_WHITE, CC_BLACK) + " " + ColorEsc(CC_RESET);
                }
            }
            o += "\r\n";
        }

        const int selLines = ed.hasSel ? (selR2 - selR1 + 1) : 0;
        char status[160];
        snprintf(status, sizeof(status), " Ln %d/%d, Col %d%s%s",
            ed.curRow + 1, (int)ed.lines.size(), ed.curCol + 1,
            ed.hasSel ? ("  (" + ToStr(selLines) + (selLines == 1 ? " line selected)" : " lines selected)")).c_str() : "",
            ed.statusMsg.empty() ? "" : ("   " + ed.statusMsg).c_str());
        o += "\033[2K" + ColorEsc(g_theme.banner) + std::string((size_t)std::min(cols, kBannerWidth + 23), '-') + ColorEsc(CC_RESET) + "\r\n";
        o += "\033[2K" + ColorEsc(g_theme.info) + FitToWidth(status, cols - 1) + ColorEsc(CC_RESET) + "\r\n";
        o += "\033[2K" + ColorEsc(g_theme.text) +
            FitToWidth(" Ctrl+S save  Ctrl+F find  F3 next  Ctrl+Z/Y undo/redo  Esc menu", cols - 1) + ColorEsc(CC_RESET);

        // place the terminal cursor
        int curCol = 0;
        for (int i = ed.leftCol; i < ed.curCol; ++i) curCol += CharWidth(DisplayChar(ed.lines[(size_t)ed.curRow][(size_t)i]));
        const int screenRow = 3 + (ed.curRow - ed.top);
        o += "\033[" + ToStr(screenRow) + ";" + ToStr(curCol + 1) + "H";
        Out(o);
        Flush();
    };

    auto bottomPrompt = [&](const std::string& prompt, const std::string& initial) -> std::pair<bool, std::string> {
        const int rows = TermRows();
        Out("\033[" + ToStr(rows) + ";1H\033[2K" + ColorEsc(g_theme.prompt) + prompt + ColorEsc(g_theme.input));
        g_col = DisplayWidth(prompt);
        CursorScope showCursor(false);
        bool cancelled = false;
        ReadLineOptions o;
        o.cancelled = &cancelled;
        const std::string s = ReadLineEx(initial, o);
        Out(ColorEsc(CC_RESET));
        return { !cancelled, s };
    };

    auto doSave = [&]() -> bool {
        if (ed.path.empty()) {
            auto r = bottomPrompt("Save as: ", "");
            if (!r.first || Trim(r.second).empty()) { ed.statusMsg = "Save cancelled."; return false; }
            ed.path = ExpandTilde(StripQuotes(r.second));
            if (ed.path[0] != '/') ed.path = JoinPath(CurrentDir(), ed.path);
        }
        std::string data;
        for (const auto& l : ed.lines) data += U32ToUtf8(l) + "\n";
        if (WriteBinaryFile(ed.path, data, 0644)) {
            ed.modified = false;
            ed.statusMsg = "Saved.";
            return true;
        }
        ed.statusMsg = "Save failed: " + std::string(strerror(errno));
        return false;
    };

    auto escMenu = [&]() {
        std::vector<std::string> opts;
        if (ed.fieldMode) opts = { "Resume editing", finishLabel.empty() ? std::string("Finish (keep changes)") : finishLabel, "Cancel (discard changes)" };
        else opts = { "Resume editing", "Save", "Save and exit", "Exit without saving" };
        ClearScreen();
        const int sel = RunTUIMenu(ed.fieldMode ? (finishLabel.empty() ? "Finish editing?" : "Done editing?") : "Editor menu", opts, 0, -1, true);
        if (sel <= 0) return;   // resume, or Esc
        if (ed.fieldMode) {
            if (sel == 1) { ed.keepChanges = true; ed.wantExit = true; }
            else { ed.keepChanges = false; ed.wantExit = true; }
        } else {
            if (sel == 1) { doSave(); }
            else if (sel == 2) { if (doSave()) { ed.keepChanges = true; ed.wantExit = true; } }
            else if (sel == 3) { ed.keepChanges = false; ed.wantExit = true; }
        }
    };

    render();
    while (!ed.wantExit) {
        const Key k = ReadKey();
        ed.statusMsg.clear();
        bool changed = true;
        switch (k.code) {
            case K_RESIZE: break;
            case K_EOF: ed.wantExit = true; ed.keepChanges = false; break;
            case K_ESC: escMenu(); break;
            case K_ENTER: ed.NewLine(); break;
            case K_TAB: ed.PushUndo(ConsoleEditor::EK_INSERT); ed.InsertText(U"    "); break;
            case K_BACKSPACE:
                if (k.ctrl) ed.DeleteWordLeft(); else ed.Backspace();
                break;
            case K_DELETE:
                if (k.shift) ed.Cut(); else if (k.ctrl) ed.DeleteWordRight(); else ed.DeleteForward();
                break;
            case K_INSERT:
                if (k.shift) ed.Paste(); else if (k.ctrl) ed.Copy(); else changed = false;
                break;
            case K_LEFT: if (k.ctrl) ed.MoveWordLeft(k.shift); else ed.MoveLeft(k.shift); break;
            case K_RIGHT: if (k.ctrl) ed.MoveWordRight(k.shift); else ed.MoveRight(k.shift); break;
            case K_UP: ed.MoveUp(k.shift); break;
            case K_DOWN: ed.MoveDown(k.shift); break;
            case K_HOME: if (k.ctrl) ed.MoveBufferHome(k.shift); else ed.MoveHome(k.shift); break;
            case K_END: if (k.ctrl) ed.MoveBufferEnd(k.shift); else ed.MoveEnd(k.shift); break;
            case K_PGUP: ed.curRow = std::max(0, ed.curRow - textRows()); ed.ClearSelection(); break;
            case K_PGDN: ed.curRow = std::min((int)ed.lines.size() - 1, ed.curRow + textRows()); ed.ClearSelection(); break;
            case K_F3: if (!ed.lastFind.empty() && !ed.FindNext(ed.lastFind, false)) ed.statusMsg = "Not found."; break;
            case K_PASTE: ed.PushUndo(ConsoleEditor::EK_PASTE); ed.InsertText(Utf8ToU32(k.text)); break;
            case K_CHAR:
                if (k.ctrl) {
                    switch (k.ch) {
                        case 'a': ed.SelectAll(); break;
                        case 'c': ed.Copy(); break;
                        case 'x': ed.Cut(); break;
                        case 'v': ed.Paste(); break;
                        case 'z': ed.Undo(); break;
                        case 'y': ed.Redo(); break;
                        case 'w': ed.DeleteWordLeft(); break;
                        case 's': doSave(); break;
                        case 'f': {
                            auto r = bottomPrompt("Find: ", U32ToUtf8(ed.lastFind));
                            if (r.first && !Trim(r.second).empty()) {
                                ed.lastFind = Utf8ToU32(r.second);
                                if (!ed.FindNext(ed.lastFind, true)) ed.statusMsg = "Not found.";
                            }
                            break;
                        }
                        default: changed = false; break;
                    }
                } else if (!k.alt) {
                    ed.TypeChar(k.ch);
                } else {
                    changed = false;
                }
                break;
            default: changed = false; break;
        }
        (void)changed;
        if (ed.lines.empty()) ed.lines.push_back(U"");
        if (!ed.wantExit) render();
    }

    if (ed.keepChanges) {
        lines.clear();
        for (const auto& l : ed.lines) lines.push_back(U32ToUtf8(l));
    }
    return ed.keepChanges;
}

static void EditLinesWithConsoleEditor(std::vector<std::string>& lines, bool removeEmptyLines) {
    std::vector<std::string> working = lines;
    if (ConsoleTextEditor(working, "")) {
        if (removeEmptyLines) {
            std::vector<std::string> cleaned;
            for (auto& l : working) { const std::string t = Trim(l); if (!t.empty()) cleaned.push_back(l); }
            working = cleaned;
        }
        lines = working;
    }
}

// ============================================================
// Menu model shared by the wizard, Text Mode and the script editor
// ============================================================
struct MenuModel {
    std::string title;
    std::vector<std::string> description;
    std::string password;               // "" (none) or a password record (sha256:salt:hash)
    std::vector<OptionData> options;
    bool requireRoot = false;
    std::string borderChar = "=";
};

struct SaveDirective {
    std::string filename;               // from "save --name--path"
    std::string path;
    bool previewOnly = false;           // "save n"
};

// ============================================================
// Config Loader / Text Mode Parser
// ============================================================
static std::string RTrim(const std::string& s) {
    size_t e = s.size();
    while (e > 0 && (unsigned char)s[e - 1] <= 32) --e;
    return s.substr(0, e);
}

static int LeadingWhitespace(const std::string& s) {
    int n = 0;
    while ((size_t)n < s.size() && (s[(size_t)n] == ' ' || s[(size_t)n] == '\t')) ++n;
    return n;
}

// Matches "keyword rest" (keyword case-insensitive, one separating space already required) and
// returns the rest of the line untouched.
static bool KeywordLine(const std::string& line, const char* keyword, std::string& rest) {
    const size_t k = strlen(keyword);
    if (line.size() <= k || line[k] != ' ') return false;
    if (ToLower(line.substr(0, k)) != keyword) return false;
    rest = line.substr(k + 1);
    return true;
}

static bool IsRootKeyword(const std::string& line, std::string& value) {
    return KeywordLine(line, "admin", value) || KeywordLine(line, "root", value) || KeywordLine(line, "sudo", value);
}

static int ParseInt(const std::string& s) { return atoi(Trim(s).c_str()); }

// Removes the indentation shared by every command line of a block, so heredocs and nested blocks
// written with the tutorial's 4-space indent behave exactly as typed.
static void DedentLines(std::vector<std::string>& lines) {
    int minIndent = -1;
    for (const auto& l : lines) {
        if (Trim(l).empty()) continue;
        const int ind = LeadingWhitespace(l);
        if (minIndent < 0 || ind < minIndent) minIndent = ind;
    }
    if (minIndent <= 0) return;
    for (auto& l : lines) {
        if ((int)l.size() >= minIndent) l = l.substr((size_t)minIndent);
        else l = Trim(l);
    }
}

static void NormalizeParsedOptions(std::vector<OptionData>& opts) {
    for (auto& opt : opts) {
        if (opt.label.empty()) opt.label = "Option";
        if (opt.runMode == 2) opt.commands.clear();
        NormalizeParsedOptions(opt.subOptions);
    }
}

static bool ParseScriptConfigLines(const std::vector<std::string>& lines, MenuModel& m, SaveDirective& save) {
    m = MenuModel();
    save = SaveDirective();

    int optionCount = 0;
    int currentOption = -1;
    bool sawDescription = false;

    std::function<void(size_t&, OptionData&, int, int)> parseOptionBlock;
    parseOptionBlock = [&](size_t& pos, OptionData& option, int depth, int openIndent) {
        std::vector<std::string> block;
        while (pos < lines.size()) {
            const std::string rawLine = RTrim(lines[pos]);
            const std::string wLine = Trim(rawLine);

            // A line with just } closes the block, unless it is indented deeper than the line that
            // opened the block (then it belongs to a command, e.g. the end of a shell function).
            if (wLine == "}" && LeadingWhitespace(rawLine) <= openIndent) { ++pos; break; }
            if (wLine.empty() || wLine.compare(0, 2, "//") == 0) { ++pos; continue; }

            std::string rest;
            if (KeywordLine(wLine, "runmode", rest)) {
                const int mode = ParseInt(rest);
                option.runMode = (mode >= 0 && mode <= 2) ? mode : 0;
                ++pos;
                continue;
            }
            if (StartsWith(ToLower(wLine), "suboption num:")) {
                int count = ParseInt(wLine.substr(14));
                if (count < 1) count = 1;
                if (count > kMaxOptions) count = kMaxOptions;
                option.subOptions.resize((size_t)count);
                ++pos;
                continue;
            }
            if (KeywordLine(wLine, "suboption", rest)) {
                const size_t lbl = wLine.find(" label ");
                const size_t brc = wLine.find(" {");
                if (lbl != std::string::npos) {
                    const int num = ParseInt(wLine.substr(10, lbl - 10));
                    if (num >= 1 && num <= kMaxOptions) {
                        if ((size_t)num > option.subOptions.size()) option.subOptions.resize((size_t)num);
                        option.subOptions[(size_t)num - 1].label = Trim(wLine.substr(lbl + 7));
                    }
                    ++pos;
                    continue;
                }
                if (brc != std::string::npos) {
                    const int num = ParseInt(wLine.substr(10, brc - 10));
                    if (num >= 1 && num <= kMaxOptions) {
                        if ((size_t)num > option.subOptions.size()) option.subOptions.resize((size_t)num);
                        const int indent = LeadingWhitespace(rawLine);
                        ++pos;
                        parseOptionBlock(pos, option.subOptions[(size_t)num - 1], depth + 1, indent);
                        continue;
                    }
                }
            }
            // "option N {" is accepted inside a submenu as a readable alias for "suboption N {".
            if (depth > 0 && StartsWith(ToLower(wLine), "option ") && wLine.find(" {") != std::string::npos) {
                const size_t brc = wLine.find(" {");
                const int num = ParseInt(wLine.substr(7, brc - 7));
                if (num >= 1 && num <= kMaxOptions) {
                    if ((size_t)num > option.subOptions.size()) option.subOptions.resize((size_t)num);
                    const int indent = LeadingWhitespace(rawLine);
                    ++pos;
                    parseOptionBlock(pos, option.subOptions[(size_t)num - 1], depth + 1, indent);
                    continue;
                }
            }
            block.push_back(rawLine);
            ++pos;
        }
        DedentLines(block);
        for (auto& l : block) option.commands.push_back(l);
    };

    auto resolveOptionPath = [&](const std::vector<size_t>& path) -> OptionData* {
        if (path.empty() || path.size() > 8 || path[0] >= (size_t)kMaxOptions) return nullptr;
        if (path[0] >= m.options.size()) {
            m.options.resize(path[0] + 1);
            optionCount = (int)m.options.size();
        }
        OptionData* cur = &m.options[path[0]];
        for (size_t d = 1; d < path.size(); ++d) {
            if (path[d] >= (size_t)kMaxOptions) return nullptr;
            if (path[d] >= cur->subOptions.size()) cur->subOptions.resize(path[d] + 1);
            cur = &cur->subOptions[path[d]];
        }
        return cur;
    };

    // "option 2 suboption 1 suboption 3 label Foo" -> path {1, 0, 2} and the rest "label Foo".
    auto parseFlatOptionLine = [&](const std::string& line, std::vector<size_t>& path, std::string& rest) -> bool {
        path.clear();
        rest.clear();
        if (line.size() < 7 || ToLower(line.substr(0, 7)) != "option ") return false;
        auto readNumber = [&](size_t& at, int& value) -> bool {
            while (at < line.size() && line[at] == ' ') ++at;
            const size_t start = at;
            while (at < line.size() && std::isdigit((unsigned char)line[at])) ++at;
            if (at == start) return false;
            value = atoi(line.substr(start, at - start).c_str());
            return true;
        };
        size_t pos = 7;
        int number = 0;
        if (!readNumber(pos, number) || number < 1) return false;
        path.push_back((size_t)number - 1);
        bool sawSub = false;
        while (true) {
            size_t probe = pos;
            while (probe < line.size() && line[probe] == ' ') ++probe;
            if (ToLower(line.substr(probe, 10)) != "suboption ") break;
            probe += 10;
            if (!readNumber(probe, number) || number < 1) { path.clear(); return false; }
            path.push_back((size_t)number - 1);
            pos = probe;
            sawSub = true;
        }
        if (!sawSub) { path.clear(); return false; }
        while (pos < line.size() && line[pos] == ' ') ++pos;
        rest = line.substr(pos);
        return true;
    };

    for (size_t i = 0; i < lines.size();) {
        const std::string rawLine = RTrim(lines[i]);
        const std::string wLine = Trim(rawLine);
        std::string rest;

        if (wLine.empty() || wLine.compare(0, 2, "//") == 0) { ++i; continue; }

        if (KeywordLine(wLine, "title", rest)) { m.title = Trim(rest); ++i; continue; }

        if (KeywordLine(wLine, "border", rest)) {
            const std::string v = Trim(rest);
            if (!v.empty()) m.borderChar = FirstChar(v);
            ++i;
            continue;
        }

        if (KeywordLine(wLine, "description", rest)) {
            if (rest != "n" && rest != "no") { m.description.push_back(rest); sawDescription = true; }
            ++i;
            continue;
        }
        // a bare "description" line is a blank line inside ASCII art (but not a placeholder before the first line)
        if (ToLower(wLine) == "description") {
            if (sawDescription) m.description.push_back("");
            ++i;
            continue;
        }

        if (IsRootKeyword(wLine, rest)) { m.requireRoot = IsYes(rest); ++i; continue; }

        if (KeywordLine(wLine, "pass", rest)) {
            const std::string p = Trim(rest);
            if (p != "n" && p != "no" && !p.empty()) m.password = p;
            ++i;
            continue;
        }

        if (StartsWith(ToLower(wLine), "option num:")) {
            optionCount = ParseInt(wLine.substr(11));
            if (optionCount < 1) optionCount = 1;
            if (optionCount > kMaxOptions) optionCount = kMaxOptions;
            m.options.resize((size_t)optionCount);
            ++i;
            continue;
        }

        {
            std::vector<size_t> flatPath;
            std::string flatRest;
            if (parseFlatOptionLine(wLine, flatPath, flatRest)) {
                OptionData* target = resolveOptionPath(flatPath);
                std::string r2;
                if (KeywordLine(flatRest, "label", r2)) {
                    if (target) target->label = Trim(r2);
                    ++i;
                } else if (!flatRest.empty() && flatRest[0] == '{') {
                    const int indent = LeadingWhitespace(rawLine);
                    ++i;
                    if (target) parseOptionBlock(i, *target, (int)flatPath.size() - 1, indent);
                    else { OptionData discarded; parseOptionBlock(i, discarded, 0, indent); }
                } else if (KeywordLine(flatRest, "runmode", r2)) {
                    if (target) {
                        const int mode = ParseInt(r2);
                        target->runMode = (mode >= 0 && mode <= 2) ? mode : 0;
                    }
                    ++i;
                } else {
                    ++i;
                }
                continue;
            }
        }

        if (KeywordLine(wLine, "option", rest)) {
            const size_t lbl = wLine.find(" label ");
            const size_t brc = wLine.find(" {");
            const size_t rmp = wLine.find(" runmode ");
            // "option N runmode M" on one line (a convenience; the classic form is a runmode line after the block)
            if (rmp != std::string::npos && (lbl == std::string::npos || rmp < lbl) && (brc == std::string::npos || rmp < brc)) {
                const int num = ParseInt(wLine.substr(7, rmp - 7));
                const int mode = ParseInt(wLine.substr(rmp + 9));
                if (num >= 1 && num <= kMaxOptions) {
                    if (num > optionCount) { optionCount = num; m.options.resize((size_t)optionCount); }
                    m.options[(size_t)num - 1].runMode = (mode >= 0 && mode <= 2) ? mode : 0;
                    currentOption = num - 1;
                }
                ++i;
                continue;
            }
            if (lbl != std::string::npos) {
                const int num = ParseInt(wLine.substr(7, lbl - 7));
                if (num >= 1 && num <= optionCount) m.options[(size_t)num - 1].label = Trim(wLine.substr(lbl + 7));
                ++i;
                continue;
            }
            if (brc != std::string::npos) {
                const int num = ParseInt(wLine.substr(7, brc - 7));
                if (num >= 1 && num <= kMaxOptions) {
                    if (num > optionCount) { optionCount = num; m.options.resize((size_t)optionCount); }
                    currentOption = num - 1;
                    const int indent = LeadingWhitespace(rawLine);
                    ++i;
                    parseOptionBlock(i, m.options[(size_t)currentOption], 0, indent);
                    // currentOption stays set: a "runmode N" right after the closing } belongs to this block
                    continue;
                }
            }
        }

        if (KeywordLine(wLine, "runmode", rest)) {
            const int mode = ParseInt(rest);
            if (currentOption >= 0 && currentOption < (int)m.options.size())
                m.options[(size_t)currentOption].runMode = (mode >= 0 && mode <= 2) ? mode : 0;
            ++i;
            continue;
        }

        if (KeywordLine(wLine, "save", rest)) {
            const std::string args = Trim(rest);
            if (args == "n") {
                save.previewOnly = true;
            } else {
                const size_t pos1 = args.find("--");
                if (pos1 != std::string::npos) {
                    const size_t pos2 = args.find("--", pos1 + 2);
                    if (pos2 != std::string::npos) {
                        save.filename = Trim(args.substr(pos1 + 2, pos2 - (pos1 + 2)));
                        save.path = Trim(args.substr(pos2 + 2));
                    } else {
                        save.filename = Trim(args.substr(pos1 + 2));
                    }
                }
            }
            ++i;
            continue;
        }
        ++i;
    }

    NormalizeParsedOptions(m.options);
    m.password = NormalizePassword(m.password);
    return true;
}

static bool ParseScriptConfig(const std::string& filePath, MenuModel& m, SaveDirective& save) {
    std::vector<std::string> lines;
    if (!ReadConsoleEditorTextFile(filePath, lines)) return false;
    return ParseScriptConfigLines(lines, m, save);
}

// Checks that a parsed config can be turned into a menu. Returns "" when fine, else the problem.
static std::string ValidateParsedMenu(const MenuModel& m) {
    if (m.options.empty()) return "No options were defined. Add \"option num: 2\" and an \"option 1 label ...\" / \"option 1 { ... }\" block for each option.";
    return "";
}

// ============================================================
// Text pager (a small "less")
// ============================================================
// Wraps one source line to 'width' columns. Continuation lines keep the line's indent.
static std::vector<std::string> WrapForDisplay(const std::string& line, int width) {
    std::vector<std::string> out;
    if (width < 8) width = 8;
    if (DisplayWidth(line) <= width) { out.push_back(line); return out; }
    const std::string indent = std::string((size_t)LeadingWhitespace(line), ' ') + (Trim(line).compare(0, 2, "//") == 0 ? "//   " : "  ");
    std::string current;
    std::string remaining = line;
    bool first = true;
    while (!remaining.empty()) {
        const std::string prefix = first ? "" : indent;
        const int avail = width - DisplayWidth(prefix);
        if (DisplayWidth(remaining) <= avail) { out.push_back(prefix + remaining); break; }
        // cut at the last space that fits
        std::u32string u = Utf8ToU32(remaining);
        int w = 0;
        size_t cut = 0, lastSpace = std::u32string::npos;
        for (size_t i = 0; i < u.size(); ++i) {
            const int cw = CharWidth(DisplayChar(u[i]));
            if (w + cw > avail) break;
            w += cw;
            cut = i + 1;
            if (u[i] == U' ' && i > (size_t)LeadingWhitespace(remaining)) lastSpace = i;
        }
        if (lastSpace != std::u32string::npos && lastSpace > 0) cut = lastSpace;
        if (cut == 0) cut = 1;
        out.push_back(prefix + RTrim(U32ToUtf8(u.substr(0, cut))));
        std::string tail = U32ToUtf8(u.substr(cut));
        size_t sp = 0;
        while (sp < tail.size() && tail[sp] == ' ') ++sp;
        remaining = tail.substr(sp);
        first = false;
    }
    return out;
}

static void ShowTextPager(const std::string& title, const std::vector<std::string>& source,
    std::function<std::string(const std::string&)> styler) {
    if (!g_stdinTty || !g_stdoutTty) {
        for (const auto& l : source) Out(l + "\n");
        return;
    }
    RawScope raw;
    AltScreenScope alt;
    CursorScope hideCursor(true);
    int top = 0;
    int lastCols = -1;
    std::vector<std::pair<std::string, std::string>> shown;   // text, style
    auto rewrap = [&]() {
        shown.clear();
        const int cols = std::max(20, TermCols()) - 2;
        for (const auto& l : source) {
            const std::string style = styler ? styler(l) : std::string();
            for (const auto& seg : WrapForDisplay(l, cols)) shown.push_back({ seg, style });
        }
        lastCols = TermCols();
    };
    while (true) {
        if (lastCols != TermCols()) rewrap();
        const int rows = std::max(3, TermRows() - 4);
        const int cols = std::max(20, TermCols());
        const int maxTop = std::max(0, (int)shown.size() - rows);
        top = std::max(0, std::min(top, maxTop));
        std::string o = "\033[H";
        o += "\033[2K" + ColorEsc(g_theme.header) + " " + FitToWidth(title, cols - 2) + ColorEsc(CC_RESET) + "\r\n";
        o += "\033[2K" + ColorEsc(g_theme.banner) + std::string((size_t)std::min(cols, 70), '-') + ColorEsc(CC_RESET) + "\r\n";
        for (int r = 0; r < rows; ++r) {
            const int i = top + r;
            o += "\033[2K";
            if (i < (int)shown.size()) o += " " + shown[(size_t)i].second + FitToWidth(shown[(size_t)i].first, cols - 2) + ColorEsc(CC_RESET);
            o += "\r\n";
        }
        char pos[96];
        snprintf(pos, sizeof(pos), " Lines %d-%d of %d", shown.empty() ? 0 : top + 1,
            std::min((int)shown.size(), top + rows), (int)shown.size());
        o += "\033[2K" + ColorEsc(g_theme.banner) + std::string((size_t)std::min(cols, 70), '-') + ColorEsc(CC_RESET) + "\r\n";
        o += "\033[2K" + ColorEsc(g_theme.info) + FitToWidth(std::string(pos) + "   Up/Down, PgUp/PgDn, Home/End scroll   Esc or q closes", cols - 1) + ColorEsc(CC_RESET);
        Out(o);
        Flush();

        const Key k = ReadKey();
        if (k.code == K_ESC || k.code == K_ENTER || k.code == K_EOF) return;
        if (k.code == K_CHAR && k.ctrl && k.ch == 'c') { Out("\n"); Quit(130); }
        if (k.code == K_CHAR && !k.ctrl && (k.ch == 'q' || k.ch == 'Q')) return;
        if (k.code == K_UP || (k.code == K_CHAR && !k.ctrl && k.ch == 'k')) --top;
        else if (k.code == K_DOWN || (k.code == K_CHAR && !k.ctrl && k.ch == 'j')) ++top;
        else if (k.code == K_PGUP) top -= rows;
        else if (k.code == K_PGDN || (k.code == K_CHAR && !k.ctrl && k.ch == ' ')) top += rows;
        else if (k.code == K_HOME) top = 0;
        else if (k.code == K_END) top = maxTop;
    }
}

// ============================================================
// Syntax tutorial
// ============================================================
static void ShowSyntaxTutorial() {
    static const char* kSyntaxTutorial = R"BMG_TUTORIAL(// SYNTAX: title [Your Menu Title Here] (Max 57 characters)
// EXAMPLE: title Admin Toolbox
// This field is not required, leave the title line out for a menu without a title (handy for ASCII art in the description)
title [your title here]

// SYNTAX: description
// Note: You can repeat this keyword on new lines to add multiple description paragraphs or multiline ASCII art.
// A line with only the word description (after the first one) adds a blank line.
// EXAMPLE: description Version 1.0 - Use with root privileges.
// This field is not required, simply don't add the description line to not include
description 

// SYNTAX: pass [Password]
// Note: Leave blank, or use 'n' or 'no' if you do not want a password.
// The password is stored in the script as a salted SHA-256 hash, never as plain text.
// EXAMPLE: pass MySecurePass123
// This field is not required
pass 

// SYNTAX: root [y or n]        (admin and sudo mean exactly the same and can be used instead)
// Note: Type 'root y' to force the script to request root privileges upon launch
// (it relaunches itself with sudo, or doas when sudo is missing).
// If not written, or set to 'n', it runs with standard user privileges unless manually launched as root.
root y/n

// CRITICAL: You MUST define this total number BEFORE writing individual option details!
// SYNTAX: option num: [number from 1 to 20]
// EXAMPLE: option num: 2
// This field is required
option num: [the number of options you want to have]

// Repeat the structure below for every option up to your total option number.

// --- OPTION 1 ---
// SYNTAX: option [Number] label [Menu Display Name]
// EXAMPLE: option 1 label Check Disk Usage
// This is required for every option up to the limit you set
option 1 label [your label]

// SYNTAX: option [Number] { [Your Bash Commands] }
// Note: Put every command on its own separate line inside the brackets.
// Note: Leave empty if runmode is set to 2 (Submenu).
// Note: A line with only } closes the block. If one of your commands needs such a line
//       (the end of a function, for example) indent your commands like below - a } that is
//       indented deeper than the "option N {" line belongs to your commands.
option 1 {
    [commands go here inside the brackets, in separate lines]
}

// SYNTAX: runmode [0, 1, or 2]
// Note: Place it immediately after the closing bracket '}' (or on its own line inside the brackets).
// 0 = Direct (your commands run inside the menu's own shell, so cd and variables carry over)
// 1 = bash -c wrapper (every command line runs in its own bash, isolated from the menu)
// 2 = Submenu (creates a nested sub-menu section)
// If omitted, runmode defaults to 0
runmode [for normal shell commands choose 0, for isolated commands choose 1, for submenus choose 2]


// --- OPTION 2 (EXAMPLE: SUBMENU / RUNMODE 2) ---
option 2 label [your submenu label]
option 2 {
    [leave empty for submenus]
}
runmode 2

// For Submenus (runmode 2), define nested sub-options (1-20 per submenu, max depth 6):
// SYNTAX: option [Parent] suboption [Number] label
// SYNTAX: option [Parent] suboption [Number] { [Submenu Commands] }
// SYNTAX: option [Parent] suboption [Number] runmode [0, 1, or 2]
// To go deeper, repeat suboption: option 2 suboption 1 suboption 2 label [your label]
option 2 suboption 1 label [your sub-option label]
option 2 suboption 1 {
    [sub-option commands go here]
}
option 2 suboption 1 runmode 0


// SYNTAX: border [character]
// EXAMPLE: border *
// Use the first character after "border" for menu borders.
// If this line is omitted, the default border character is =
border =

// SYNTAX OPTIONS FOR 'save':
//   save n                          -> Preview the script only (will not write to disk)
//   save --[filename.sh]            -> Auto-saves to the folder where this tool runs
//   save --[filename.sh]--[path]    -> Auto-saves to a specific target folder path
//   save --[filename.bin]--[path]   -> Compiles directly into a standalone binary
// EXAMPLE: save --my_new_menu.sh--/home/user/Desktop
// Configs written for BatchMenuGenerator work here too: .bat is saved as .sh and .exe as a binary.
save [your preferred option]

// BONUS: To compile ANY bash script into a binary, in your terminal type the command:
// bmg -compile "path-to-script.sh"
// OR
// BashMenuGenerator -compile "path-to-script.sh")BMG_TUTORIAL";
    ShowTextPager("Text Mode Syntax Tutorial", SplitLines(kSyntaxTutorial), [](const std::string& line) {
        if (line.compare(0, 2, "//") == 0) return ColorEsc(g_theme.info);
        return ColorEsc(g_theme.input);
    });
}

// ============================================================
// Writing a menu back out as Text Mode config lines (used by AutoSave)
// ============================================================
static void WriteOptionConfig(std::vector<std::string>& lines, const OptionData& opt, const std::string& prefix, int depth) {
    if (depth > 8) return;
    lines.push_back(prefix + " {");
    lines.push_back("runmode " + ToStr(opt.runMode));
    if (opt.runMode == 2) {
        if (!opt.subOptions.empty()) lines.push_back("suboption num: " + ToStr((long long)opt.subOptions.size()));
        for (size_t si = 0; si < opt.subOptions.size(); ++si) {
            const std::string n = ToStr((long long)si + 1);
            if (!opt.subOptions[si].label.empty()) lines.push_back("suboption " + n + " label " + opt.subOptions[si].label);
            WriteOptionConfig(lines, opt.subOptions[si], "suboption " + n, depth + 1);
        }
    } else {
        // indented so a command that is just "}" cannot end the block when the autosave is read again
        for (const auto& c : opt.commands) lines.push_back("    " + c);
    }
    lines.push_back("}");
}

static std::vector<std::string> BuildConfigLines(const MenuModel& m, const std::string& header, const std::string& stepCode) {
    std::vector<std::string> lines;
    lines.push_back(header);
    if (!stepCode.empty()) lines.push_back("step " + stepCode);
    lines.push_back("title " + m.title);
    for (const auto& d : m.description) lines.push_back(d.empty() ? "description" : "description " + d);
    if (!m.password.empty()) lines.push_back("pass " + m.password);
    lines.push_back(m.requireRoot ? "root y" : "root n");
    if (stepCode.empty() && !m.borderChar.empty()) lines.push_back("border " + m.borderChar);
    lines.push_back("option num: " + ToStr((long long)m.options.size()));
    for (size_t oi = 0; oi < m.options.size(); ++oi) {
        const std::string optNum = ToStr((long long)oi + 1);
        if (!m.options[oi].label.empty()) lines.push_back("option " + optNum + " label " + m.options[oi].label);
        WriteOptionConfig(lines, m.options[oi], "option " + optNum, 0);
    }
    return lines;
}

// ============================================================
// Password entry (hidden, typed twice)
// ============================================================
// Returns the new password record, "" for none. Sets 'changed' false when the user cancelled.
static std::string AskNewPassword(bool& entered) {
    entered = true;
    while (true) {
        C_P; Out("  Enter password (leave empty for none): "); C_I;
        const std::string first = ReadSecret(); C_R;
        if (first.empty()) return "";
        C_P; Out("  Repeat the password: "); C_I;
        const std::string second = ReadSecret(); C_R;
        if (first == second) return NormalizePassword(first);
        C_E; Out("  [Error] The passwords do not match. Try again.\n"); C_R;
    }
}

// ============================================================
// Wizard (answer questions to build a menu)
// ============================================================
static bool RunWizardFlow(MenuModel& m, const std::string& resumeStep = "") {
    int resumeStage = 0;
    size_t resumeOptIndex = 0;

    if (resumeStep == "TITLE_DONE") resumeStage = 1;
    else if (resumeStep == "DESC_LINE" || resumeStep == "DESC_DONE") resumeStage = 2;
    else if (resumeStep == "PASS_DONE") resumeStage = 3;
    else if (resumeStep == "ADMIN_DONE" || resumeStep == "ROOT_DONE") resumeStage = 4;
    else if (resumeStep == "OPTCOUNT_DONE") { resumeStage = 5; resumeOptIndex = 0; }
    else if (StartsWith(resumeStep, "OPT_")) {
        resumeStage = 5;
        if (StartsWith(resumeStep, "OPT_DONE_")) { const int idx = atoi(resumeStep.c_str() + 9); resumeOptIndex = idx >= 0 ? (size_t)(idx + 1) : 0; }
        else if (StartsWith(resumeStep, "OPT_LABEL_")) { const int idx = atoi(resumeStep.c_str() + 10); resumeOptIndex = idx >= 0 ? (size_t)idx : 0; }
        else if (StartsWith(resumeStep, "OPT_RUNMODE_")) { const int idx = atoi(resumeStep.c_str() + 12); resumeOptIndex = idx >= 0 ? (size_t)idx : 0; }
        else if (StartsWith(resumeStep, "OPT_CMD_")) { const int idx = atoi(resumeStep.c_str() + 8); resumeOptIndex = idx >= 0 ? (size_t)idx : 0; }
    } else if (resumeStep == "ALL_OPTIONS_DONE") {
        resumeStage = 6;
        resumeOptIndex = m.options.size();    // every option is already filled in
    }

    const std::string sessionTimestamp = TimestampNow("%H:%M %d/%m/%y");
    auto SaveStep = [&](const std::string& stepCode) {
        if (!g_appConfig.autosave) return;
        WriteAutoSave(BuildConfigLines(m, "// Autosave from " + sessionTimestamp, stepCode));
    };

    if (resumeStage < 1) {
        std::string entered = m.title;
        if (!ReadTitleWithPreview(entered)) return false;
        m.title = entered;
        SaveStep("TITLE_DONE");
    }

    if (resumeStage < 2) {
        bool wantDesc = true;
        if (g_enableTUI) {
            const int descChoice = RunTUIMenu("Add Description?", { "Yes, add description", "No, continue", "Exit" }, 0, -1, false);
            if (descChoice == -1 || descChoice == 2) Quit(0);
            wantDesc = (descChoice == 0);
        } else {
            C_P; Out("  Do you want to add a description? (Y/n) [X to exit]: "); C_I;
            const std::string a = Trim(ReadLine()); C_R;
            if (a == "X" || a == "x") Quit(0);
            wantDesc = a.empty() || IsYes(a);
        }
        if (wantDesc) {
            C_P; Out("  Enter description lines below.\n");
            Out("  Use this area for multiline ASCII art when no text title is wanted.\n");
            Out("  Press Enter on an empty line when done.\n"); C_R;
            while (true) {
                C_I; Out("    > "); const std::string line = ReadLine(); C_R;
                if (line.empty()) break;
                m.description.push_back(line);
                SaveStep("DESC_LINE");
            }
        }
        SaveStep("DESC_DONE");
    }

    if (resumeStage < 3) {
        C_P; Out("\n  [Note] The password is stored in the script as a salted SHA-256 hash, never as plain text.\n");
        Out("  Anyone who can edit the script can still remove the check. Compile the script to a binary to make\n");
        Out("  that harder. A compiled binary only obfuscates the script - it is not a cryptographic security boundary.\n"); C_R;
        bool entered = false;
        m.password = AskNewPassword(entered);
        SaveStep("PASS_DONE");
    }

    if (resumeStage < 4) {
        if (g_enableTUI) {
            const int rootChoice = RunTUIMenu("Root Privileges?", { "Yes - Require root privileges (sudo)", "No - Standard user privileges", "Exit" },
                m.requireRoot ? 0 : 1, -1, false);
            if (rootChoice == -1 || rootChoice == 2) Quit(0);
            m.requireRoot = (rootChoice == 0);
        } else {
            C_P; Out("\n  Will this script require root privileges? (y/N) [X to exit]: "); C_I;
            const std::string a = Trim(ReadLine()); C_R;
            if (a == "X" || a == "x") Quit(0);
            m.requireRoot = !a.empty() && IsYes(a);
        }
        if (m.requireRoot) { C_S; Out("  [Root elevation code will be added to the script]\n"); C_R; }
        SaveStep("ROOT_DONE");
    }

    if (resumeStage < 5) {
        int optionCount = 3;
        while (true) {
            C_P; Printf("  Number of options (1-%d) [3]: ", kMaxOptions); C_I;
            const std::string numStr = Trim(ReadOptionCountInput()); C_R;
            if (numStr.empty()) { optionCount = 3; break; }
            char* end = nullptr;
            const long val = strtol(numStr.c_str(), &end, 10);
            if (end && *end == '\0' && val >= 1 && val <= kMaxOptions) { optionCount = (int)val; break; }
            C_E; Printf("  [Error] Invalid number. Please enter a value between 1 and %d.\n", kMaxOptions); C_R;
        }
        m.options.resize((size_t)optionCount);
        SaveStep("OPTCOUNT_DONE");
    }

    for (size_t i = resumeOptIndex; i < m.options.size(); ++i) {
        OptionData& opt = m.options[i];
        C_N; Printf("\n  --- Option %zu ---\n", i + 1); C_R;

        C_P;
        if (!opt.label.empty()) Printf("  Label [Option %zu] [%s]: ", i + 1, opt.label.c_str());
        else Printf("  Label [Option %zu]: ", i + 1);
        C_I;
        const std::string label = ReadLine(); C_R;
        if (!label.empty()) opt.label = label;
        if (opt.label.empty()) opt.label = "Option";
        SaveStep("OPT_LABEL_" + ToStr((long long)i));

        opt.runMode = PromptRunMode(opt.runMode, true);
        SaveStep("OPT_RUNMODE_" + ToStr((long long)i));

        if (opt.runMode == 2) {
            opt.commands.clear();
            ConfigureSubmenuInteractive(opt.subOptions);
        } else {
            opt.subOptions.clear();
            if (!opt.commands.empty()) {
                C_S; Printf("  Existing commands for Option %zu:\n", i + 1);
                for (const auto& c : opt.commands) Out("    > " + c + "\n");
                C_R;
                C_P; Out("  Add more command(s), one per line (empty line when done):\n"); C_R;
            } else {
                C_P; Out("  Enter command(s), one per line.\n  Press Enter on an empty line when done.\n"); C_R;
            }
            while (true) {
                C_I; Out("    > "); const std::string cmd = ReadLine(); C_R;
                if (cmd.empty()) break;
                opt.commands.push_back(cmd);
                SaveStep("OPT_CMD_" + ToStr((long long)i));
            }
        }
        SaveStep("OPT_DONE_" + ToStr((long long)i));
    }

    SaveStep("ALL_OPTIONS_DONE");
    return true;
}

// ============================================================
// Script -> binary compiler
// ============================================================
// The binary contains the script scrambled with a random key per build (obfuscation, not encryption:
// do not rely on it to protect secrets). When it runs, the script is unpacked into an in-memory
// file (memfd) - or a private temp file when memfd is not available - and handed to its interpreter
// (bash unless the script's first line names another one). Arguments and the exit code pass through.
//
// Two ways to build it:
//   1. a small C launcher compiled with the system C compiler (cc, gcc, clang, tcc), or
//   2. when no compiler exists: a copy of this very program with the scrambled script appended
//      (the copy recognises the appended data at start-up and just runs the script).
static const char kPayloadMagic[8] = { 'B', 'M', 'G', 'P', 'A', 'Y', 'L', 'D' };

static std::string ScrambleScript(const std::string& plain, const unsigned char key[16]) {
    std::string cipher = plain;
    for (size_t i = 0; i < cipher.size(); ++i) {
        const unsigned char mix = (unsigned char)((i * 29u + 0x5Au) & 0xFFu);
        cipher[i] = (char)((unsigned char)cipher[i] ^ key[i % 16] ^ mix);
    }
    return cipher;
}

static std::string UnscrambleScript(const std::string& cipher, const unsigned char key[16]) {
    return ScrambleScript(cipher, key);      // XOR is its own inverse
}

static std::string BuildLauncherSource(const std::string& scrambled, const unsigned char key[16]) {
    std::string keyText, payloadText;
    for (int i = 0; i < 16; ++i) keyText += (i ? "," : "") + ToStr(key[i]);
    payloadText.reserve(scrambled.size() * 4 + 64);
    for (size_t i = 0; i < scrambled.size(); ++i) {
        payloadText += ToStr((unsigned char)scrambled[i]);
        payloadText += (i + 1 < scrambled.size()) ? "," : "";
        if ((i + 1) % 24 == 0) payloadText += "\n";
    }
    if (payloadText.empty()) payloadText = "0";

    std::string src = R"BMG(#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static const unsigned char K[16] = { @KEY@ };
static const unsigned char P[] = { @PAYLOAD@ };
#define N @LEN@UL

static int write_all(int fd, const unsigned char* d, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, d + off, n - off);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        off += (size_t)w;
    }
    return 0;
}

int main(int argc, char** argv) {
    unsigned char* s = (unsigned char*)malloc(N + 1);
    size_t i;
    int fd = -1, usedTemp = 0, na = 0, k;
    char path[PATH_MAX], tmpl[PATH_MAX], self[PATH_MAX], line[512];
    char* ia[8];
    char** args;
    ssize_t n;
    if (!s) return 127;
    for (i = 0; i < N; ++i) s[i] = (unsigned char)(P[i] ^ K[i % 16] ^ ((i * 29u + 0x5Au) & 0xFFu));
    s[N] = 0;

    /* interpreter: the script's #! line, otherwise bash */
    line[0] = 0;
    for (i = 0; i < N && i < sizeof(line) - 1 && s[i] != '\n'; ++i) line[i] = (char)s[i];
    line[i] = 0;
    if (line[0] == '#' && line[1] == '!') {
        char* p = line + 2;
        while (*p && na < 3) {
            while (*p == ' ' || *p == '\t') ++p;
            if (!*p) break;
            ia[na++] = p;
            while (*p && *p != ' ' && *p != '\t' && *p != '\r') ++p;
            if (*p) *p++ = 0;
        }
    }
    if (na == 0) { ia[0] = (char*)"bash"; na = 1; }

#ifdef SYS_memfd_create
    fd = (int)syscall(SYS_memfd_create, "bmg", 0u);
#endif
    if (fd >= 0) {
        if (write_all(fd, s, N) != 0) return 127;
        snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    } else {
        const char* d = getenv("TMPDIR");
        if (!d || !*d) d = "/tmp";
        snprintf(tmpl, sizeof(tmpl), "%s/bmgXXXXXX", d);
        fd = mkstemp(tmpl);
        if (fd < 0) { perror("bmg"); return 127; }
        if (write_all(fd, s, N) != 0) { close(fd); unlink(tmpl); return 127; }
        close(fd);
        chmod(tmpl, 0700);
        snprintf(path, sizeof(path), "%s", tmpl);
        usedTemp = 1;
    }
    memset(s, 0, N);
    free(s);

    n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n > 0) { self[n] = 0; setenv("BMG_SELF", self, 1); }

    args = (char**)malloc(sizeof(char*) * (size_t)(na + argc + 2));
    if (!args) return 127;
    for (k = 0; k < na; ++k) args[k] = ia[k];
    args[na] = path;
    for (k = 1; k < argc; ++k) args[na + k] = argv[k];
    args[na + argc] = NULL;

    if (!usedTemp) {
        execvp(args[0], args);
        perror(args[0]);
        return 127;
    } else {
        int st = 0;
        pid_t pid = fork();
        if (pid == 0) { execvp(args[0], args); perror(args[0]); _exit(127); }
        signal(SIGINT, SIG_IGN);
        signal(SIGQUIT, SIG_IGN);
        if (pid > 0) waitpid(pid, &st, 0);
        unlink(path);
        if (WIFEXITED(st)) return WEXITSTATUS(st);
        if (WIFSIGNALED(st)) return 128 + WTERMSIG(st);
        return 127;
    }
}
)BMG";
    src = ReplaceAll(src, "@KEY@", keyText);
    src = ReplaceAll(src, "@LEN@", ToStr((long long)scrambled.size()));
    src = ReplaceAll(src, "@PAYLOAD@", payloadText);
    return src;
}

struct CompilerInfo {
    std::string path;
    bool cppDriver = false;     // g++ / c++ / clang++: needs -x c
};

static bool FindCCompiler(CompilerInfo& info) {
    const std::string envCc = GetEnv("CC");
    if (!envCc.empty()) {
        const std::vector<std::string> words = SplitWords(envCc);
        if (!words.empty() && CommandExists(words[0])) { info.path = words[0]; return true; }
    }
    const char* names[] = { "cc", "gcc", "clang", "tcc", "c99" };
    for (const char* n : names) {
        const std::string p = FindInPath(n);
        if (!p.empty()) { info.path = p; return true; }
    }
    const char* cpp[] = { "g++", "c++", "clang++" };
    for (const char* n : cpp) {
        const std::string p = FindInPath(n);
        if (!p.empty()) { info.path = p; info.cppDriver = true; return true; }
    }
    return false;
}

// Appends the scrambled script (plus key, length and magic) to a copy of this program.
static bool PackScriptIntoProgramCopy(const std::string& plain, const std::string& outputPath, std::string& error) {
    const std::string self = GetExecutablePath();
    std::string program;
    if (self.empty() || !ReadBinaryFile(self, program)) { error = "Could not read the running program to build a copy."; return false; }
    // when this program is itself a packed binary, cut the old payload off first
    if (program.size() >= 32 && memcmp(program.data() + program.size() - 8, kPayloadMagic, 8) == 0) {
        uint64_t oldLen = 0;
        memcpy(&oldLen, program.data() + program.size() - 16, 8);
        if (oldLen + 32 <= program.size()) program.resize(program.size() - (size_t)oldLen - 32);
    }
    unsigned char key[16];
    const std::string keyHex = RandomHex(16);
    for (int i = 0; i < 16; ++i) key[i] = (unsigned char)strtol(keyHex.substr((size_t)i * 2, 2).c_str(), nullptr, 16);
    const std::string cipher = ScrambleScript(plain, key);
    std::string blob = program;
    blob += cipher;
    blob.append((const char*)key, 16);
    const uint64_t len = cipher.size();
    blob.append((const char*)&len, 8);
    blob.append(kPayloadMagic, 8);
    unlink(outputPath.c_str());
    if (!WriteBinaryFile(outputPath, blob, 0755)) { error = "Could not write " + outputPath + "."; return false; }
    return true;
}

static bool CompileScriptToBinary(const std::string& plain, const std::string& outputPath) {
    if (plain.empty()) { C_E; Out("  [Error] The script is empty.\n"); C_R; return false; }
    if (DirExists(outputPath)) { C_E; Out("  [Error] The output path is a directory.\n"); C_R; return false; }

    unsigned char key[16];
    const std::string keyHex = RandomHex(16);
    for (int i = 0; i < 16; ++i) key[i] = (unsigned char)strtol(keyHex.substr((size_t)i * 2, 2).c_str(), nullptr, 16);

    C_N; Out("  Compiling binary...\n"); C_R;
    bool ok = false;
    std::string method;
    std::string log;

    CompilerInfo cc;
    if (FindCCompiler(cc)) {
        const std::string tempDir = MakeTempDir("bmg_compile_");
        if (!tempDir.empty()) {
            const std::string srcPath = tempDir + "/launcher.c";
            const std::string tmpOut = tempDir + "/launcher.bin";
            if (WriteBinaryFile(srcPath, BuildLauncherSource(ScrambleScript(plain, key), key), 0600)) {
                std::string cmd = ShQuote(cc.path);
                if (cc.cppDriver) cmd += " -x c";
                cmd += " -O2 -w -o " + ShQuote(tmpOut) + " " + ShQuote(srcPath) + " 2>&1";
                const int rc = RunCommand({ "sh", "-c", cmd }, [&]() { ProcOpts o; o.out = &log; return o; }());
                if (rc == 0 && IsRegularFile(tmpOut)) {
                    std::string bin;
                    unlink(outputPath.c_str());
                    if (ReadBinaryFile(tmpOut, bin) && WriteBinaryFile(outputPath, bin, 0755)) {
                        ok = true;
                        method = "C launcher (" + BaseName(cc.path) + ")";
                    }
                }
            }
            RemoveTree(tempDir);
        }
        if (!ok) {
            C_E; Out("  [Note] The C compiler could not build the launcher");
            if (!Trim(log).empty()) Out(":\n" + Trim(log).substr(0, 600));
            Out("\n  Using the built-in packer instead.\n"); C_R;
        }
    }
    if (!ok) {
        std::string error;
        if (PackScriptIntoProgramCopy(plain, outputPath, error)) {
            ok = true;
            method = "built-in packer";
        } else {
            unlink(outputPath.c_str());
            C_E; Out("  [Error] Binary compilation failed: " + error + "\n"); C_R;
            return false;
        }
    }
    chmod(outputPath.c_str(), 0755);
    C_S; Out("  [SUCCESS] Compiled binary: " + outputPath + "\n"); C_R;
    C_N; Out("  (built with the " + method + ")\n"); C_R;
    return ok;
}

// Binary name suggested for a script: menu.sh -> menu (never the script's own name).
static std::string SuggestBinaryName(const std::string& scriptPath) {
    const std::string dir = DirName(scriptPath);
    std::string base = BaseNameNoExt(scriptPath);
    std::string candidate = JoinPath(dir, base);
    if (AbsolutePath(candidate) == AbsolutePath(scriptPath)) candidate += ".bin";
    return candidate;
}

// Asks where the binary should go. Returns false when cancelled.
static bool ChooseBinaryOutputPath(const std::string& suggestedSource, std::string& outputPath) {
    const std::string suggested = SuggestBinaryName(suggestedSource);
    const std::string startDir = DirName(suggested);
    const std::string picked = ChooseFile(DK_SAVE_FILE, "Compile Script to Binary", "", BaseName(suggested), startDir);
    if (picked.empty()) return false;
    outputPath = picked;
    if (EndsWithNoCase(outputPath, ".exe")) outputPath.resize(outputPath.size() - 4);
    if (PathExists(outputPath) && !DirExists(outputPath) && ResolveFileBrowser() == "builtin") {
        const int r = g_enableTUI ? RunTUIMenu("File already exists!", { "Replace", "Cancel" }, 0, -1, false) : (AskYesNo("File already exists. Replace it?", false) ? 0 : 1);
        if (r != 0) return false;
    }
    return true;
}

static bool CompileScriptToBinaryInteractive(const std::string& script, const std::string& suggestedSource) {
    std::string outputPath;
    if (!ChooseBinaryOutputPath(suggestedSource, outputPath)) {
        C_P; Out("  Binary compilation canceled.\n"); C_R;
        return false;
    }
    return CompileScriptToBinary(script, outputPath);
}

// ============================================================
// Runner mode: this program was packed with a script appended to it
// ============================================================
// Called first thing in main(). Returns only when no payload is attached.
static void RunEmbeddedPayloadIfPresent(int argc, char** argv) {
    const std::string self = GetExecutablePath();
    if (self.empty()) return;
    const int fd = open(self.c_str(), O_RDONLY);
    if (fd < 0) return;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 32) { close(fd); return; }
    unsigned char trailer[32];
    if (pread(fd, trailer, 32, st.st_size - 32) != 32 || memcmp(trailer + 24, kPayloadMagic, 8) != 0) { close(fd); return; }
    uint64_t len = 0;
    memcpy(&len, trailer + 16, 8);
    if (len == 0 || len + 32 > (uint64_t)st.st_size) { close(fd); return; }
    std::string cipher((size_t)len, '\0');
    size_t got = 0;
    while (got < len) {
        const ssize_t r = pread(fd, &cipher[got], len - got, st.st_size - 32 - (off_t)len + (off_t)got);
        if (r <= 0) { close(fd); return; }
        got += (size_t)r;
    }
    close(fd);
    const std::string script = UnscrambleScript(cipher, trailer);

    // interpreter from the first line
    std::vector<std::string> interp;
    const std::string firstLine = FirstLine(script);
    if (firstLine.compare(0, 2, "#!") == 0) {
        const std::vector<std::string> words = SplitWords(firstLine.substr(2));
        for (size_t i = 0; i < words.size() && i < 3; ++i) interp.push_back(words[i]);
    }
    if (interp.empty()) interp.push_back("bash");

    int sfd = -1;
    std::string path;
    bool usedTemp = false;
#ifdef SYS_memfd_create
    sfd = (int)syscall(SYS_memfd_create, "bmg", 0u);
#endif
    if (sfd >= 0) {
        size_t off = 0;
        while (off < script.size()) {
            const ssize_t w = write(sfd, script.data() + off, script.size() - off);
            if (w < 0) { if (errno == EINTR) continue; _exit(127); }
            off += (size_t)w;
        }
        path = "/proc/self/fd/" + ToStr(sfd);
    } else {
        std::string tmpl = EnsureTrailingSlash(GetEnv("TMPDIR").empty() ? "/tmp" : GetEnv("TMPDIR")) + "bmgXXXXXX";
        std::vector<char> buf(tmpl.begin(), tmpl.end());
        buf.push_back('\0');
        sfd = mkstemp(buf.data());
        if (sfd < 0) { perror("bmg"); _exit(127); }
        const ssize_t written = write(sfd, script.data(), script.size());
        close(sfd);
        if (written != (ssize_t)script.size()) { unlink(buf.data()); _exit(127); }
        chmod(buf.data(), 0700);
        path = buf.data();
        usedTemp = true;
    }
    setenv("BMG_SELF", self.c_str(), 1);

    std::vector<std::string> full = interp;
    full.push_back(path);
    for (int i = 1; i < argc; ++i) full.push_back(argv[i]);
    std::vector<char*> cargs;
    for (auto& a : full) cargs.push_back(const_cast<char*>(a.c_str()));
    cargs.push_back(nullptr);

    if (!usedTemp) {
        execvp(cargs[0], cargs.data());
        perror(cargs[0]);
        _exit(127);
    }
    const int rc = RunCommand(full);
    unlink(path.c_str());
    _exit(rc);
}

// ============================================================
// Generated Script Parser (reads a BashMenuGenerator script back into a MenuModel)
// ============================================================
static bool IsBashMenuGeneratorScript(const std::vector<std::string>& lines) {
    for (size_t i = 0; i < lines.size() && i < 6; ++i)
        if (lines[i].find("Created with BashMenuGenerator!") != std::string::npos) return true;
    return false;
}

struct ScriptFunc {
    size_t begin = 0;      // line index of the "name() {" header
    size_t end = 0;        // one past the last line that belongs to it
};

static bool ParseFunctionHeader(const std::string& line, std::string& name) {
    // "bmg_menu_3_1() {" or "bmg_run_2() {" at column 0
    if (!StartsWith(line, "bmg_menu_") && !StartsWith(line, "bmg_run_")) return false;
    const size_t paren = line.find("() {");
    if (paren == std::string::npos || RTrim(line) != line.substr(0, paren) + "() {") return false;
    name = line.substr(0, paren);
    return true;
}

static bool ParseGeneratedBashScript(const std::vector<std::string>& lines, MenuModel& m, std::string& whyNot) {
    m = MenuModel();
    whyNot.clear();

    std::map<std::string, ScriptFunc> funcs;
    std::string lastName;
    size_t mainLine = lines.size();
    for (size_t i = 0; i < lines.size(); ++i) {
        std::string name;
        if (ParseFunctionHeader(lines[i], name)) {
            if (!lastName.empty()) funcs[lastName].end = i;
            funcs[name].begin = i;
            lastName = name;
        } else if (StartsWith(lines[i], "bmg_main() {")) {
            mainLine = i;
            if (!lastName.empty()) funcs[lastName].end = i;
            lastName.clear();
        }
    }
    if (!lastName.empty()) funcs[lastName].end = lines.size();

    // settings block
    bool sawBorder = false;
    for (size_t i = 0; i < lines.size() && i < mainLine; ++i) {
        const std::string& l = lines[i];
        if (StartsWith(l, "bmg_")) { if (l.find("() {") != std::string::npos) break; continue; }
        size_t pos = 0;
        std::string value;
        if (StartsWith(l, "BMG_TITLE=")) { pos = 10; if (ShUnquoteAt(l, pos, value)) m.title = value; }
        else if (StartsWith(l, "BMG_BORDER=")) {
            pos = 11;
            if (ShUnquoteAt(l, pos, value) && !value.empty()) { m.borderChar = FirstChar(value); sawBorder = true; }
        }
        else if (StartsWith(l, "BMG_ROOT=")) m.requireRoot = Trim(l.substr(9)) == "1";
        else if (StartsWith(l, "BMG_PASS=")) { pos = 9; if (ShUnquoteAt(l, pos, value)) m.password = value; }
        else if (StartsWith(l, "BMG_DESC=(")) {
            if (Trim(l.substr(10)) == ")") continue;       // BMG_DESC=()
            for (size_t j = i + 1; j < lines.size(); ++j) {
                if (lines[j] == ")") { i = j; break; }
                const std::string t = lines[j];
                size_t q = t.find('\'');
                if (q == std::string::npos) continue;
                std::string v;
                size_t p2 = q;
                if (ShUnquoteAt(t, p2, v)) m.description.push_back(v);
            }
        }
    }
    if (!sawBorder) m.borderChar = "=";

    std::function<bool(const std::string&, std::vector<OptionData>&, int)> parseMenu;
    parseMenu = [&](const std::string& menuId, std::vector<OptionData>& out, int depth) -> bool {
        if (depth > 8) return false;
        const std::string fname = "bmg_menu_" + (menuId.empty() ? std::string("main") : menuId);
        auto it = funcs.find(fname);
        if (it == funcs.end()) return false;
        std::vector<std::string> labels;
        for (size_t i = it->second.begin; i < it->second.end; ++i) {
            const std::string t = Trim(lines[i]);
            if (!StartsWith(t, "bmg_item ")) continue;
            const std::string numPart = t.substr(9, t.find(' ', 9) - 9);
            if (numPart.empty() || !std::isdigit((unsigned char)numPart[0])) continue;      // the X) Exit / Back line
            size_t pos = t.find('\'');
            std::string label;
            if (pos == std::string::npos || !ShUnquoteAt(t, pos, label)) label = "Option";
            labels.push_back(label);
        }
        for (size_t i = 0; i < labels.size(); ++i) {
            OptionData opt;
            opt.label = labels[i];
            const std::string childId = OptionChildId(menuId, i);
            if (funcs.count("bmg_menu_" + childId)) {
                opt.runMode = 2;
                if (!parseMenu(childId, opt.subOptions, depth + 1)) return false;
            } else {
                auto rit = funcs.find("bmg_run_" + childId);
                if (rit != funcs.end()) {
                    int mode = 0;
                    bool inBlock = false, emptySub = false;
                    for (size_t j = rit->second.begin; j < rit->second.end; ++j) {
                        const std::string& l = lines[j];
                        if (!inBlock && StartsWith(Trim(l), "# >>> submenu")) { emptySub = true; break; }
                        if (!inBlock && StartsWith(l, "# >>> commands")) {
                            inBlock = true;
                            const size_t rm = l.find("runmode=");
                            if (rm != std::string::npos) mode = atoi(l.c_str() + rm + 8) == 1 ? 1 : 0;
                            continue;
                        }
                        if (inBlock && l == "# <<< commands") { inBlock = false; break; }
                        if (inBlock) {
                            if (mode == 1 && StartsWith(l, "bash -c ")) {
                                size_t pos = 8;
                                std::string cmd;
                                if (ShUnquoteAt(l, pos, cmd)) { opt.commands.push_back(cmd); continue; }
                            }
                            opt.commands.push_back(l);
                        }
                    }
                    opt.runMode = emptySub ? 2 : mode;
                }
            }
            out.push_back(opt);
        }
        return true;
    };

    if (!parseMenu("", m.options, 0)) {
        whyNot = "The menu structure (bmg_menu_main and friends) was not found or was changed too much.";
        return false;
    }
    return true;
}

// ============================================================
// Interactive script editor
// ============================================================
struct OptionTreeRow {
    std::string text;
    std::vector<size_t> path;
};

static void BuildOptionTreeRows(const std::vector<OptionData>& options, std::vector<OptionTreeRow>& rows,
    std::vector<size_t> path = {}, int depth = 0) {
    for (size_t i = 0; i < options.size(); ++i) {
        std::vector<size_t> currentPath = path;
        currentPath.push_back(i);
        const OptionData& opt = options[i];
        const std::string label = opt.label.empty() ? OptionFallbackLabel(i) : opt.label;
        std::string mode = "RunMode " + ToStr(opt.runMode);
        if (opt.runMode == 2) mode += " / Sub menu";
        else mode += ", " + ToStr((long long)opt.commands.size()) + " cmd(s)";
        std::string prefix;
        if (depth > 0) prefix = std::string((size_t)depth * 4, ' ') + "\xE2\x94\x94\xE2\x94\x80 ";     // └─
        rows.push_back({ prefix + "Option " + ToStr((long long)i + 1) + ": [" + label + "] [" + mode + "]", currentPath });
        if (opt.runMode == 2 && !opt.subOptions.empty()) BuildOptionTreeRows(opt.subOptions, rows, currentPath, depth + 1);
    }
}

static OptionData* GetOptionByPath(std::vector<OptionData>& roots, const std::vector<size_t>& path) {
    if (path.empty() || path[0] >= roots.size()) return nullptr;
    OptionData* cur = &roots[path[0]];
    for (size_t p = 1; p < path.size(); ++p) {
        if (cur->runMode != 2 || path[p] >= cur->subOptions.size()) return nullptr;
        cur = &cur->subOptions[path[p]];
    }
    return cur;
}

static std::vector<OptionData>* GetOptionContainer(std::vector<OptionData>& roots, const std::vector<size_t>& path) {
    if (path.empty()) return nullptr;
    if (path.size() == 1) return &roots;
    const std::vector<size_t> parentPath(path.begin(), path.end() - 1);
    OptionData* parent = GetOptionByPath(roots, parentPath);
    if (!parent || parent->runMode != 2) return nullptr;
    return &parent->subOptions;
}

static OptionData MakeNewOptionInteractive(size_t oneBasedIndex, const std::string& heading = "New Option") {
    OptionData result;
    C_N; Out("\n  --- " + heading + " ---\n"); C_R;
    C_P; Printf("  Label [Option %zu]: ", oneBasedIndex); C_I;
    result.label = ReadLine(); C_R;
    if (result.label.empty()) result.label = "Option " + ToStr((long long)oneBasedIndex);
    result.runMode = PromptRunMode(0, true);
    if (result.runMode != 2) EditLinesWithConsoleEditor(result.commands, true);
    return result;
}

static void FlashMessage(const std::string& message, bool error, int ms = 800) {
    if (error) { C_E; } else { C_S; }
    Out("  " + message + "\n"); C_R;
    Sleep(ms);
}

// Shows the edit actions for one option. Returns when the user goes back or deletes it.
static void EditOptionNode(std::vector<OptionData>& options, const std::vector<size_t>& selectedPath) {
    OptionData* selected = GetOptionByPath(options, selectedPath);
    while (selected) {
        const std::string label = selected->label.empty() ? "Option" : selected->label;
        std::vector<std::string> actions = { "Edit Label", "Change RunMode (0/1/2)", "View/Edit Commands", "Add Command" };
        if (selected->runMode == 2) actions.push_back("Add Submenu Option");
        actions.push_back("Delete This Option");
        actions.push_back("Back");

        const int action = RunTUIMenu("Edit Option: " + label, actions);
        if (action == -1 || action == (int)actions.size() - 1) return;
        const int addChildAction = (selected->runMode == 2) ? 4 : -1;
        const int deleteAction = (int)actions.size() - 2;

        if (action == 0) {
            C_P; Out("  New label [" + label + "]: "); C_I;
            const std::string nl = ReadLine(); C_R;
            if (!nl.empty()) selected->label = nl;
        } else if (action == 1) {
            const int newMode = PromptRunMode(selected->runMode, true);
            selected->runMode = newMode;
            if (newMode == 2) selected->commands.clear();
            else selected->subOptions.clear();
        } else if (action == 2) {
            EditCommandsTUI(selected->commands);
        } else if (action == 3) {
            if (selected->runMode == 2) { FlashMessage("A sub menu has no commands. Use \"Add Submenu Option\".", true, 1200); }
            else {
                C_P; Out("  Command to add: "); C_I;
                const std::string cmd = ReadLine(); C_R;
                if (!Trim(cmd).empty()) selected->commands.push_back(cmd);
            }
        } else if (action == addChildAction && addChildAction >= 0) {
            if (selectedPath.size() >= (size_t)kMaxDepth) { FlashMessage("Maximum submenu depth (" + ToStr(kMaxDepth) + ") reached.", true); continue; }
            if (selected->subOptions.size() >= (size_t)kMaxOptions) { FlashMessage("Maximum " + ToStr(kMaxOptions) + " submenu options reached.", true); continue; }
            OptionData created = MakeNewOptionInteractive(selected->subOptions.size() + 1, "New Submenu Option");
            selected = GetOptionByPath(options, selectedPath);
            if (selected) selected->subOptions.push_back(created);
        } else if (action == deleteAction) {
            std::vector<OptionData>* container = GetOptionContainer(options, selectedPath);
            if (container && selectedPath.size() == 1 && container->size() <= 1) {
                FlashMessage("A menu needs at least one option.", true, 1000);
                continue;
            }
            if (container && selectedPath.back() < container->size()) {
                container->erase(container->begin() + (std::ptrdiff_t)selectedPath.back());
                FlashMessage("Option deleted from the tree.", false, 700);
            }
            return;
        }
        // Re-resolve after any change so a reallocated vector never leaves a stale pointer.
        selected = GetOptionByPath(options, selectedPath);
    }
}

static void ManageOptionsTreeTUI(std::vector<OptionData>& options) {
    int cursor = 0;
    while (true) {
        std::vector<OptionTreeRow> rows;
        BuildOptionTreeRows(options, rows);
        std::vector<std::string> menu;
        std::vector<std::vector<size_t>> paths;
        for (const auto& row : rows) { menu.push_back(row.text); paths.push_back(row.path); }
        menu.push_back("[ Add Root Option ]");
        const int addRootAction = (int)menu.size() - 1;
        menu.push_back("[ Back ]");
        const int backAction = (int)menu.size() - 1;

        cursor = RunTUIMenu("Manage Options (expanded tree)", menu, std::min(cursor, (int)menu.size() - 1));
        if (cursor == -1 || cursor == backAction) return;
        if (cursor == addRootAction) {
            if (options.size() >= (size_t)kMaxOptions) { FlashMessage("Maximum " + ToStr(kMaxOptions) + " root options reached.", true); continue; }
            options.push_back(MakeNewOptionInteractive(options.size() + 1));
            continue;
        }
        if (cursor >= 0 && cursor < (int)paths.size()) EditOptionNode(options, paths[(size_t)cursor]);
    }
}

static void ManageOptionsTreeCLI(std::vector<OptionData>& options) {
    while (true) {
        C_H; Out("\n  --- Manage Options (expanded tree) ---\n"); C_R;
        std::vector<OptionTreeRow> rows;
        BuildOptionTreeRows(options, rows);
        for (size_t i = 0; i < rows.size(); ++i) { C_P; Printf("    %zu) ", i + 1); C_T; Out(rows[i].text + "\n"); C_R; }
        C_P; Out("\n    A) Add Root Option\n    E) Edit Option (use tree number)\n    D) Delete Option (use tree number)\n    0) Back\n  Select: "); C_I;
        const std::string cmd = Trim(ReadLine()); C_R;
        if (cmd == "0") return;
        if (cmd == "A" || cmd == "a") {
            if (options.size() >= (size_t)kMaxOptions) { FlashMessage("Maximum " + ToStr(kMaxOptions) + " root options reached.", true); continue; }
            options.push_back(MakeNewOptionInteractive(options.size() + 1));
            continue;
        }
        const bool isEdit = (cmd == "E" || cmd == "e");
        const bool isDelete = (cmd == "D" || cmd == "d");
        if (!isEdit && !isDelete) continue;
        C_P; Out("  Enter tree number: "); C_I;
        const int n = atoi(Trim(ReadLine()).c_str()); C_R;
        if (n < 1 || n > (int)rows.size()) continue;
        const std::vector<size_t> path = rows[(size_t)n - 1].path;
        OptionData* selected = GetOptionByPath(options, path);
        if (!selected) continue;
        if (isDelete) {
            std::vector<OptionData>* container = GetOptionContainer(options, path);
            if (container && path.size() == 1 && container->size() <= 1) { FlashMessage("A menu needs at least one option.", true, 1000); continue; }
            if (container && path.back() < container->size()) container->erase(container->begin() + (std::ptrdiff_t)path.back());
        } else {
            C_P; Out("  New Label [" + selected->label + "]: "); C_I;
            const std::string nl = ReadLine(); C_R;
            if (!nl.empty()) selected->label = nl;
            const int newMode = PromptRunMode(selected->runMode, true);
            selected->runMode = newMode;
            if (newMode == 2) selected->commands.clear(); else selected->subOptions.clear();
            if (newMode != 2) EditCommandsTUI(selected->commands);
        }
    }
}

static bool CompileCurrentEditorToBinary(const MenuModel& m, const std::string& loadedFilePath) {
    const std::string script = GenerateBash(m.title, m.description, m.options, m.password, m.requireRoot, m.borderChar);
    const std::string suggestedSource = loadedFilePath.empty() ? JoinPath(CurrentDir(), "BashMenu.sh") : loadedFilePath;
    return CompileScriptToBinaryInteractive(script, suggestedSource);
}

static bool SelectBorderStyle(std::string& borderChar) {
    const int bIdx = RunTUIMenu("Select Border Style", { "Classic (=)", "Modern (-)", "Zigzag (Z)", "Custom Char" });
    if (bIdx == 0) borderChar = "=";
    else if (bIdx == 1) borderChar = "-";
    else if (bIdx == 2) borderChar = "Z";
    else if (bIdx == 3) {
        C_P; Out("  Enter custom border character: "); C_I;
        const std::string bc = ReadLine(); C_R;
        if (!bc.empty()) borderChar = FirstChar(bc);
    } else return false;
    return true;
}

static bool RunScriptEditor(MenuModel& m, const std::string& loadedFilePath) {
    auto saveChangesToScript = [&]() {
        const std::string script = GenerateBash(m.title, m.description, m.options, m.password, m.requireRoot, m.borderChar);
        if (!WriteScriptToFile(loadedFilePath, script)) {
            C_E; Out("  [Error] Could not save changes to: " + loadedFilePath + "\n"); C_R;
            return false;
        }
        C_S; Out("  Changes saved to: " + loadedFilePath + "\n"); C_R;
        return true;
    };
    auto changePassword = [&]() {
        bool entered = false;
        const std::string np = AskNewPassword(entered);
        if (np.empty()) FlashMessage("Password removed!", false, 700);
        else FlashMessage("Password updated!", false, 700);
        m.password = np;
    };

    int editorCursor = 0;       // the menu keeps its position after each change
    while (true) {
        ClearScreen();
        C_B; Out(BannerRule() + "\n"); C_N;
        Out(BannerTitle("BashMenuGenerator Editor") + "\n"); C_B;
        Out(BannerRule() + "\n\n"); C_T;

        if (g_enableTUI) {
            std::vector<std::string> opts;
            opts.push_back("Title          : " + (m.title.empty() ? std::string("(None)") : m.title));
            opts.push_back("Description    : " + (m.description.empty() ? std::string("(None)") : ToStr((long long)m.description.size()) + " line(s)"));
            opts.push_back("Password       : " + PasswordDisplay(m.password));
            opts.push_back(std::string("Root Privilege : ") + (m.requireRoot ? "YES (Enabled)" : "NO (Disabled)"));
            opts.push_back("Border Style   : " + m.borderChar);
            opts.push_back("Manage Options : " + ToStr((long long)m.options.size()) + " option(s)");
            opts.push_back("Compile to binary");
            opts.push_back("[ Save Changes to Script ]");
            opts.push_back("[ Cancel / Return ]");

            const int choice = RunTUIMenu("Script Editor", opts, editorCursor);
            if (choice == -1 || choice == 8) return false;
            editorCursor = choice;
            if (choice == 0) { std::string nt = m.title; if (ReadTitleWithPreview(nt)) m.title = nt; }
            else if (choice == 1) EditDescriptionTUI(m.description);
            else if (choice == 2) {
                const int pChoice = RunTUIMenu("Password Options", { "Set / Change Password", "Remove Password", "Back" });
                if (pChoice == 0) changePassword();
                else if (pChoice == 1) { m.password.clear(); FlashMessage("Password removed!", false, 700); }
            }
            else if (choice == 3) m.requireRoot = !m.requireRoot;
            else if (choice == 4) SelectBorderStyle(m.borderChar);
            else if (choice == 5) ManageOptionsTreeTUI(m.options);
            else if (choice == 6) { CompileCurrentEditorToBinary(m, loadedFilePath); PressAnyKey(); }
            else if (choice == 7) { if (saveChangesToScript()) return true; PressAnyKey(); }
        } else {
            C_P; Out("    1) Title           : "); C_I; Out((m.title.empty() ? std::string("(None)") : m.title) + "\n");
            C_P; Out("    2) Description     : "); C_I; Out((m.description.empty() ? std::string("(None)") : ToStr((long long)m.description.size()) + " line(s)") + "\n");
            C_P; Out("    3) Password        : "); C_I; Out(PasswordDisplay(m.password) + "\n");
            C_P; Out("    4) Root Privileges : "); C_I; Out(std::string(m.requireRoot ? "YES (Enabled)" : "NO (Disabled)") + "\n");
            C_P; Out("    5) Border Style    : "); C_I; Out(m.borderChar + "\n");
            C_P; Out("    6) Manage Options  : "); C_I; Out(ToStr((long long)m.options.size()) + " option(s)\n");
            C_P; Out("    C) Compile to binary\n    S) Save Changes to Script\n    0) Cancel\n\n  Select option: "); C_I;
            const std::string edChoice = Trim(ReadLine()); C_R;
            if (edChoice == "0") return false;
            if (edChoice == "S" || edChoice == "s") { if (saveChangesToScript()) return true; continue; }
            if (edChoice == "C" || edChoice == "c") { CompileCurrentEditorToBinary(m, loadedFilePath); PressAnyKey(); continue; }
            if (edChoice == "1") { std::string nt = m.title; if (ReadTitleWithPreview(nt)) m.title = nt; }
            else if (edChoice == "2") EditLinesWithConsoleEditor(m.description);
            else if (edChoice == "3") {
                C_P; Out("  Password Options:\n    1) Set/Change Password\n    2) Remove Password\n  Select [1]: "); C_I;
                const std::string pc = Trim(ReadLine()); C_R;
                if (pc == "2") m.password.clear(); else changePassword();
            }
            else if (edChoice == "4") m.requireRoot = !m.requireRoot;
            else if (edChoice == "5") {
                C_P; Out("  Enter border character (=, -, Z, or custom): "); C_I;
                const std::string bc = ReadLine(); C_R;
                if (!bc.empty()) m.borderChar = FirstChar(bc);
            }
            else if (edChoice == "6") ManageOptionsTreeCLI(m.options);
        }
    }
}

// Picks a generated script, parses it and opens the editor. Returns true when changes were saved.
static bool OpenGeneratedScriptEditor(MenuModel& m, std::string& finalSavedPath, const std::string& requestedPath = "") {
    std::string path = requestedPath;
    if (path.empty()) {
        std::string method;
        if (g_appConfig.bconfig == 1 && g_appConfig.saveload > 0) method = (g_appConfig.saveload == 1) ? "1" : "2";
        if (method.empty()) {
            if (g_enableTUI) {
                const int lc = RunTUIMenu("Choose Script to Edit", { "Browse (file browser)", "Type path manually", "Cancel" });
                if (lc == 0) method = "1"; else if (lc == 1) method = "2"; else return false;
            } else {
                C_P; Out("\n  Choose Load Method:\n    1 = Browse\n    2 = Type path\n    3 = Cancel\n  Select [1]: "); C_I;
                const std::string lc = Trim(ReadLine()); C_R;
                if (lc == "3") return false;
                method = (lc == "2") ? "2" : "1";
            }
        }
        if (method == "1") {
            path = ChooseFile(DK_OPEN_FILE, "Select Generated Bash Script", "sh");
            if (path.empty()) { C_P; Out("\n  Dialog canceled.\n"); C_R; return false; }
        } else {
            path = PromptPathManually("\n  Enter script path: ");
        }
    }
    if (path.empty()) return false;
    path = ExpandTilde(StripQuotes(path));

    std::vector<std::string> sLines;
    if (!ReadConsoleEditorTextFile(path, sLines)) {
        C_E; Out("\n  [Error] Could not open file: " + path + "\n"); C_R;
        return false;
    }
    if (!IsBashMenuGeneratorScript(sLines)) {
        C_E; Out("\n  [Error] This script was not created by BashMenuGenerator!\n");
        Out("  Missing the required watermark: '# Created with BashMenuGenerator!'\n"); C_R;
        return false;
    }
    std::string whyNot;
    if (!ParseGeneratedBashScript(sLines, m, whyNot)) {
        C_E; Out("\n  [Error] Failed to read the generated script structure.\n"); C_R;
        C_N; Out("  " + whyNot + "\n  Scripts made by older builds use a different layout and cannot be edited;\n  re-create them with the wizard or Text Mode.\n"); C_R;
        return false;
    }
    if (!RunScriptEditor(m, path)) return false;
    finalSavedPath = path;
    return true;
}

// ============================================================
// Helpers for the main flow
// ============================================================
static bool g_debugMode = false;

// Preview text: the runtime helper block is identical in every script, so it is collapsed.
static std::string PreviewText(const std::string& script, bool collapse) {
    if (!collapse) return script;
    const std::vector<std::string> lines = SplitLines(script);
    size_t start = lines.size(), end = lines.size();
    for (size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].find("# Runtime - the same in every script") != std::string::npos && i > 0) { start = i - 1; }
        if (StartsWith(lines[i], "bmg_menu_main() {")) { end = i; break; }
    }
    if (start >= end) return script;
    std::string out;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i == start) {
            out += "# [" + ToStr((long long)(end - start)) + " lines of runtime helper functions are identical in every script and are not shown here]\n\n";
        }
        if (i >= start && i < end) continue;
        out += lines[i] + "\n";
    }
    return out;
}

// Runs a script in this terminal and waits for it.
static void LaunchScript(const std::string& path) {
    C_N; Out("\n  Launching " + path + " ...\n"); C_R;
    Flush();
    Sleep(300);
    const int rc = RunCommand({ "bash", path });
    ClearScreen();
    if (rc == 0) { C_S; Out("  [Script finished]\n"); C_R; }
    else { C_N; Printf("  [Script finished with exit code %d]\n", rc); C_R; }
    PressAnyKey("Press any key to continue...");
}

static bool IsBinarySaveName(const std::string& name) {
    return EndsWithNoCase(name, ".bin") || EndsWithNoCase(name, ".exe") || EndsWithNoCase(name, ".elf") || EndsWithNoCase(name, ".out");
}

// `save --name.bin--path` compiles straight to a standalone binary.
static bool SaveConfigScriptAsBinary(const std::string& script, const std::string& filename, const std::string& path) {
    std::string name = Trim(filename);
    if (EndsWithNoCase(name, ".exe")) name.resize(name.size() - 4);
    if (name.empty() || name.find('/') != std::string::npos) { C_E; Out("  [Error] Invalid output file name.\n"); C_R; return false; }
    std::string dir = Trim(path);
    if (dir.empty()) dir = CurrentDir();
    dir = ExpandTilde(dir);
    if (dir[0] != '/') dir = JoinPath(CurrentDir(), dir);
    if (!DirExists(dir) && !CreateDirectoryChain(dir)) { C_E; Out("  [Error] Could not create the output folder: " + dir + "\n"); C_R; return false; }
    return CompileScriptToBinary(script, JoinPath(dir, name));
}

// ============================================================
// Text Mode
// ============================================================
enum TextModeResult { TM_BUILD, TM_BACK, TM_FATAL };

static std::string TextModeScratchPath() { return GetAppDataDir() + "bmg_text_mode.txt"; }

static TextModeResult RunTextMode(MenuModel& m, SaveDirective& sv, int& startupMode, const std::string& startupPath) {
    int txtChoice = -1;
    std::vector<std::string> keptEditorText;      // survives a failed attempt so nothing typed is lost

    while (true) {
        if (startupMode == 2) {
            if (!ParseScriptConfig(startupPath, m, sv)) {
                C_E; Out("  [Error] Failed to load config file: " + startupPath + "\n"); C_R;
                return TM_FATAL;
            }
            startupMode = 0;
            const std::string problem = ValidateParsedMenu(m);
            if (!problem.empty()) { C_E; Out("  [Error] " + problem + "\n"); C_R; return TM_FATAL; }
            return TM_BUILD;
        } else if (startupMode == 1) {
            startupMode = 0;
            txtChoice = 1;
        } else if (g_enableTUI) {
            std::vector<std::string> editorArgv;
            std::string editorName;
            if (!ResolveEditor(editorArgv, editorName)) editorName = "a text editor";
            txtChoice = RunTUIMenu("Text Mode", {
                "Edit in " + editorName,
                "Edit in CLI",
                "Load TXT config",
                "Show syntax tutorial",
                "Exit to Main Menu",
                "Exit Program & Save"
            });
        } else {
            ClearScreen();
            C_P; Out("\n  Text Mode:\n    1) Edit in text editor\n    2) Edit in CLI\n    3) Load TXT config\n    4) Show syntax tutorial\n    5) Exit to Main Menu\n    X) Exit Program & Save\n  Select: "); C_I;
            const std::string a = Trim(ReadLine()); C_R;
            if (a == "1") txtChoice = 0;
            else if (a == "2") txtChoice = 1;
            else if (a == "3") txtChoice = 2;
            else if (a == "4") txtChoice = 3;
            else if (a == "5") txtChoice = 4;
            else if (a == "X" || a == "x") txtChoice = 5;
            else txtChoice = -1;
        }

        if (txtChoice == 4 || txtChoice == -1) return TM_BACK;
        if (txtChoice == 5) Quit(0);

        if (txtChoice == 0) {
            const std::string tmp = TextModeScratchPath();
            if (!PathExists(tmp)) {
                WriteBinaryFile(tmp, "// BashMenuGenerator Text Mode - lines starting with // are ignored.\n"
                                     "// Pick \"Show syntax tutorial\" in the Text Mode menu for the full reference.\n", 0600);
            }
            if (!OpenInEditorAndWait(tmp)) { PressAnyKey(); continue; }
            if (ParseScriptConfig(tmp, m, sv)) {
                const std::string problem = ValidateParsedMenu(m);
                if (problem.empty()) return TM_BUILD;
                C_E; Out("  [Error] " + problem + "\n"); C_R;
            } else {
                C_E; Out("  [Error] Failed to read the editor file.\n"); C_R;
            }
            PressAnyKey();
        } else if (txtChoice == 1) {
            while (true) {
                std::vector<std::string> editorLines = keptEditorText;
                const bool build = ConsoleTextEditor(editorLines, "", "Build the menu now");
                if (!build) break;
                keptEditorText = editorLines;
                if (ParseScriptConfigLines(editorLines, m, sv)) {
                    const std::string problem = ValidateParsedMenu(m);
                    if (problem.empty()) return TM_BUILD;
                    C_E; Out("\n  [Error] " + problem + "\n"); C_R;
                } else {
                    C_E; Out("\n  [Error] Failed to process editor text!\n"); C_R;
                }
                const int r = g_enableTUI ? RunTUIMenu("Fix the text?", { "Yes, reopen the editor (your text is kept)", "No, go back" }, 0, -1, false)
                                          : (AskYesNo("Reopen the editor with your text?", true) ? 0 : 1);
                if (r != 0) break;
            }
        } else if (txtChoice == 2) {
            bool loaded = false;
            std::string loadChoice;
            if (g_appConfig.bconfig == 1 && g_appConfig.saveload > 0) loadChoice = (g_appConfig.saveload == 1) ? "1" : "2";
            while (!loaded) {
                if (loadChoice.empty()) {
                    if (g_enableTUI) {
                        const int lc = RunTUIMenu("Choose Load Method", { "Browse (file browser)", "Type path manually", "Cancel" });
                        if (lc == 2 || lc == -1) break;
                        loadChoice = (lc == 0) ? "1" : "2";
                    } else {
                        C_P; Out("\n  Choose Load Method:\n    1 = Browse\n    2 = Type path\n    3 = Cancel\n  Select [1]: "); C_I;
                        loadChoice = Trim(ReadLine()); C_R;
                        if (loadChoice == "3") break;
                        if (loadChoice.empty()) loadChoice = "1";
                    }
                }
                if (loadChoice != "1" && loadChoice != "2") { loadChoice.clear(); continue; }

                std::string configPath;
                if (loadChoice == "1") {
                    configPath = ChooseFile(DK_OPEN_FILE, "Select Configuration File", "txt");
                    if (configPath.empty()) { C_P; Out("\n  Dialog canceled.\n"); C_R; loadChoice = "2"; continue; }
                } else {
                    configPath = PromptPathManually("\n  Enter config file path: ");
                }
                configPath = ExpandTilde(StripQuotes(configPath));
                std::string problem;
                bool ok = !configPath.empty() && ParseScriptConfig(configPath, m, sv);
                if (ok) { problem = ValidateParsedMenu(m); ok = problem.empty(); }
                if (!ok) {
                    C_E; Out("  [Error] " + (problem.empty() ? std::string("Failed to load config file.") : problem) + "\n"); C_R;
                    if (g_enableTUI) {
                        const int r = RunTUIMenu("Error loading file", { "Try again", "Cancel" });
                        if (r == 1 || r == -1) break;
                    } else {
                        C_P; Out("    1) Try again\n    2) Cancel\n  Select [1]: "); C_I;
                        const std::string retry = Trim(ReadLine()); C_R;
                        if (retry == "2") break;
                    }
                    loadChoice.clear();
                } else loaded = true;
            }
            if (loaded) return TM_BUILD;
        } else if (txtChoice == 3) {
            ShowSyntaxTutorial();
        }
    }
}

// ============================================================
// Tester Menu & Debug Mode (hidden: type 2211 at the classic main-menu prompt)
// ============================================================
struct TesterAction {
    char key;
    const char* label;
    void (*action)();
};

static std::string RunBashSyntaxCheck(const std::string& script) {
    const std::string tmp = GenerateTempFilePath();
    if (!WriteScriptToFile(tmp, script)) return "could not write the temp file";
    std::string out;
    ProcOpts o;
    o.out = &out;
    const int rc = RunCommand({ "sh", "-c", "bash -n " + ShQuote(tmp) + " 2>&1" }, o);
    unlink(tmp.c_str());
    return rc == 0 ? "OK" : "FAILED: " + Trim(out);
}

static void TaShowScript(const std::string& script) {
    Out(PreviewText(script, true));
    C_N; Out("\n  bash -n syntax check: " + RunBashSyntaxCheck(script) + "\n"); C_R;
}

static void TaGenAll() {
    C_S; Out("\n  [TEST] Generating script with all features...\n"); C_R;
    std::vector<OptionData> opts(3);
    opts[0].label = "List files"; opts[0].commands = { "ls -la" };
    opts[1].label = "Show uptime"; opts[1].runMode = 1; opts[1].commands = { "uptime" };
    opts[2].label = "Tools"; opts[2].runMode = 2;
    opts[2].subOptions.resize(1); opts[2].subOptions[0].label = "Disk"; opts[2].subOptions[0].commands = { "df -h" };
    TaShowScript(GenerateBash("Test Menu", { "Test description line 1", "Test description line 2" }, opts, "secret", true, "="));
}
static void TaGenRoot() {
    C_S; Out("\n  [TEST] Generating script with root check only...\n"); C_R;
    std::vector<OptionData> opts(2);
    opts[0].label = "Whoami"; opts[0].commands = { "id" };
    opts[1].label = "Exit";
    TaShowScript(GenerateBash("Root Test", {}, opts, "", true, "="));
}
static void TaGenPassword() {
    C_S; Out("\n  [TEST] Generating script with password only...\n"); C_R;
    std::vector<OptionData> opts(1);
    opts[0].label = "Say hi"; opts[0].commands = { "echo hi" };
    TaShowScript(GenerateBash("Password Test", {}, opts, "secret", false, "="));
}
static void TaGenBorder() {
    C_S; Out("\n  [TEST] Generating script with a custom border (ZigZag)...\n"); C_R;
    std::vector<OptionData> opts(1);
    opts[0].label = "Say hi"; opts[0].commands = { "echo hi" };
    TaShowScript(GenerateBash("Border Test", {}, opts, "", false, "Z"));
}
static void TaSavePath() {
    C_S; Out("\n  [TEST] SaveToManualPath...\n"); C_R;
    std::string saved;
    SaveToManualPath("#!/usr/bin/env bash\n# Created with BashMenuGenerator!\necho test\n", saved);
    if (!saved.empty()) { C_N; Out("  Saved to: " + saved + "\n"); C_R; }
}
static void TaParseConfig() {
    C_S; Out("\n  [TEST] ParseScriptConfigLines...\n"); C_R;
    std::vector<std::string> cfg = { "title Parsed Menu", "root y", "option num: 2", "option 1 label Files", "option 1 {", "    ls", "}",
        "option 2 label Tools", "runmode 2", "option 2 suboption 1 label Disk", "option 2 suboption 1 {", "    df -h", "}" };
    MenuModel m; SaveDirective sv;
    ParseScriptConfigLines(cfg, m, sv);
    C_N; Out("  Title: " + m.title + "\n"); C_R;
    Printf("  Root: %s, options: %zu\n", m.requireRoot ? "yes" : "no", m.options.size());
    for (size_t i = 0; i < m.options.size(); ++i) {
        C_P; Printf("  Option %zu: ", i + 1); C_I; Out(m.options[i].label + "\n"); C_R;
    }
}
static void TaRoundTrip() {
    C_S; Out("\n  [TEST] generate -> parse -> generate round trip...\n"); C_R;
    std::vector<OptionData> opts(2);
    opts[0].label = "It's \"quoted\" $x"; opts[0].commands = { "echo 'a b'", "echo \"$HOME\"" };
    opts[1].label = "Isolated"; opts[1].runMode = 1; opts[1].commands = { "echo hi" };
    const std::string a = GenerateBash("Round Trip", { "line" }, opts, "pw", true, "-");
    MenuModel back; std::string why;
    const bool parsed = ParseGeneratedBashScript(SplitLines(a), back, why);
    const std::string b = parsed ? GenerateBash(back.title, back.description, back.options, back.password, back.requireRoot, back.borderChar) : "";
    if (parsed && a == b) { C_S; Out("  Round trip OK: the regenerated script is identical.\n"); C_R; }
    else { C_E; Out("  Round trip FAILED" + (why.empty() ? std::string("") : ": " + why) + "\n"); C_R; }
}
static void TaWizard() {
    C_S; Out("\n  [TEST] Running the wizard (autosave steps)...\n"); C_R;
    MenuModel m;
    RunWizardFlow(m);
    C_N; Out("  Wizard finished. Title: " + m.title + ", options: " + ToStr((long long)m.options.size()) + "\n"); C_R;
}
static void TaCompile() {
    C_S; Out("\n  [TEST] Compile a tiny script to a binary...\n"); C_R;
    const std::string out = JoinPath(GetAppDataDir(), "bmg_tester_binary");
    if (CompileScriptToBinary("#!/usr/bin/env bash\necho compiled-binary-works\n", out)) {
        std::string captured;
        ProcOpts o; o.out = &captured;
        const int rc = RunCommand({ out }, o);
        C_N; Out("  Ran it: exit " + ToStr(rc) + ", output: " + Trim(captured) + "\n"); C_R;
    }
    unlink(out.c_str());
}

static const TesterAction g_testerActions[] = {
    { '1', "Generate script with ALL features", TaGenAll },
    { '2', "Generate script with ROOT check only", TaGenRoot },
    { '3', "Generate script with PASSWORD only", TaGenPassword },
    { '4', "Generate script with CUSTOM BORDER (ZigZag)", TaGenBorder },
    { '5', "Run SaveToManualPath test", TaSavePath },
    { '6', "Run ParseScriptConfig test", TaParseConfig },
    { '7', "Run script round-trip test", TaRoundTrip },
    { '8', "Run Wizard (autosave steps)", TaWizard },
    { '9', "Run compile-to-binary test", TaCompile }
};

static void RunTesterMenu() {
    while (true) {
        ClearScreen();
        C_B; Out(BannerRule() + "\n"); C_E; Out(BannerTitle("TESTER MENU") + "\n"); C_B; Out(BannerRule() + "\n\n"); C_R;
        for (const auto& a : g_testerActions) { C_P; Printf("    %c) ", a.key); C_T; Out(std::string(a.label) + "\n"); }
        C_P; Out("    0) Back\n\n  Select test: "); C_I;
        const std::string c = Trim(ReadLine()); C_R;
        if (c == "0" || c.empty()) return;
        for (const auto& a : g_testerActions) {
            if (c.size() == 1 && c[0] == a.key) { a.action(); PressAnyKey(); }
        }
    }
}

static void RunDebugMenu() {
    while (true) {
        ClearScreen();
        C_E; Out("\n  ** DEBUG MENU **\n\n"); C_R;
        C_P; Out("    1) Settings (bconfig) toggle : "); C_I; Out(g_appConfig.bconfig ? "ENABLED\n" : "disabled\n");
        C_P; Out("    2) Launch Tester\n\n    0) Back\n\n  Select: "); C_I;
        const std::string c = Trim(ReadLine()); C_R;
        if (c == "1") {
            g_appConfig.bconfig = g_appConfig.bconfig ? 0 : 1;
            SaveSettingsFile(g_appConfig);
        } else if (c == "2") RunTesterMenu();
        else return;
    }
}

// ============================================================
// Command line
// ============================================================
static void ShowUsage() {
    Out("  Usage: bmg [-cli | -config \"path\" | -edit \"path\" | -compile \"path\" | -h]\n");
    Out("    -cli               open the built-in editor for a Text Mode config straight away\n");
    Out("    -config \"path\"     build a menu from a Text Mode config file\n");
    Out("    -edit \"path\"       edit a generated script (or any text file in the built-in editor)\n");
    Out("    -compile \"path\"    compile a script into a binary next to it (exit code 0 built, 1 failed, 2 bad usage)\n");
    Out("  (-- works in place of -, for example --config)\n");
}

static bool MatchesSwitch(const std::string& arg, const char* name, std::string* inlineValue = nullptr) {
    const std::string a = ToLower(arg);
    const std::string n = name;
    if (a == "-" + n || a == "--" + n) return true;
    if (inlineValue && (StartsWith(a, "--" + n + "="))) { *inlineValue = arg.substr(n.size() + 3); return true; }
    return false;
}

static void ShowFinishMoveMessage() {
    const std::string v = GetEnv("BMG_FINISH_MOVE");
    if (v.empty()) return;
    unsetenv("BMG_FINISH_MOVE");
    if (v == "removed") { C_S; Out("  BashMenuGenerator moved to its data folder.\n"); C_R; }
    else { C_E; Out("  [Warning] The copy in the data folder is running, but the old program could not be removed: " + v + "\n"); C_R; }
    Sleep(900);
}

int main(int argc, char** argv) {
    RunEmbeddedPayloadIfPresent(argc, argv);     // a packed binary just runs its script and never comes back
    InstallTerminalHandlers();

    bool compileFlag = false;
    int compilePathArg = -1;
    int startupMode = 0;     // 0=normal, 1=CLI config editor, 2=TXT config, 3=generated/raw text editor
    std::string startupPath;
    bool invalidArguments = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        std::string inlineValue;
        if (MatchesSwitch(arg, "help") || arg == "-h" || arg == "-?" || arg == "/?") { ShowUsage(); return 0; }
        if (MatchesSwitch(arg, "compile")) {
            compileFlag = true;
        } else if (MatchesSwitch(arg, "cli")) {
            if (startupMode != 0) invalidArguments = true;
            startupMode = 1;
        } else if (MatchesSwitch(arg, "config", &inlineValue) || MatchesSwitch(arg, "edit", &inlineValue)) {
            const int requestedMode = ToLower(arg).find("config") != std::string::npos ? 2 : 3;
            if (startupMode != 0) { invalidArguments = true; continue; }
            if (!inlineValue.empty()) { startupMode = requestedMode; startupPath = inlineValue; }
            else if (i + 1 >= argc) invalidArguments = true;
            else { startupMode = requestedMode; startupPath = argv[++i]; if (startupPath.empty()) invalidArguments = true; }
        } else if (compilePathArg == -1) {
            compilePathArg = i;
        } else {
            invalidArguments = true;
        }
    }
    if (invalidArguments || (compileFlag && startupMode != 0) || (!compileFlag && compilePathArg != -1)) {
        C_E; ShowUsage(); C_R;
        return 2;
    }

    if (compileFlag) {
        if (compilePathArg == -1) { C_E; Out("  Usage: bmg -compile \"path\"\n"); C_R; return 2; }
        std::string target = argv[compilePathArg];
        if (target.size() > 2 && target.compare(0, 2, "--") == 0 && !PathExists(target) && PathExists(target.substr(2)))
            target = target.substr(2);
        target = ExpandTilde(target);
        std::string script;
        if (!IsRegularFile(target) || !ReadBinaryFile(target, script)) {
            C_E; Out("  [Error] Target script not found: " + target + "\n"); C_R;
            return 2;
        }
        C_H; Out("  BashMenuGenerator binary compiler\n"); C_R;
        return CompileScriptToBinary(script, SuggestBinaryName(AbsolutePath(target))) ? 0 : 1;
    }

    // First launch is decided before settings are read, because reading them creates the folders.
    const bool firstRun = !DirExists(GetAppDataDir(false)) && !DirExists(GetConfigDir(false));

    std::vector<std::string> configWarnings;
    LoadSettingsFile(g_appConfig, configWarnings);
    ApplyConfigToProgram(g_appConfig);
    SetTerminalTitle("BashMenuGenerator");
    ShowFinishMoveMessage();

    // Only for a real interactive launch: never when a switch like -config asks for automation or input is piped.
    if (firstRun && startupMode == 0 && g_stdinTty && g_stdoutTty) {
        ClearScreen();
        if (!g_enableTUI) { C_H; Out("  First time Setup\n\n"); C_R; }
        C_T; Out("  It looks like you're using the program for the first time.\n");
        Out("  Hi! For the best experience, we recommend that you complete this quick setup.\n");
        Out("  It is completely optional and you can find it again at any time in\n");
        Out("  Settings > Set Up Program.\n\n"); C_R;

        int setupChoice = 0;
        if (g_enableTUI) {
            const int selected = RunTUIMenu("First time Setup", { "Yes, continue to setup", "No, continue to main menu" }, 1, -1, false);
            setupChoice = selected < 0 ? 2 : selected + 1;
        }
        while (setupChoice != 1 && setupChoice != 2) {
            C_P; Out("    1) Yes, continue to setup\n    2) No, continue to main menu\n\n  Select option [2]: "); C_I;
            std::string choice = Trim(ReadLine()); C_R;
            if (choice.empty()) choice = "2";
            if (choice == "1") setupChoice = 1;
            else if (choice == "2") setupChoice = 2;
            else { C_E; Out("  Please enter 1 or 2.\n\n"); C_R; }
        }
        if (setupChoice == 1) RunProgramSetupMenu();
    }

    std::string startupGeneratedEditPath;
    if (startupMode == 3) {
        startupPath = ExpandTilde(startupPath);
        std::vector<std::string> editLines;
        if (!ReadConsoleEditorTextFile(startupPath, editLines)) {
            C_E; Out("  [Error] Could not open text file: " + startupPath + "\n"); C_R;
            return 2;
        }
        if (!IsBashMenuGeneratorScript(editLines)) {
            ConsoleTextEditor(editLines, AbsolutePath(startupPath));      // saves itself with Ctrl+S / "Save and exit"
            return 0;
        }
        startupGeneratedEditPath = startupPath;
    }

    const std::string autoSavePath = GetAutoSavePath();
    const std::string autoSaveTimestamp = ReadAutoSaveTimestamp(autoSavePath);
    const std::vector<std::string> autosaveData = ReadAutoSave(autoSavePath);
    bool hasAutosave = !autosaveData.empty() && startupMode == 0;
    const bool configSwitchRun = (startupMode == 2);

    while (true) {
        if (!g_debugMode) ClearScreen();
        C_B; Out("\n" + BannerRule() + "\n"); C_B;
        Out(BannerTitle("BashMenuGenerator") + "\n");
        if (g_debugMode) { C_E; Out("                ** DEBUG MODE **\n"); }
        C_B; Out(BannerRule() + "\n\n"); C_R;

        if (!configWarnings.empty()) {
            C_E; Out("  [Settings Warnings]\n");
            for (const auto& warn : configWarnings) Out("   * " + warn + "\n");
            Out("\n"); C_R;
            configWarnings.clear();
        }

        std::string workflowChoice;
        MenuModel m;
        SaveDirective sv;
        std::string finalSavedPath, tempFilePath, resumeStep;
        bool resumingAutosave = false;
        bool wizardSession = false;

        if (hasAutosave) {
            C_N; Out("  [Autosave found from " + autoSaveTimestamp + "]\n");
            Out("  Would you like to continue generating the bash script? (Y/n): "); C_I;
            const std::string asAns = Trim(ReadLine()); C_R;
            const bool resumeAs = asAns.empty() || IsYes(asAns);
            if (resumeAs) {
                std::string savedStep;
                for (const auto& line : autosaveData) {
                    const std::string wl = Trim(line);
                    if (wl.compare(0, 5, "step ") == 0) savedStep = Trim(wl.substr(5));
                }
                SaveDirective ignored;
                if (ParseScriptConfigLines(autosaveData, m, ignored)) {
                    C_S; Out("  [Autosave restored - resuming at step: " + savedStep + "]\n"); C_R;
                    Sleep(1000);
                    resumingAutosave = true;
                    resumeStep = savedStep;
                } else { C_E; Out("  [Autosave data corrupted, starting fresh]\n"); C_R; }
            }
            hasAutosave = false;
            if (!resumingAutosave) { DeleteAutoSave(autoSavePath); ClearScreen(); continue; }
        }

        if (resumingAutosave) {
            workflowChoice = "1";
        } else if (startupMode == 1 || startupMode == 2) {
            workflowChoice = "2";
        } else if (startupMode == 3 && !startupGeneratedEditPath.empty()) {
            workflowChoice = "3";
        } else if (g_enableTUI) {
            std::vector<std::string> tuiOptions, optionKeys;
            tuiOptions.push_back("Make new Bash Menu"); optionKeys.push_back("1");
            tuiOptions.push_back("Text Mode"); optionKeys.push_back("2");
            tuiOptions.push_back("Edit existing generated .sh script"); optionKeys.push_back("3");
            if (g_appConfig.bconfig == 1) { tuiOptions.push_back("Settings (bconfig)"); optionKeys.push_back("S"); }
            tuiOptions.push_back("Exit Program"); optionKeys.push_back("0");
            const int chosenIndex = RunTUIMenu("BashMenuGenerator", tuiOptions);
            if (chosenIndex == -1 || optionKeys[(size_t)chosenIndex] == "0") break;
            workflowChoice = optionKeys[(size_t)chosenIndex];
        } else {
            C_P; Out("  Select Mode:\n    1 = Make new Bash Menu\n    2 = Text Mode\n    3 = Edit existing generated .sh script\n");
            if (g_appConfig.bconfig == 1) Out("    S = Settings (bconfig)\n");
            Out("    0 = Exit Program\n  Choice [1]: "); C_I;
            workflowChoice = Trim(ReadLine()); C_R;
            if (workflowChoice.empty()) workflowChoice = "1";
            if (workflowChoice == "0") break;
        }

        if (workflowChoice == "S" || workflowChoice == "s") {
            if (g_appConfig.bconfig == 1) {
                RunSettingsMenu();
                // Re-read the on-disk settings so Cancel cannot leave temporary changes alive and
                // external edits of settings.txt are picked up at once.
                configWarnings.clear();
                ReloadSettingsFromDisk(configWarnings);
                continue;
            }
        }

        auto askNextAction = [&]() -> int {      // 0 main menu, 1 launch, 2 exit
            if (g_appConfig.bconfig == 1 && g_appConfig.end > 0) return g_appConfig.end - 1;
            if (g_enableTUI) {
                const int n = RunTUIMenu("What would you like to do next?", { "Main menu", "Launch script", "Exit" }, 2, -1, false);
                return n < 0 ? 2 : n;
            }
            while (true) {
                C_P; Out("\n  " + BannerRule().substr(2) + "\n  What would you like to do next?\n    1) Main menu\n    2) Launch script\n    3) Exit\n  Select [3]: "); C_I;
                std::string c = Trim(ReadLine()); C_R;
                if (c.empty()) c = "3";
                if (c == "1" || c == "2" || c == "3") return atoi(c.c_str()) - 1;
            }
        };

        if (workflowChoice == "3") {
            if (!OpenGeneratedScriptEditor(m, finalSavedPath, startupGeneratedEditPath)) {
                if (!startupGeneratedEditPath.empty()) return 1;
                PressAnyKey();
                continue;
            }
            startupGeneratedEditPath.clear();
            startupMode = 0;
            const int nextChoice = askNextAction();
            if (nextChoice == 2) break;
            if (nextChoice == 1) LaunchScript(finalSavedPath);
            continue;
        }

        if (workflowChoice == "2211") { g_debugMode = !g_debugMode; if (g_debugMode) RunDebugMenu(); continue; }

        if (workflowChoice == "2") {
            const TextModeResult tm = RunTextMode(m, sv, startupMode, startupPath);
            if (tm == TM_FATAL) return 2;
            if (tm == TM_BACK) { ClearScreen(); continue; }
        } else {
            if (!RunWizardFlow(m, resumeStep)) { ClearScreen(); continue; }
            wizardSession = true;
            // The AutoSave is kept on purpose so the "Exit" choices below can resume this session;
            // it is removed once the script has been saved or the user chose not to save it.
        }

        if (workflowChoice == "2") {
            // Text Mode picks the border with the `border` directive (default =)
        } else if (g_appConfig.bconfig == 1 && g_appConfig.menuborder > 0) {
            m.borderChar = g_appConfig.menuborder == 1 ? "=" : (g_appConfig.menuborder == 2 ? "-" : "Z");
        } else if (g_enableTUI) {
            std::vector<std::string> bOpts = {
                "Classic  :  " + std::string(40, '='),
                "Modern   :  " + std::string(40, '-'),
                "Zigzag   :  " + std::string(40, 'Z'),
                "Custom   :  (Enter custom character)",
                "Exit & Save"
            };
            const int bIdx = RunTUIMenu("Select Border Style", bOpts, 0, -1, false);
            if (bIdx == 4 || bIdx == -1) Quit(0);
            if (bIdx == 1) m.borderChar = "-";
            else if (bIdx == 2) m.borderChar = "Z";
            else if (bIdx == 3) {
                C_P; Out("  Enter border character: "); C_I; const std::string cb = ReadLine(); C_R;
                if (!cb.empty()) m.borderChar = FirstChar(cb);
            } else m.borderChar = "=";
        } else {
            C_P; Out("\n  Select border style:\n    1) Classic  :  " + std::string(40, '=') + "\n");
            Out("    2) Modern   :  " + std::string(40, '-') + "\n");
            Out("    3) Zigzag   :  " + std::string(40, 'Z') + "\n");
            Out("    4) Custom   :  (Enter any character you want)\n    X) Exit & Save\n  Select (1-4) [1]: "); C_I;
            const std::string bc = Trim(ReadLine()); C_R;
            if (bc == "X" || bc == "x") Quit(0);
            if (bc == "2") m.borderChar = "-";
            else if (bc == "3") m.borderChar = "Z";
            else if (bc == "4") { C_P; Out("  Enter border character: "); C_I; const std::string cb = ReadLine(); C_R; if (!cb.empty()) m.borderChar = FirstChar(cb); }
        }

        C_H; Out("\n  Generating bash script...\n"); C_R;
        const std::string script = GenerateBash(m.title, m.description, m.options, m.password, m.requireRoot, m.borderChar);
        C_B; Out("\n  ========== PREVIEW ==========\n"); C_R;
        Out(PreviewText(script, g_stdoutTty) + "\n");

        if (workflowChoice == "2" && sv.previewOnly) {
            C_P; Out("\n  Not saved! Exiting preview mode.\n"); C_R;
            if (configSwitchRun) return 0;
        } else if (workflowChoice == "2" && !sv.filename.empty()) {
            bool ok;
            std::string dirArg = sv.path;
            if (!dirArg.empty() && LooksLikeWindowsPath(dirArg)) {
                C_E; Out("  [Note] \"" + dirArg + "\" is a Windows-style path and is ignored. Using the current directory.\n"); C_R;
                dirArg.clear();
            }
            if (dirArg.empty()) dirArg = CurrentDir();       // "save --name" means: the folder this tool runs in, no questions asked
            if (IsBinarySaveName(sv.filename)) ok = SaveConfigScriptAsBinary(script, sv.filename, dirArg);
            else ok = SaveToManualPath(script, finalSavedPath, sv.filename, dirArg);
            if (configSwitchRun) return ok ? 0 : 1;
        } else {
            bool saveAnsBool = false;
            if (g_enableTUI) {
                const int saveChoice = RunTUIMenu("Save to file?", { "Yes, save the generated script", "No, continue without saving", "Exit" }, 0, -1, false);
                if (saveChoice == -1 || saveChoice == 2) Quit(0);
                saveAnsBool = (saveChoice == 0);
            } else {
                C_P; Out("  Save to file? (Y/n) [X to exit]: "); C_I;
                const std::string saveAns = Trim(ReadLine()); C_R;
                if (saveAns == "X" || saveAns == "x") Quit(0);
                saveAnsBool = saveAns.empty() || IsYes(saveAns);
            }

            if (saveAnsBool) {
                std::string methodChoice;
                if (g_appConfig.bconfig == 1 && g_appConfig.saveload > 0) {
                    methodChoice = (g_appConfig.saveload == 1) ? "2" : "1";
                } else if (g_enableTUI) {
                    const int saveMethod = RunTUIMenu("Choose Save Method", { "Type path manually", "Browse (file browser)", "Exit" }, 1, -1, false);
                    if (saveMethod == -1 || saveMethod == 2) Quit(0);
                    methodChoice = (saveMethod == 0) ? "1" : "2";
                } else {
                    while (true) {
                        C_P; Out("  Choose Save Method:\n    1 = Type path manually\n    2 = Browse (file browser)\n    X = Exit & Save\n  Select [2]: "); C_I;
                        methodChoice = Trim(ReadLine()); C_R;
                        if (methodChoice == "X" || methodChoice == "x") Quit(0);
                        if (methodChoice.empty()) methodChoice = "2";
                        if (methodChoice == "1" || methodChoice == "2") break;
                        C_E; Out("  Invalid option choice.\n"); C_R;
                    }
                }

                if (methodChoice == "1") {
                    SaveToManualPath(script, finalSavedPath);
                } else {
                    std::string picked = ChooseFile(DK_SAVE_FILE, "Save Bash Menu Script", "sh", "menu.sh");
                    if (picked.empty()) {
                        C_P; Out("\n  Dialog canceled. Defaulting to manual path entry...\n"); C_R;
                        SaveToManualPath(script, finalSavedPath);
                    } else {
                        if (BaseName(picked).find('.') == std::string::npos) picked += ".sh";
                        if (!WriteScriptToFile(picked, script)) {
                            C_E; Out("\n  [ERROR] The file browser save failed.\n"); C_R;
                            SaveToManualPath(script, finalSavedPath);
                        } else {
                            C_S; Out("\n  [SUCCESS] Saved to: " + picked + "\n"); C_R;
                            finalSavedPath = picked;
                        }
                    }
                }
            }

            // Saved (every Exit choice above leaves through Quit) or deliberately not saved:
            // the wizard's AutoSave has done its job.
            if (wizardSession) DeleteAutoSave(autoSavePath);

            if (!sv.previewOnly) {
                bool compileBin = false;
                if (g_enableTUI) {
                    const int cc = RunTUIMenu("Compile this script into a binary?", { "Yes, compile to binary", "No, continue" }, 1, -1, false);
                    compileBin = (cc == 0);
                } else {
                    compileBin = AskYesNo("\n  Compile this script into a binary?", false);
                }
                if (compileBin) CompileScriptToBinaryInteractive(script, finalSavedPath.empty() ? JoinPath(CurrentDir(), "BashMenu.sh") : finalSavedPath);
            }

            const int endChoice = askNextAction();
            if (endChoice == 2) { DeleteTempFile(tempFilePath); break; }
            if (endChoice == 1) {
                if (finalSavedPath.empty()) {
                    tempFilePath = GenerateTempFilePath();
                    if (!WriteScriptToFile(tempFilePath, script)) { C_E; Out("  [Error] Could not create temp file!\n"); C_R; continue; }
                    finalSavedPath = tempFilePath;
                }
                LaunchScript(finalSavedPath);
                DeleteTempFile(tempFilePath);
                tempFilePath.clear();
            }
        }
    }
    return 0;
}

// ============================================================
// Embedded program icon
// ============================================================
// (the data below is produced by embed_icon.py; see the comment inside the block)
// ===== BEGIN EMBEDDED ICON (generated by embed_icon.py - edit the artwork, not this block) =====
// The artwork centred on a transparent square, scaled with Lanczos to the standard desktop icon sizes and
// stored as PNG in base64. Program Setup installs these into the user's icon theme so the launchers look
// like any other desktop application.
static std::string Base64Decode(const char* text) {
    std::string out;
    uint32_t acc = 0;
    int bits = 0;
    for (const char* p = text; *p; ++p) {
        const char c = *p;
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62;
        else if (c == '/') v = 63;
        else continue;                               // padding and whitespace
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) { bits -= 8; out.push_back((char)((acc >> bits) & 0xFF)); }
    }
    return out;
}

static std::vector<std::pair<int, std::string>> DecodeEmbeddedIcons() {
    struct Entry { int size; const char* base64; };
    static const Entry kIcons[] = {
        { 16,
          "iVBORw0KGgoAAAANSUhEUgAAABAAAAAQCAYAAAAf8/9hAAADNUlEQVR42lXSTUxcZRTG8f95770zzDB8zQBDJTQtKKWYUFva"
          "tMa4IChpUk26kbh2hxs3aneSJl246UbDRhc1dUeaGMNCMTFNJGIzMVVagaRiKQxIIeVjgLnMnXvve1zQkrh+zvNLTvKI6qgR"
          "uc733926daKjbUj9A2sVV0QNICACAGpFJIrESGFmbvXjT25c8X1/3RW5bqE38VJrbqgwN9c2s7NHMuEQhhYbW+JYUQwiEFvl"
          "7d5O9kp7+ao1p4Cn7qH+Kk4YVn5a29SC32WbK66pz9eQrquhocHDxBVqPJfFnRKtJmPrgsBEYAGeA2BjNc25jOTvxRI/LEvn"
          "YIqOnkY66mPirSVSnsdxv0LSqxcFeVE9AhCLjRLYxikeBxMs/VxDNBES7oc4rkscRbT1nefDi59hjQEX8z9AQcUYgoMDdjZ3"
          "cJMeoR9CcJgnk0ma2MJb+IG54jOoWB84VABUEQzYANQXopIiVcEYwfMcxj6/yI2RWtJrv6jxAt6/2n8JMEeAqKqqkNAYFyVh"
          "IxxVrIW+02nyzTVsVgfIHevVgXMrhFFwBbBHLyRTiTiqhpH7+pv6xqkeImtNJtikO1GiLrGCX2lhvfQvYew7J7It9mz3wwvB"
          "W8cHnwNNurS+nR3uaXcHvTrEdBMGFTLbkxQXMzQ0vUNxtUC+vYFSOUu+7YDp6bve8prruodr+yr68usnH1VK280ptbX1TZkL"
          "3af73j13pkWzjSrGibk/5zOUXaL35Em7+Pe0M7/U9MeD2ceToqoCICJ6Z/yLT6PYvpdK5Y7t7pbbf/1tRluaM7JSXCbWGl7p"
          "qGKrRV3ZSMdrT4uX+3+cvyvj4+PO7NisDIz2T3R1Nl8u7x9Q9kOMcdjf36NUKpNOp6hNJ/E8w9T0A/39/jzf3r7dDSy4w8PD"
          "MWDOV765NFkoRHm3HeOoI4KEoWVldYswitQ4jrTmGuL19WeO2ir19Ul2dwPcmzdHs2dfOzMWVMLGrOkmRohisAqZtEfXyzli"
          "i9g45NHCE2epuLpfXF65trsb/DM6OmpkZOSDqy25pjtWKYPKi1mAYAyqqhhjbBRZ+Wvu0Z9T04VrWxsb9w5v0P8Agk2HgAVC"
          "7D8AAAAASUVORK5CYII=" },
        { 24,
          "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"
          "uJzO/wNuluHMQMMdQAAAAABJRU5ErkJggg==" },
        { 32,
          "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" },
        { 48,
          "iVBORw0KGgoAAAANSUhEUgAAADAAAAAwCAMAAABg3Am1AAADAFBMVEUAAAAAAQDOya/n5NdbWlR2dnGqp5G8uJiXlomKh3cz"
          "Ozj////CuppJRURsa2bZ2NV6em/Y0bgWGROKioQDlJZmXE4Op6l2triUlZMQEBEIXWGrljIHBwehnoimp6fYxU8FBQZnZViW"
          "hi6b0dC5uK0BioxsiYu5ojaVm6coJydYYVaVpK0ODhCak2/PsznQuEsmJRZsZS51wL2SiFDFqznPw2/Rx4/Jys5NSBRYiop4"
          "c054xcGspHIKJAoJYmZGQzh5cjl/f39oda+FdyuTlJGOqc/CvaLt7OESORJCPRJFSElVVVVyZhN5eG+De1WTysuhjSvOvW3j"
          "yk/49+8WFhUaGRkZGRgeIiEKV1ckKZEzMzM1PJk0Q0NAOJRbUxhYVDBUZK1mXBR3gXdthsKCch0MDDIAAIwYGBUSGJsXSxgj"
          "IyUuQDwui3JDPS5FRUpJTqZQVqpjWipgYVh7hrd4l8p9vMGGe3GKiXqIiIaNl8WdvdqomUyookv//wAMDBAICAgfHx8TExMe"
          "HjMCTVIaVBsRmqAYtLUnH4kpJxohLDckJCQtLSs/PwA0MBw7Nxg1NTU4ODs5OVE/P0c2dy03i41OTlhHS5JTU0pfX1pRW2ZO"
          "ajlYYmddc7lclptEqKZ/AABsWZ9lZWVsbGZubmdoYJ1lZ65/fz98fHdxdG5xcWp+fnN9f4N9lJd6qK1/pdJ60zqYhh6FhXmB"
          "gXWDg3uGhoObkSidlUKZmZWimn6+s0eyvMu0xN2h2NPJrUnEq2jf0o/c3OH/AAD/f//x21j063QAAAAAAAAAAAAAAAAAAAAA"
          "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
          "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
          "AAAAAAAAAAAAAAAAAAAAAACeGPfVAAABAHRSTlMA//7+/v3+/P7+/QH+/v7+sv79/v////+80//+sf7//2/+/v/9///+//7+"
          "/9H////8/v/+/////v7////+/////wL//sL//v7//ioD/sD///////5IkKz///8F/////vr//v7//ij/bf//XP//8U////9x"
          "/////8/U/////wFPngg1Gf//////wxc43QS5/zBRFiD//xr/N8UZ//////8C/zV4vf//BDFRdZ3///////8Vnq10//9G////"
          "////////AQL//wAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAwMRW"
          "LQAABC5JREFUeNqFlgd72jgYgA2RhGSwCXauucTFrh0TIJRVZigJ2atJmtGMXtvsXq+97j1v77333nvvvX7YyUkTbCD0fSz5"
          "wc/36pPszzIMY+BkPvymtoSR2pER089PvmaKdDJD2H4DHCN3mYVv3TXVcWNwZ1FwMj01DleV4ftcXqhuNQs3UwHetCnYXkmY"
          "ube9vZGy1m8w29g+G/J4YUuZsH/LJjywpamicHvdG/UlnDtHu92tviZ7JYGt2/nWgwdOnHy/68ylS729v7ozv2QyGfdfbtHX"
          "5MEQlAguKtQfuPW2vftOLLy2vHz2i8u6jEUYkHn8WQWhZzXDmnByoWv5bG/vZVEMiBCHeNzsM9YAKkxpx3t7972w0NXVdSYQ"
          "6g0141AoEArJ+ppQcQ28IAgx4TozM0IsSM9cFYGzDdhsgs1KkAp2XHlKXNt9gtA2YDssWIXfNhWEw0KbbYAqZgH7PrejygKw"
          "tRkh1kkFX225moMVhZ3AVoqgAltc+S8XFMFWZ1lp7CgTBn8Ijw4r/txoAaiVp2Qlpvg1SVGkU4Rw7LG7mTdLhPqyDGMTykQk"
          "GdVGYSLvZZm/b5TBFp9IdudGr0TGNCmB1SPMY9biK1t07OdohxgI4IRfia5khZcrPGkLi1cVDcoojQJhvzQ3P//ItdI1nOfW"
          "KXD7uWPf/xvRMFIkKSxfGYsn4lMlr+j9zzVTWluN9voSga1ZLdodQH62RcLYr8Ul7fGnLRl8vrpVfHVHj9a9EumYbQESzSCB"
          "Ri0sR7Vh/1fZg2YB725qWjvq879L0fHUH2mQjRAsJXISkVNJ//Di4EHzXbJ7KMa25UHnx6MEynbXT2pHIhCW/AlRVpLJ4exD"
          "95he0T7HdXScH1dIkOOhnM4DyRESCQ4gpXswNu3c2FtPc0VY3i7TDCznTbsAqykpXZZRKjmXjQ2ZNvBtGzzKwcynYSXtSaeQ"
          "1xEUuhUllZqISHPxjmknU86PkKiQd+RF/c88EEUBgLg/SUsjonXMHzIH3sIw79TWvvsSdmBQYGWe6LrKQihAyEr+U0/Yuldi"
          "T5njnczDHyN7H0bGndJVEbJQ5tgCCyCvLq4MFgjgjhuDFuM/goh+ZojuNqCxPAAsRmHCe/vCOYgQYQ91muNPYxe9oy5E1sFY"
          "VY2z1+VI23XCQ3ZbMf4f5rtMtc8VgrxVcDIXJ+kD9pi/Up4iOuRhidDZ0+9wIOQogvA6yIV4WCowPZOuL58lpgTGLFZRib1M"
          "+IB55mIGbY7XmBFk96w/513M0AVMsIiNThQJrTVMG8EQYkIviMb4PLuH2Yg/fqHPC2kcWW3GSUQII7yEvIiICPMtkAseKca/"
          "qE/2u0von3T3u2vo4dYxgYCb2hifuTZEqv3F8KAlCIQpU1Ewb9MiqIKqCtPP77IU9B0N27c30EY7K6uXGxqetET/Dw1e06C4"
          "XZDGAAAAAElFTkSuQmCC" },
        { 64,
          "iVBORw0KGgoAAAANSUhEUgAAAEAAAABACAMAAACdt4HsAAADAFBMVEUAAAABAQHNya66tZh2dnCpppGWlIooKSjBu5v19PCK"
          "ioRWV1KIhngUFhPl4tMQp6ilpajp6uVra2cThYfDvqPh3su0tKsTFBNmW01wcWzSuUkypaVtbWgJR0xlY1lsbWmqljCpqakn"
          "JRbYxU5Rsq7Eqjg1NjBwaC24x9DNszyZmZlPSBSFeTGhnYnNxI1JRi8KlZdVVVXIysyXhy2Rh064ojjS1tF/f3+Vm62qqlUY"
          "eHpKrKp9xMeYprQTExMsmpZHSEd6dFFtcGrHuXIGBgYQKBB9gHiZlHert8f///8MCw1xZhSGpc0VSBZpeK+EdRoHnaFXaLNx"
          "hIeEe0vUyGvjzFAICJQROhIWsrJBPjJaVTFGVapmXBFyiMMAAHUVGJUkJSckKZc6Ng42NjYyOJVDPRFoaWV6cjp0dW90hbV6"
          "l8eQsdSpmUzEqkvQ2+EFBQkcVh0QVFUQZ24oKCs3NTBHR49WV1VkaGh3dnC2p0q8sk27tHy2trbV0bjl0lTk13QEBA0ICAwc"
          "HBwaGiUef4EgHhInJR8qKig8RKIqn6FISEhVV1dKZUhxc26RjrKZkTOijTWtpHSqqo3Gxsb362oJCRMWFh4REREfGiEbGyEd"
          "HSAdHiEELjMuLi4nKSs/PwA6OjowMDQvMDU0PqA5RUUhSEQ8QZ0oUEYgZCAhZ1o4jYw3ti0/sLFVAABAPitKQCNKSkhFTaNV"
          "VQBdUQVcXGFfZTtbbGxda4ZbYJpccXJdcrtciVtoXyhxVVVra11hYVVgYF5hYl5tbWdrbmtsbWlra4Zzc1x0dG1pf4Fof8Fs"
          "kZFvwFqRgh+JiYmdm4eAnsqfo22NtuGBxzedxluCztKqVVWkmmeiq8OwsJy90NnX0Hvf37/iz3jm4Lj//38AAAAAAAAAAAAA"
          "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
          "AAAAAAAAAAAAAAAAAAAAAADq57gVAAABAHRSTlMA/P7+/v7+/v7+/v7+/v7///7+//78/sH/qf//qv/+jv8F/v/////+//8K"
          "/v77//v/A////v/+Av8D/////7v/+/+X/977/f7/AYj9//////////7///////r+//3///9p//4R//9x/nP///////81////"
          "T8v/ZQrO////Cv3//0y4TTD//8vT//9GMP81/////wkJ/2t/gHuAtP//FoAESnqx/xb//////////wPD0GP/A/8v//8T////"
          "//8JEz+BvSpR2RMLRv//////Dd7///////8D//8N//8I//8CAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAlNQD"
          "LAAABohJREFUeNqVl4dj00YUxi3pdDYniQjbCBCpo1iRFONJbCeB1IkdE0waQgJJCgmhpVBW2XtDC4XuQlu6995777333v1b"
          "+mQ7JLZlJ/xsn2xL77tPd0+nJ5stz2zbDetrgPaaLO3t7dXt7TXtM0aozu/I7f3YVsraIDthmOD6kviP+MOTp04UGju3/lcQ"
          "Pts2g548YRox4qhZBQK1thlTaIZhHIzDfAPmD7N1jMDkP9CwiOfm3WwhYEaNj5tmQICyEqA9wvjoNM2i+cWnMCcnMP0e13jM"
          "fWoyOJhvPQb09O3LIlUVWVY1c4qjgsB5yyJtM0uB/65pa7sGvrRFnqksUFVX35xI/L4gR32eP+sbtzTWm82brhZTwGIaq0cE"
          "mi+68OJLLn3sxeWLF23YcNn3CzfWNzSIC6Cp37KlsQG5ZloKzBl1kBO4evnixYtWrLhsIQhIEkk4nWgBRkhcEhkRqC3OxLxA"
          "y40lAkERJ4iI6kWOiF+UcTAqUAcCoPDw8uWL3/rSFPgRel+SQMiZIAQlkKtuiruigzrx0t27d7+xCNiwobk5sXAjNAlJkkR4"
          "SfWvRerys1BbNAZnB1Gye+0m3jwn7HkEeCMUabGcxrEOqAo4l5jTyFoJVI8KCE9so6iObqpjqYVApGWcaWwRqe5uQeigtglU"
          "99JyDiokEggs7RaoDkrYRnV3WDiYCQciVCGVYQygZwhd2tFRegpoB4cwWzETKw5iMmyc6R/Sx3Ngjfc3RHmj6pl0ymuuSGUc"
          "LCsnIARuCuKhLjUeSPUNJhE3r/PcTkFQVCNlRFVVM/oCfNjOUavL5EGLtcCqI6p8QB4YkLXhdZjgcDi2a2vnuQhQhipD95pP"
          "jkdTvOfAQdZ5V5lroYzAGlX2RQNGYJMm9xinAhmSCR+3NVmtSGXG4NSAHPUQjHFM8fmOKEbMf3KvlYMys+Ad2n+kxyOKzaIo"
          "goImd63ytp7DKaxRDqhaFxHZ0F8sFvm4r8c/1Bc+/uxL1utBKX51QFZSItuv+rQYFmNalxHrC19u6aANF0MINhQ5KyCfthta"
          "TMRK3EitCzx/63tnBX7NCXi2V7nmuq6cm8XcuCJPBmKPu7ZTXbLiEdnNAX3HOgWLm7SAEZXj/vtK86DX1RuJ9PZGeqtg0/Z2"
          "W9v1ysC6R6/nPQEtILL7+7gdryq8mJZ7uhSly39b8TTyX/1y/igtP8hKdLMsy5vTId0/oBAcjce4KAiE5Hi85+Vh/x1FAg56"
          "6pg6ZBJLHLKcDgbpwyGH54ShxMSMEo/7AkQMwURGh7tW3VkgcDhXl5wlyKf2q5s4pxOLkiN0Ez8E45+JbupDYnPIpyg9fu/K"
          "1WPXg0a6sA5hSfqo3I8EjkNOQjMZNCin2Wa4kFhRVH3DXi/14JhpbLKt54oRG6WQmuZ14W7ChBg9GdbUowzkYjMbUhX/8KqT"
          "RXXeFdQ8wGyyuS8IrERIWv4n5HCEQozjICf4T/vU/hDrOCpr3/j9RnhrQXznodpDt/x9S+2h2j9qZ9XewPGIpQmS+xk6lJYz"
          "iDAcZQ/HfSCh+nxa9J1ULHyFrSzf7oPbuY448i+PcEMozXGY6AJy6gEtDtEBzRcwjOd2NhWFNe1tsn1QXVNTXb2PYYMoqduR"
          "yCG4pfJhD8aDAo+S4c1xI+yl/L4e767XbWUKZgbcQ4GZyfAQ7UFYIhzcWJNCkkx36ormN7z64JrBhz61rS7q3/bunrWYhioQ"
          "u/MVLUkSThDAhp14wtN5pzPo8fvXeD005pIvlMY/sp5gt9tBY15qyAIVka4jOFjnGqBEh3g3zfI8C4Umsl/QWez+3nZMZ0tl"
          "OohG4E0QpFIW1iydaZoBKyBQFN752QyJnggMOEGlAk1wLUyaCI3YCV5KBcz1YCrjcFeGhjEw48s4aGTgEEchbrisoMCScq0k"
          "lReYDQIHf3paZwslGOIZA2R3sLyDSZOZn79OMYVD5mAz/CiYGXHAFwl02q77RJLMLCx8Oit9kDPHEKaBu+pYQR50vrIPw6TP"
          "x4jABirS7Au6RDyZD5kAm9wffNY/4jqOFfb/PmQ/j1me4VmexfCFRdkvEs8y8BOBeXMPWHKa4ZzwwNj4vba1hKbh/mEBTfMM"
          "QwjU625CQAE5YYETqGtnF5RHe6QJ5dAkGs7I7P7a6+4vGP8939ETA54XCDzX7SzOwQ8JnhgEFgb7yp0li8jnu+zjomcb/arb"
          "V1otQ5dPa21tnZZrypDd09pqEf4/YPJvKswOYq4AAAAASUVORK5CYII=" },
        { 128,
          "iVBORw0KGgoAAAANSUhEUgAAAIAAAACACAMAAAD04JH5AAADAFBMVEXm1lzg491dXVsiIiIoUilbaWBUmZigtiuSmY/e3tkO"
          "Dg4RH00jqh8X0BFlnV1qnZ2PjIKbmpOjw2cUFBQnJRs+PkENDqEAf38bYV0A/wB/fwBHRj1LS01qf8BpvS9/oM1Lxwl1zF2D"
          "e3uAfnKAf6uCgHOKiHyakVG2tsKBncz/AP/49IgAAAABAQF2d3HBvJ3JxKr7+/jCvaG7t5qYlopra2cVFxQXpKapppOIhnnO"
          "tkilpaQSmJk3NzRkY1mMi4TKy8vq6eLEqjoql5lUVVTR1tRHSERXV1d0dW8VnaFYWFT///63tq4oKCagnY0YGRgdd3nS5+l8"
          "fXlGRDV4eHStmTK6ozdxZhBwaC/y521kWg7Nsz0rKBKGejCUhi53tLKGps/Nw43XxU6LhE6Yk3hVZ7DLu2qEfE6Rl66uucXh"
          "3tA5NRBQSA7o5tZ+gHlTq6h5dU+HeBlyiMLErURqeLUxnaGBfW4ZV1d6mMeZpa8IBo+3w8xZUxB7cjIlR0cTF5QvNpm4pkzV"
          "0bYBAXUkKZVHVqhyc25CPBJqa2Zsa2eNstgupaiVhByolkjl2mxXVTBqambN3OTo1lQbZmcqhCxBSqRjWypqaWcMJwwbiowS"
          "oJw/Pz8oUVAndyhqaWp5eZZyrazRxHMWRRdpamYgHQ5KSothXlN8chhxiLmFhXmvpm6tyeG70+bc0oshISEkZiUqhXldY2VV"
          "qlWckzGZmZf9824RNxIaXWAlJiYrKywpKio4ODY3ODc1PaA8R6RYV1ddcLaKia2hjS3Jxs/k2qkTJyYdI5USSUo0SDEniowj"
          "tLdeYFt5eHJ+h355hYyqqKju5YUbVhocsbQ9PTs6Q5ojWiQjZWREREJnZmR/fz91dnKfoJ6csL6jmXS2t0m6urq8wZfc1nrh"
          "zkzj15Dq4bgAAAAWGBodeSEoKCQyMjQ1NS8waEs/fz8peXsylDMzoDVBPjFKSkdaW1lVVptZeEtiYl1jZF98k6eJh3+EhISa"
          "mI+WloiqqlWjo5amrcC0t28Ol9Y0AAABAHRSTlP//54T9Br//xEPlP////8Il+H/Y8Zi/wL/AQLTcP//////HW3/dbL/Ff8B"
          "/wD+/v/+/v7+/vv8//7+//7//v7+/v7//wP+/ndU//4F/vz+0P//Dv0u///+/v/+//7+/////v7//v///v////7+/////v//"
          "/////v///////v7//////////2j+UHD////+//7Y///////+EP///wT//y//////s///////F//////Y////A/4W////a5Cy"
          "dI3//zH///8S///////////SGv8H////V////1aKBLD/////EP//////glP/TqLb/wT/////zkf//zeB/0kqmLQDE///RYLR"
          "OQAAE0FJREFUeNq9mwlgG+WVgMMNvdu976PdWzOSpZnJzOgYWbYUPJkRRIoO6yKWhBVHsldaB1NjO2C3xA6J7cQJuSCBkDSB"
          "lEDYEI4Cy3KV+6ZQylGudntv2+3dbbvv/TOSJR+6nOyLI80h6X3z3vvf//7R0zLDPPG1Gwzn33LLLT4izz7r+1vf6kq5sGyz"
          "UsrPwZvJg+9Z+Ofz4Sd+3Oebq22ZYQHZ/7WtIOw8oUsPpX2aLj9Ps4sLfCJ94Xxd8wBWGvZP/IpnTo984qerawI8//pBmjct"
          "WcwLHhXYr9Xhgq+yXqPFYjbDA4pRf9alcq9uMYMYzQJdC+BPDKs3fdLr7u1tPXXSq31ar8ki0PSPvvF7K6sBtBs20ry5t+U0"
          "SKvC0zRtpy6qAXAh7WxZxINLE0UQ6gD4dwBwt5jA8adKwPXaM14/LdQC+N8yADOR4seYKz92rpp5aktv0DdNjQMY0REahbn4"
          "rO+QYTZ7Cv/KXlE6ayL6TWUAfQ25AEaPYjkVggQNAiCzReVOjchoAw2g7iDErMFz1KkROzqkUQBwnoWjXt1+8cV79uy5uDn5"
          "Pnncvv3cFncRoK+ePKADmADgSZtt+ZLF9q8tvZZGAJwlC3BtNhtB6FjegVv40IGf2dFB9m0dKLhPTmh7HXgY9dp0egBYZ2k0"
          "BowawOble4ufAypRLflEosum6V6uA5XtaZAlgoYA2ucAvLRcv1LbAqKpJSc1k3QQTu2UBoAItnMbBHDOArTB29vK5bLy7aK0"
          "aX94ZPNm3MbNzSDw/KRt+d4GLcBWAuy9ZNW2VXNlm3bIfcYZ7uoyuGGPzdYkgKkI0PrQDetBPvPQQ2vXwp8mVpDf/ranXIaG"
          "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" },
        { 256,
          "iVBORw0KGgoAAAANSUhEUgAAAQAAAAEACAMAAABrrFhUAAADAFBMVEUqUyRaWldNmlHl3a0XFxYtLSxYWFcYGBctkW0hJWlV"
          "VVPy8vNbY5yUuCeZmJLc0VobfoFWamFbxC1QXy6VlZPe5eUfHx8uWUybqkavrsOvwL0iywZ5mpqZl4t+oM2AbRKtwWtUvDqC"
          "gX2am5g/QEAfkh5APz1AQD99wb6Eg36HhnsdI6IpvcFAP4V/gH1/gHVOwE+Ef4C9vcC8wL7Av8IAAAABAQHBvJ12dnDCvaH7"
          "/PlqamWFhHmlpaSqp5cRl5iXlo65tZvOtknHw6wZpaba2Mq4tqnEqzsTFRBmZVkpKCVXV1NKSkclqKpwZQ8PioxlWw00NjTm"
          "6eqLi4YVnaG7pDnp5tjHx8iTk5Nxcm5waC4ulpbMszzKumt+gHm22NmPhk706WxZWVmrmDNxcmyGps8oJRSrxctbUw+hnpEY"
          "V1mXknotiYpzdHAKJiglsrQ2NjaDe03FrUR6dE+AfnRUZrCRmahOSBCWhywFBI3Wxk9JRi9ld7jV5O24pkyGdhXNxI56lseS"
          "tNauuMdyiMLV1NYwOJlzdG5Lo6R1dnJ7czRFmpvg3c7K2ueEeTP78nIEBHMTFpV1dnNLWquao6trbGgQNTM5NRNZVSvm11Yl"
          "KpcVoZ2vpm7v6IgLJgvRxnFjWyxra2Y9R6NpamWomUshHQxCS6ZgXliHh4dqamaoqKfm2WkMNg1YWFhra2d0hreFi7GJx8Ur"
          "naFJnqEdI5ajm20HHiEaZWckZWZFRUQTTE4Xd3g0lzS2trUthy58cRPc1I00MzQrdi1dcbTl2qwXRxa81eYdsrEweXBCPSxp"
          "amaeoJtISEdrbGhseox7uriHh4ehjSrx5FpCPBJ0hpObklIpaCo2pzlYWpddYFyyx4smJiZkZppqfsCInMiRvbyvy+TPzM7k"
          "2Y4VFXUnop0vszZdYmGVhBqbkjI4PEEwRkg7RZtESJJcXFy0tElGoZ10dXB6krD589kEBAQXGaQ0PKA0dVZhYl2JwL67s3Ww"
          "4OHb03AaGhoZUxkaXmAQyQpTOTRGAAABAHRSTlP/Hf//HaJdYP//jgL//+b///L/+SX/2////////5H/////GGSs/8qn/1Or"
          "////H1X//wcI/wD+//7///7+//7//v7//v///v/+/v3+/v/////9//////7/Ggj//////////8b/qP/+///+////LP//sv//"
          "///////////+////////////BP/X/23//////////1D//w3//////////////9H/jv////4oKA3//7mw//////////7//7H/"
          "//8K////0f/////////9dv/NUP//DP/////////////V//////8F//////////////90//+O///V////B//////V////xBTj"
          "cQAAN91JREFUeNrlnQl8W9WV8MMWKLQUOu3Xfp12ts438818+xJJL3qSnixbeZJsWYosm8ixKyNQrNgh44VYjYgcppSAE5xC"
          "MXaIEzskIYRkkhaahjUsDZAQoAQaloG2A2XK1lKWlpbSduacc+997z4tWWzJhN+cOFreIun+77nnnnvuue/NmvMfXGYd/6Ej"
          "C0iWHLdcZ8rVTPCZvZc2Hluuk+U4vph+5YIFFQaw5O23L7roolVlxTFjYn6f+OqSP+jqJysKYORtx8dMLqogAFCnBav+AwPY"
          "vnnzo1/2e0ncbjf+F8K2FIm37JsqSOnf482gyVlQEQCPO51OTUHxKZLYlY9A7Ox77Uf9djuI4kY1uLpiALrwU6XPp+9gjwYK"
          "2mJsxNcKf8t28G28CLIoxWDt7PzyokiPpT4kgwDerhgA1fIdlq8WtcJ3mFjYa6XUb7RbGchlt9uPUXILgCKKPh/qKbzLOCum"
          "Ads3OQMqWQBLNfm9ZTQQjrOXaiteP/6HB8VvFho/1m4qBlcaOko+zYs2yO+1w/nw58dH88PhA6Qf52MNlQD83fQBjGwf+TtV"
          "Vb11HxvB8ns1TYXyO888fc6c06cFYMFm5yas/bq5HxupAwBu1hU6be+9994Z50wLwEUO58cPgI8DCNhA/uHWaQJwqNDkPk4A"
          "4OdaAPzf6WqAe35zc53vYyNuMFmqaAII4BvT1QAXYvWW7MysvRDr7+Xu/lhdmcWfKuzLi7tcyecodRgd4dNMd7hKACzugNQf"
          "m1246P+LGBUTKHJhBLcSp7PPtCt2g3RRXVQPQMl6K+GaSFWmHEMDFIvuGP6U5EyyoioGLEPBiqmZP25GAEg/8Gh1fBwO3Yn4"
          "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" },
    };
    std::vector<std::pair<int, std::string>> icons;
    for (const auto& e : kIcons) icons.push_back({ e.size, Base64Decode(e.base64) });
    return icons;
}
// ===== END EMBEDDED ICON =====
