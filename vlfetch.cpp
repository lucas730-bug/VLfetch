/*
 * VL fetch — a fastfetch-style system information tool for Linux.
 *
 * Single file, no dependencies beyond libc/libstdc++.
 * Everything is collected from /proc, /sys and small standard utilities,
 * so it works on any distribution (Arch, Debian, Fedora, Void, Alpine, ...).
 *
 * Build:  g++ -O2 -std=c++17 -o vlfetch vlfetch.cpp
 * Usage:  ./vlfetch [--no-logo] [--no-color] [--logo-only]
 */

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <dirent.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Colour helpers                                                      */
/* ------------------------------------------------------------------ */

namespace col {

bool enabled = true;

std::string fg(int r, int g, int b) {
    if (!enabled) return "";
    char buf[40];
    std::snprintf(buf, sizeof buf, "\x1b[38;2;%d;%d;%dm", r, g, b);
    return buf;
}

std::string bg(int r, int g, int b) {
    if (!enabled) return "";
    char buf[40];
    std::snprintf(buf, sizeof buf, "\x1b[48;2;%d;%d;%dm", r, g, b);
    return buf;
}

std::string bold() { return enabled ? "\x1b[1m" : ""; }
std::string dim()  { return enabled ? "\x1b[2m" : ""; }
std::string reset() { return enabled ? "\x1b[0m" : ""; }

} // namespace col

struct RGB { int r, g, b; };

static RGB lerp(RGB a, RGB b, double t) {
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    return {
        (int)std::lround(a.r + (b.r - a.r) * t),
        (int)std::lround(a.g + (b.g - a.g) * t),
        (int)std::lround(a.b + (b.b - a.b) * t)
    };
}

struct Palette { RGB a, b; };

/* ------------------------------------------------------------------ */
/* Small string / file / process helpers                               */
/* ------------------------------------------------------------------ */

static const size_t NOPOS = std::string::npos;

static std::string trim(const std::string& in) {
    size_t a = in.find_first_not_of(" \t\r\n");
    if (a == NOPOS) return "";
    size_t b = in.find_last_not_of(" \t\r\n");
    return in.substr(a, b - a + 1);
}

static std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

static bool startsWith(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

static bool endsWith(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}

static std::string firstLine(const std::string& s) {
    size_t p = s.find('\n');
    return trim(p == NOPOS ? s : s.substr(0, p));
}

static bool exists(const std::string& path) {
    return ::access(path.c_str(), F_OK) == 0;
}

static std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return "";
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static long toLong(const std::string& s) {
    if (s.empty()) return 0;
    char* end = nullptr;
    long v = std::strtol(s.c_str(), &end, 10);
    if (end == s.c_str()) return 0;
    return v;
}

static double toDouble(const std::string& s) {
    if (s.empty()) return 0;
    char* end = nullptr;
    double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str()) return 0;
    return v;
}

static std::string env(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

static std::string baseName(const std::string& p) {
    size_t s = p.find_last_of('/');
    return s == NOPOS ? p : p.substr(s + 1);
}

static std::vector<std::string> listDir(const std::string& path) {
    std::vector<std::string> out;
    DIR* d = ::opendir(path.c_str());
    if (!d) return out;
    struct dirent* e;
    while ((e = ::readdir(d)) != nullptr) {
        std::string n = e->d_name;
        if (n == "." || n == "..") continue;
        out.push_back(n);
    }
    ::closedir(d);
    return out;
}

static bool isNumeric(const std::string& s) {
    if (s.empty()) return false;
    return std::all_of(s.begin(), s.end(), [](unsigned char c) { return c >= '0' && c <= '9'; });
}

static bool have(const std::string& cmd) {
    const char* pathEnv = std::getenv("PATH");
    if (!pathEnv) return false;
    std::string path = pathEnv;
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find(':', start);
        std::string dir = path.substr(start, end == NOPOS ? NOPOS : end - start);
        if (dir.empty()) dir = ".";
        if (::access((dir + "/" + cmd).c_str(), X_OK) == 0) return true;
        if (end == NOPOS) break;
        start = end + 1;
    }
    return false;
}

static bool g_hasTimeout = false;

static std::string T(const std::string& cmd) {
    if (!g_hasTimeout) return cmd;
    return "timeout 0.6s " + cmd;
}

static std::string run(const std::string& cmd) {
    FILE* p = ::popen(cmd.c_str(), "r");
    if (!p) return "";
    std::string out;
    char buf[4096];
    while (std::fgets(buf, sizeof buf, p)) out += buf;
    ::pclose(p);
    return out;
}

static std::string runFirstLine(const std::string& cmd) {
    return firstLine(run(cmd));
}

static long countLines(const std::string& s) {
    long n = 0;
    std::istringstream ss(s);
    std::string line;
    while (std::getline(ss, line)) {
        if (!trim(line).empty()) n++;
    }
    return n;
}

static std::string homeDir() {
    std::string h = env("HOME");
    if (!h.empty()) return h;
    struct passwd* pw = ::getpwuid(::getuid());
    return (pw && pw->pw_dir) ? std::string(pw->pw_dir) : std::string(".");
}

static std::string userName() {
    struct passwd* pw = ::getpwuid(::getuid());
    if (pw && pw->pw_name) return pw->pw_name;
    std::string u = env("USER");
    return u.empty() ? "user" : u;
}

/* Read a value from a simple INI file ("[group]" / "key=value"). */
static std::string iniValue(const std::string& file, const std::string& group,
                            const std::string& key) {
    std::ifstream in(file);
    if (!in) return "";
    std::string line, current;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line.front() == '[' && line.back() == ']') {
            current = line.substr(1, line.size() - 2);
            continue;
        }
        if (current != group) continue;
        size_t eq = line.find('=');
        if (eq == NOPOS) continue;
        if (trim(line.substr(0, eq)) == key) return trim(line.substr(eq + 1));
    }
    return "";
}

/* Extract the first version-like token from a line of text. */
static std::string firstVersion(const std::string& text) {
    std::smatch m;
    static const std::regex re(R"(([0-9]+\.[0-9]+(?:\.[0-9]+)?))");
    if (std::regex_search(text, m, re)) return m[1].str();
    return "";
}

/* ------------------------------------------------------------------ */
/* Unicode-aware display width (skips ANSI escapes)                    */
/* ------------------------------------------------------------------ */

static size_t codepointWidth(unsigned cp) {
    if (cp == 0 || cp < 32) return 0;
    if (cp >= 0x0300 && cp <= 0x036F) return 0; /* combining marks */
    if ((cp >= 0x1100 && cp <= 0x115F) ||
        (cp >= 0x2E80 && cp <= 0xA4CF) ||
        (cp >= 0xAC00 && cp <= 0xD7A3) ||
        (cp >= 0xF900 && cp <= 0xFAFF) ||
        (cp >= 0xFE30 && cp <= 0xFE6F) ||
        (cp >= 0xFF00 && cp <= 0xFF60) ||
        (cp >= 0xFFE0 && cp <= 0xFFE6) ||
        (cp >= 0x1F300 && cp <= 0x1FAFF) ||
        (cp >= 0x20000 && cp <= 0x3FFFD)) return 2;
    return 1;
}

static size_t visLen(const std::string& s) {
    size_t w = 0, i = 0, n = s.size();
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0x1b) {
            i++;
            if (i < n && s[i] == '[') {
                i++;
                while (i < n && !(s[i] >= 0x40 && s[i] <= 0x7e)) i++;
                if (i < n) i++;
            } else if (i < n && s[i] == ']') {
                i++;
                while (i < n && s[i] != '\a' &&
                       !(s[i] == 0x1b && i + 1 < n && s[i + 1] == '\\')) i++;
                if (i < n && s[i] == '\a') i++;
                else if (i + 1 < n && s[i] == 0x1b) i += 2;
            } else {
                i++;
            }
            continue;
        }

        unsigned cp = 0;
        int len = 1;
        if ((c & 0x80) == 0) {
            cp = c;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1F; len = 2;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0F; len = 3;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07; len = 4;
        } else {
            i++;
            continue;
        }
        for (int k = 1; k < len && i + (size_t)k < n; k++)
            cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
        w += codepointWidth(cp);
        i += len;
    }
    return w;
}

static std::string truncateVisible(const std::string& s, size_t maxW) {
    if (maxW == 0) return "";
    if (visLen(s) <= maxW) return s;
    if (maxW == 1) return col::reset() + "…";

    std::string out;
    size_t w = 0, i = 0, n = s.size();
    size_t target = maxW - 1;

    while (i < n && w < target) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0x1b) {
            size_t start = i;
            i++;
            if (i < n && s[i] == '[') {
                i++;
                while (i < n && !(s[i] >= 0x40 && s[i] <= 0x7e)) i++;
                if (i < n) i++;
            } else if (i < n && s[i] == ']') {
                i++;
                while (i < n && s[i] != '\a' &&
                       !(s[i] == 0x1b && i + 1 < n && s[i + 1] == '\\')) i++;
                if (i < n && s[i] == '\a') i++;
                else if (i + 1 < n && s[i] == 0x1b) i += 2;
            } else {
                i++;
            }
            out.append(s, start, i - start);
            continue;
        }

        unsigned cp = 0;
        int len = 1;
        if ((c & 0x80) == 0) cp = c;
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
        else { i++; continue; }
        if (i + (size_t)len > n) len = 1;
        for (int k = 1; k < len; k++)
            cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);

        size_t cw = codepointWidth(cp);
        if (w + cw > target) break;
        out.append(s, i, len);
        w += cw;
        i += len;
    }

    out += col::reset() + "…";
    return out;
}

static std::string humanBytes(unsigned long long bytes) {
    static const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
    if (bytes == 0) return "0 B";
    double v = (double)bytes;
    int u = 0;
    while (v >= 1024.0 && u < 5) { v /= 1024.0; u++; }
    char buf[64];
    if (v >= 100)      std::snprintf(buf, sizeof buf, "%.0f %s", v, units[u]);
    else if (v >= 10)  std::snprintf(buf, sizeof buf, "%.1f %s", v, units[u]);
    else               std::snprintf(buf, sizeof buf, "%.2f %s", v, units[u]);
    return buf;
}

/* ------------------------------------------------------------------ */
/* Distribution detection + colour palette                             */
/* ------------------------------------------------------------------ */

struct OsInfo {
    std::string id, name, pretty, version, versionId, idLike;
};

static std::string unquote(std::string v) {
    v = trim(v);
    if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'')) {
        char q = v.front();
        v = v.substr(1);
        if (!v.empty() && v.back() == q) v.pop_back();
    }
    return v;
}

static OsInfo detectOs() {
    OsInfo os;
    std::string data = readFile("/etc/os-release");
    if (data.empty()) data = readFile("/usr/lib/os-release");

    std::istringstream ss(data);
    std::string line;
    while (std::getline(ss, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == NOPOS) continue;
        std::string k = trim(line.substr(0, eq));
        std::string v = unquote(line.substr(eq + 1));
        if (k == "ID")             os.id = toLower(v);
        else if (k == "NAME")      os.name = v;
        else if (k == "PRETTY_NAME") os.pretty = v;
        else if (k == "VERSION")   os.version = v;
        else if (k == "VERSION_ID") os.versionId = v;
        else if (k == "ID_LIKE")   os.idLike = toLower(v);
    }

    if (os.pretty.empty()) os.pretty = os.name;

    if (os.pretty.empty() && exists("/etc/lsb-release")) {
        std::istringstream ls(readFile("/etc/lsb-release"));
        while (std::getline(ls, line)) {
            if (startsWith(line, "DISTRIB_DESCRIPTION="))
                os.pretty = unquote(line.substr(20));
        }
    }
    if (os.pretty.empty()) {
        std::string rh = readFile("/etc/redhat-release");
        if (!rh.empty()) os.pretty = firstLine(rh);
    }
    if (os.pretty.empty()) os.pretty = "Linux";
    if (os.id.empty()) os.id = "linux";
    return os;
}

static Palette paletteFor(const std::string& id) {
    auto has = [&](std::initializer_list<const char*> keys) {
        for (const char* k : keys)
            if (id.find(k) != NOPOS) return true;
        return false;
    };

    if (has({"arch"}))       return {{23, 147, 209}, {64, 224, 208}};
    if (has({"ubuntu"}))     return {{233, 84, 32},  {255, 176, 59}};
    if (has({"debian"}))     return {{215, 10, 60},  {255, 105, 140}};
    if (has({"fedora"}))     return {{41, 65, 122},  {96, 160, 255}};
    if (has({"mint"}))       return {{134, 190, 60}, {60, 210, 130}};
    if (has({"opensuse", "suse"})) return {{115, 186, 40}, {160, 230, 80}};
    if (has({"gentoo"}))     return {{120, 90, 180}, {190, 145, 255}};
    if (has({"void"}))       return {{71, 130, 60},  {150, 210, 90}};
    if (has({"alpine"}))     return {{35, 90, 160},  {90, 170, 240}};
    if (has({"manjaro"}))    return {{53, 190, 110}, {130, 235, 165}};
    if (has({"kali"}))       return {{40, 80, 180},  {95, 140, 255}};
    if (has({"raspbian", "raspberry"})) return {{190, 30, 60}, {245, 100, 130}};
    if (has({"elementary"})) return {{90, 130, 200}, {150, 190, 255}};
    if (has({"pop"}))        return {{72, 194, 225}, {90, 230, 200}};
    if (has({"endeavouros"}))return {{120, 60, 180}, {185, 110, 240}};
    if (has({"nixos"}))      return {{100, 150, 230}, {140, 205, 255}};
    if (has({"artix"}))      return {{60, 170, 220}, {120, 225, 235}};
    if (has({"slackware"}))  return {{70, 90, 150},  {130, 160, 220}};
    if (has({"centos", "rocky", "alma"})) return {{190, 70, 90}, {240, 130, 140}};
    if (has({"oracle"}))     return {{200, 40, 50},  {245, 100, 90}};
    if (has({"zorin"}))      return {{40, 130, 190}, {80, 190, 230}};
    if (has({"mx"}))         return {{70, 120, 180}, {110, 180, 230}};
    if (has({"freebsd"}))    return {{170, 30, 40},  {235, 80, 80}};
    if (has({"android"}))    return {{80, 200, 100}, {170, 240, 160}};
    return {{70, 205, 220}, {160, 120, 255}};
}

/* ------------------------------------------------------------------ */
/* Uptime / load / boot time                                           */
/* ------------------------------------------------------------------ */

static double uptimeSeconds() {
    return toDouble(firstLine(readFile("/proc/uptime")));
}

static std::string uptimeString() {
    long t = (long)uptimeSeconds();
    if (t <= 0) return "";
    long d = t / 86400, h = (t % 86400) / 3600, m = (t % 3600) / 60;
    std::ostringstream o;
    if (d > 0) o << d << (d == 1 ? " day, " : " days, ");
    if (d > 0 || h > 0) o << h << (h == 1 ? " hour, " : " hours, ");
    o << m << (m == 1 ? " min" : " mins");
    return o.str();
}

static std::string bootTimeString() {
    time_t boot = std::time(nullptr) - (time_t)uptimeSeconds();
    struct tm tmv;
    if (!::localtime_r(&boot, &tmv)) return "";
    char buf[64];
    if (std::strftime(buf, sizeof buf, "%a %b %e %H:%M", &tmv) == 0) return "";
    return buf;
}

static std::string loadAverage() {
    std::string data = firstLine(readFile("/proc/loadavg"));
    std::istringstream ss(data);
    double a = 0, b = 0, c = 0;
    if (!(ss >> a >> b >> c)) return "";
    char buf[96];
    std::snprintf(buf, sizeof buf, "%.2f, %.2f, %.2f", a, b, c);
    return buf;
}

static long processCount() {
    long n = 0;
    for (const auto& e : listDir("/proc"))
        if (isNumeric(e)) n++;
    return n;
}

/* ------------------------------------------------------------------ */
/* Host / firmware                                                     */
/* ------------------------------------------------------------------ */

static std::string dmi(const std::string& name) {
    return trim(readFile("/sys/devices/virtual/dmi/id/" + name));
}

static bool meaninglessDmi(const std::string& s) {
    std::string l = toLower(s);
    return l.empty() || l == "system product name" || l == "system version" ||
           l == "to be filled by o.e.m." || l == "default string" ||
           l == "not specified" || l == "not applicable" || l == "none" ||
           l == "type1productconfigid" || l == "o.e.m." || l == "unknown";
}

static std::string hostName() {
    std::string h = dmi("product_name");
    if (meaninglessDmi(h)) h = dmi("board_name");
    if (meaninglessDmi(h)) h = dmi("product_version");
    if (meaninglessDmi(h)) {
        struct utsname u;
        if (::uname(&u) == 0) h = u.nodename;
    }
    return h;
}

static std::string biosString() {
    std::string v = dmi("bios_version");
    std::string d = dmi("bios_date");
    if (meaninglessDmi(v)) v.clear();
    if (meaninglessDmi(d)) d.clear();
    if (v.empty() && d.empty()) return "";
    if (v.empty()) return d;
    if (d.empty()) return v;
    return v + " (" + d + ")";
}

/* ------------------------------------------------------------------ */
/* CPU                                                                 */
/* ------------------------------------------------------------------ */

struct CpuInfo {
    std::string model;
    int cores = 0;
    int threads = 0;
    double maxGhz = 0;
    double tempC = -1;
};

static double cpuTemperature() {
    static const char* wanted[] = {
        "coretemp", "k10temp", "zenpower", "cpu_thermal", "cpu-thermal",
        "soc_thermal", "soc-thermal", "cpu"
    };
    double best = -1;

    for (const auto& h : listDir("/sys/class/hwmon")) {
        std::string base = "/sys/class/hwmon/" + h;
        std::string name = toLower(firstLine(readFile(base + "/name")));
        bool ok = false;
        for (const char* w : wanted) {
            if (!name.empty() && name.find(w) != NOPOS) { ok = true; break; }
        }
        if (!ok) continue;
        for (const auto& f : listDir(base)) {
            if (!startsWith(f, "temp") || !endsWith(f, "_input")) continue;
            long milli = toLong(firstLine(readFile(base + "/" + f)));
            if (milli > 0) best = std::max(best, milli / 1000.0);
        }
    }
    if (best > 0 && best < 200) return best;

    best = -1;
    for (const auto& z : listDir("/sys/class/thermal")) {
        if (!startsWith(z, "thermal_zone")) continue;
        std::string base = "/sys/class/thermal/" + z;
        std::string type = toLower(firstLine(readFile(base + "/type")));
        if (type.find("cpu") == NOPOS && type.find("soc") == NOPOS &&
            type.find("x86_pkg_temp") == NOPOS) continue;
        long milli = toLong(firstLine(readFile(base + "/temp")));
        if (milli > 0) best = std::max(best, milli / 1000.0);
    }
    return (best > 0 && best < 200) ? best : -1;
}

static CpuInfo collectCpu() {
    CpuInfo c;
    std::istringstream ss(readFile("/proc/cpuinfo"));
    std::string line, curPhys, hardware, processorLabel;
    double maxMhz = 0;
    std::set<std::string> cores;

    while (std::getline(ss, line)) {
        size_t colon = line.find(':');
        if (colon == NOPOS) continue;
        std::string k = trim(line.substr(0, colon));
        std::string v = trim(line.substr(colon + 1));

        if (k == "processor") {
            c.threads++;
            processorLabel = v;
        } else if (k == "model name" && c.model.empty()) {
            c.model = v;
        } else if (k == "Hardware" && hardware.empty()) {
            hardware = v;
        } else if (k == "Processor" && processorLabel.empty()) {
            processorLabel = v;
        } else if (k == "physical id") {
            curPhys = v;
        } else if (k == "core id") {
            cores.insert(curPhys + ":" + v);
        } else if (k == "cpu MHz") {
            maxMhz = std::max(maxMhz, toDouble(v));
        }
    }

    if (c.model.empty()) c.model = hardware.empty() ? processorLabel : hardware;
    if (c.threads == 0) {
        long n = ::sysconf(_SC_NPROCESSORS_ONLN);
        c.threads = n > 0 ? (int)n : 1;
    }
    c.cores = cores.empty() ? c.threads : (int)cores.size();

    long khz = toLong(firstLine(readFile(
        "/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq")));
    if (khz > 0) c.maxGhz = khz / 1000000.0;
    else if (maxMhz > 0) c.maxGhz = maxMhz / 1000.0;

    c.tempC = cpuTemperature();
    return c;
}

static std::string cpuString(const CpuInfo& c) {
    std::ostringstream o;
    if (!c.model.empty()) o << c.model << " ";
    if (c.cores > 0 && c.threads > 0 && c.cores != c.threads)
        o << "(" << c.cores << "C/" << c.threads << "T)";
    else if (c.threads > 0)
        o << "(" << c.threads << "T)";
    if (c.maxGhz > 0) {
        o << " @ " << std::fixed << std::setprecision(2) << c.maxGhz << " GHz";
    }
    if (c.tempC > 0) {
        o << " [" << (int)std::lround(c.tempC) << "°C]";
    }
    return trim(o.str());
}

/* ------------------------------------------------------------------ */
/* Memory / swap / disk                                                */
/* ------------------------------------------------------------------ */

static std::map<std::string, unsigned long long> meminfo() {
    std::map<std::string, unsigned long long> m;
    std::istringstream ss(readFile("/proc/meminfo"));
    std::string line;
    while (std::getline(ss, line)) {
        size_t colon = line.find(':');
        if (colon == NOPOS) continue;
        std::string k = trim(line.substr(0, colon));
        std::string v = trim(line.substr(colon + 1));
        unsigned long long kb = (unsigned long long)toLong(v);
        m[k] = kb;
    }
    return m;
}

static std::string bar(double pct, int width) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int filled = (int)std::lround(pct * width / 100.0);
    RGB c = pct < 60 ? RGB{80, 220, 120}
          : pct < 85 ? RGB{240, 200, 80}
                     : RGB{240, 90, 90};

    std::string out;
    for (int i = 0; i < filled; i++) out += col::fg(c.r, c.g, c.b) + "█";
    if (filled < width) {
        out += col::reset() + col::dim();
        for (int i = filled; i < width; i++) out += "░";
    }
    out += col::reset();
    return out;
}

static std::string memoryString(const std::map<std::string, unsigned long long>& m) {
    unsigned long long total = m.count("MemTotal") ? m.at("MemTotal") : 0;
    unsigned long long avail = m.count("MemAvailable") ? m.at("MemAvailable") : 0;
    if (total == 0) return "";
    if (avail == 0 && m.count("MemFree")) avail = m.at("MemFree");
    unsigned long long used = total > avail ? total - avail : 0;
    double pct = total ? used * 100.0 / total : 0;
    std::ostringstream o;
    o << humanBytes(used * 1024ULL) << " / " << humanBytes(total * 1024ULL)
      << " (" << (int)std::lround(pct) << "%)  " << bar(pct, 10);
    return o.str();
}

static std::string swapString(const std::map<std::string, unsigned long long>& m) {
    unsigned long long total = m.count("SwapTotal") ? m.at("SwapTotal") : 0;
    unsigned long long freeKb = m.count("SwapFree") ? m.at("SwapFree") : 0;
    if (total == 0) return "";
    unsigned long long used = total > freeKb ? total - freeKb : 0;
    double pct = used * 100.0 / total;
    std::ostringstream o;
    o << humanBytes(used * 1024ULL) << " / " << humanBytes(total * 1024ULL)
      << " (" << (int)std::lround(pct) << "%)  " << bar(pct, 10);
    return o.str();
}

static std::string diskString() {
    struct statvfs vfs;
    if (::statvfs("/", &vfs) != 0) return "";
    unsigned long long total = (unsigned long long)vfs.f_blocks * vfs.f_frsize;
    unsigned long long avail = (unsigned long long)vfs.f_bavail * vfs.f_frsize;
    if (total == 0) return "";
    unsigned long long used = total > avail ? total - avail : 0;
    double pct = used * 100.0 / total;
    std::ostringstream o;
    o << humanBytes(used) << " / " << humanBytes(total)
      << " (" << (int)std::lround(pct) << "%)  " << bar(pct, 10);
    return o.str();
}

/* ------------------------------------------------------------------ */
/* Display resolution                                                  */
/* ------------------------------------------------------------------ */

static std::string displayInfo() {
    std::string xr = run(T("xrandr --current 2>/dev/null"));
    std::smatch m;
    static const std::regex modeRe(R"(([0-9]{3,5}x[0-9]{3,5})\s+([0-9.]+)\*)");
    if (std::regex_search(xr, m, modeRe)) {
        long hz = (long)std::lround(toDouble(m[2].str()));
        std::ostringstream o;
        o << m[1].str() << " @ " << hz << " Hz";
        return o.str();
    }

    for (const auto& e : listDir("/sys/class/drm")) {
        if (e.find('-') == NOPOS) continue;
        std::string base = "/sys/class/drm/" + e;
        if (firstLine(readFile(base + "/status")) != "connected") continue;
        std::string mode = firstLine(readFile(base + "/modes"));
        if (!mode.empty()) return mode;
    }
    return "";
}

/* ------------------------------------------------------------------ */
/* Package counts                                                      */
/* ------------------------------------------------------------------ */

static long commandCount(const std::string& cmd) {
    long v = toLong(firstLine(run(cmd)));
    return v > 0 ? v : 0;
}

static std::string packagesString() {
    std::vector<std::pair<std::string, long>> found;

    if (have("pacman")) {
        long n = commandCount("pacman -Qq 2>/dev/null | wc -l");
        if (n) found.push_back({"pacman", n});
    }
    if (exists("/var/lib/dpkg/status")) {
        long n = commandCount("grep -c '^Package:' /var/lib/dpkg/status 2>/dev/null");
        if (n) found.push_back({"dpkg", n});
    }
    if (have("rpm") && (exists("/var/lib/rpm") || exists("/usr/lib/sysimage/rpm"))) {
        long n = commandCount("rpm -qa 2>/dev/null | wc -l");
        if (n) found.push_back({"rpm", n});
    }
    if (have("apk")) {
        long n = commandCount("apk info 2>/dev/null | wc -l");
        if (n) found.push_back({"apk", n});
    }
    if (have("xbps-query")) {
        long n = commandCount("xbps-query -l 2>/dev/null | wc -l");
        if (n) found.push_back({"xbps", n});
    }
    if (exists("/var/db/pkg")) {
        long n = 0;
        for (const auto& category : listDir("/var/db/pkg"))
            if (!category.empty() && category[0] != '.')
                n += (long)listDir("/var/db/pkg/" + category).size();
        if (n) found.push_back({"portage", n});
    }
    if (have("flatpak")) {
        long n = commandCount(T("flatpak list --app --columns=application 2>/dev/null | wc -l"));
        if (n) found.push_back({"flatpak", n});
    }
    if (have("snap")) {
        long n = commandCount(T("snap list 2>/dev/null | tail -n +2 | wc -l"));
        if (n) found.push_back({"snap", n});
    }
    if (found.empty()) return "";

    std::ostringstream o;
    for (size_t i = 0; i < found.size(); i++) {
        if (i) o << ", ";
        o << found[i].second << " (" << found[i].first << ")";
    }
    return o.str();
}

/* ------------------------------------------------------------------ */
/* Battery                                                             */
/* ------------------------------------------------------------------ */

static std::string batteryString() {
    int sum = 0, count = 0;
    std::string status;

    for (const auto& e : listDir("/sys/class/power_supply")) {
        std::string base = "/sys/class/power_supply/" + e;
        if (firstLine(readFile(base + "/type")) != "Battery") continue;
        long cap = toLong(firstLine(readFile(base + "/capacity")));
        if (cap < 0 || cap > 100) continue;
        sum += (int)cap;
        count++;
        if (status.empty()) {
            std::string s = firstLine(readFile(base + "/status"));
            if (!s.empty() && s != "Unknown") status = s;
        }
    }
    if (count == 0) return "";

    int avg = sum / count;
    std::ostringstream o;
    o << avg << "%";
    if (!status.empty()) o << " (" << status << ")";
    o << "  " << bar(avg, 10);
    return o.str();
}

/* ------------------------------------------------------------------ */
/* Network                                                             */
/* ------------------------------------------------------------------ */

struct RouteInfo { std::string iface, gateway; };

static RouteInfo defaultRoute() {
    RouteInfo r;
    std::ifstream in("/proc/net/route");
    std::string line;
    std::getline(in, line); /* header */
    while (std::getline(in, line)) {
        std::istringstream ss(line);
        std::string iface, dest, gw, flags;
        if (!(ss >> iface >> dest >> gw >> flags)) continue;
        if (dest != "00000000") continue;

        r.iface = iface;
        unsigned long raw = std::strtoul(gw.c_str(), nullptr, 16);
        if (raw != 0) {
            struct in_addr addr;
            addr.s_addr = (in_addr_t)raw;
            char buf[INET_ADDRSTRLEN] = {0};
            if (::inet_ntop(AF_INET, &addr, buf, sizeof buf))
                r.gateway = buf;
        }
        break;
    }
    return r;
}

static std::string connectionType(const std::string& iface) {
    if (iface.empty()) return "";
    std::string base = "/sys/class/net/" + iface;

    if (exists(base + "/wireless") || startsWith(iface, "wl")) return "Wi-Fi";
    if (startsWith(iface, "tun") || startsWith(iface, "tap") ||
        startsWith(iface, "wg")) return "VPN";
    if (startsWith(iface, "br-")) return "Bridge";
    if (startsWith(iface, "veth") || startsWith(iface, "docker") ||
        startsWith(iface, "virbr")) return "Virtual";
    if (firstLine(readFile(base + "/type")) == "1") return "Ethernet";
    return "Network";
}

static std::string dnsServer() {
    auto parseResolv = [](const std::string& text) {
        std::istringstream ss(text);
        std::string line;
        while (std::getline(ss, line)) {
            line = trim(line);
            if (!startsWith(line, "nameserver")) continue;
            std::istringstream ls(line);
            std::string key, value;
            if (ls >> key >> value) return value;
        }
        return std::string();
    };

    std::string dns = parseResolv(readFile("/etc/resolv.conf"));
    if (dns == "127.0.0.53" || dns == "127.0.0.1") {
        std::string real = parseResolv(readFile("/run/systemd/resolve/resolv.conf"));
        if (!real.empty()) dns = real;
    }
    return dns;
}

struct NetInfo {
    std::string ip, iface, connection, gateway, dns;
};

static NetInfo collectNetwork() {
    NetInfo n;
    RouteInfo route = defaultRoute();
    std::string want = route.iface;
    std::string fallbackIp, fallbackIface;

    struct ifaddrs* list = nullptr;
    if (::getifaddrs(&list) != 0) return n;

    for (struct ifaddrs* ia = list; ia; ia = ia->ifa_next) {
        if (!ia->ifa_addr || ia->ifa_addr->sa_family != AF_INET) continue;
        if (ia->ifa_flags & IFF_LOOPBACK) continue;
        char buf[INET_ADDRSTRLEN] = {0};
        auto* sin = reinterpret_cast<struct sockaddr_in*>(ia->ifa_addr);
        if (!::inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof buf)) continue;
        std::string name = ia->ifa_name;
        if (!want.empty() && name == want) {
            n.ip = buf;
            n.iface = name;
        }
        if (fallbackIp.empty() && (ia->ifa_flags & IFF_UP)) {
            fallbackIp = buf;
            fallbackIface = name;
        }
    }
    ::freeifaddrs(list);

    if (n.ip.empty()) {
        n.ip = fallbackIp;
        n.iface = fallbackIface;
    }
    n.connection = connectionType(n.iface);
    n.gateway = route.gateway;
    n.dns = dnsServer();
    return n;
}

/* ------------------------------------------------------------------ */
/* Desktop / WM / themes                                               */
/* ------------------------------------------------------------------ */

static std::string detectDe() {
    std::string s = env("XDG_CURRENT_DESKTOP");
    if (s.empty()) s = env("XDG_SESSION_DESKTOP");
    if (s.empty()) s = env("DESKTOP_SESSION");
    std::string l = toLower(s);

    struct Entry { const char* key; const char* name; };
    static const Entry map[] = {
        {"gnome", "GNOME"},          {"kde", "KDE Plasma"},
        {"plasma", "KDE Plasma"},    {"xfce", "Xfce"},
        {"mate", "MATE"},            {"cinnamon", "Cinnamon"},
        {"budgie", "Budgie"},        {"lxqt", "LXQt"},
        {"lxde", "LXDE"},            {"deepin", "Deepin"},
        {"pantheon", "Pantheon"},    {"hyprland", "Hyprland"},
        {"sway", "Sway"},            {"i3", "i3"},
        {"bspwm", "bspwm"},          {"openbox", "Openbox"},
        {"awesome", "Awesome"},      {"qtile", "Qtile"},
        {"river", "River"},          {"wayfire", "Wayfire"},
        {"labwc", "LabWC"},          {"niri", "Niri"},
        {"unity", "Unity"},          {"cosmic", "COSMIC"},
        {"enlightenment", "Enlightenment"},
        {"cutefish", "Cutefish"},
    };
    for (const auto& e : map)
        if (l.find(e.key) != NOPOS) return e.name;
    return s;
}

static std::string detectWm(const std::string& de) {
    struct Entry { const char* proc; const char* name; };
    static const Entry map[] = {
        {"hyprland", "Hyprland"},   {"sway", "Sway"},
        {"i3", "i3"},               {"bspwm", "bspwm"},
        {"openbox", "Openbox"},     {"xfwm4", "Xfwm4"},
        {"gnome-shell", "Mutter"},  {"mutter", "Mutter"},
        {"kwin_wayland", "KWin"},   {"kwin_x11", "KWin"},
        {"kwin", "KWin"},           {"cinnamon", "Muffin"},
        {"muffin", "Muffin"},       {"marco", "Marco"},
        {"awesome", "Awesome"},     {"dwm", "dwm"},
        {"qtile", "Qtile"},         {"river", "River"},
        {"wayfire", "Wayfire"},     {"labwc", "LabWC"},
        {"niri", "Niri"},           {"herbstluftwm", "herbstluftwm"},
        {"fluxbox", "Fluxbox"},     {"icewm", "IceWM"},
        {"jwm", "JWM"},             {"fvwm3", "FVWM"},
        {"fvwm", "FVWM"},           {"spectrwm", "Spectrwm"},
        {"xmonad", "XMonad"},       {"leftwm", "LeftWM"},
        {"hlwm", "herbstluftwm"},
    };

    for (const auto& pid : listDir("/proc")) {
        if (!isNumeric(pid)) continue;
        std::string comm = firstLine(readFile("/proc/" + pid + "/comm"));
        if (comm.empty()) continue;
        for (const auto& e : map) {
            if (comm == e.proc || startsWith(comm, std::string(e.proc) + " ") ||
                startsWith(comm, std::string(e.proc) + ":"))
                return e.name;
        }
    }

    if (de.find("GNOME") != NOPOS) return "Mutter";
    if (de.find("KDE") != NOPOS) return "KWin";
    if (de.find("Xfce") != NOPOS) return "Xfwm4";
    if (de.find("MATE") != NOPOS) return "Marco";
    if (de.find("Cinnamon") != NOPOS) return "Muffin";
    if (de.find("LXQt") != NOPOS) return "Openbox";
    return "";
}

static bool graphicalSession() {
    return !env("DISPLAY").empty() || !env("WAYLAND_DISPLAY").empty();
}

static std::string gs(const std::string& schemaKey) {
    if (!have("gsettings")) return "";
    std::string v = trim(run(T("gsettings get " + schemaKey + " 2>/dev/null")));
    v = unquote(v);
    if (v.empty() || v == "none" || v == "''") return "";
    return v;
}

static std::string xfconf(const std::string& channel, const std::string& prop) {
    if (!have("xfconf-query")) return "";
    return trim(run(T("xfconf-query -c " + channel + " -p " + prop + " 2>/dev/null")));
}

static std::string firstNonEmpty(std::initializer_list<std::string> values) {
    for (const auto& v : values)
        if (!v.empty()) return v;
    return "";
}

struct ThemeInfo {
    std::string wmTheme, theme, icons, cursor, font;
};

static ThemeInfo collectThemes(const std::string& de) {
    ThemeInfo t;
    if (!graphicalSession()) return t;

    std::string home = homeDir();
    bool kde = de.find("KDE") != NOPOS;
    bool mate = de.find("MATE") != NOPOS;
    bool cinnamon = de.find("Cinnamon") != NOPOS;
    bool xfce = de.find("Xfce") != NOPOS;

    if (kde) {
        t.theme  = iniValue(home + "/.config/kdeglobals", "General", "ColorScheme");
        t.icons  = iniValue(home + "/.config/kdeglobals", "Icons", "Theme");
        t.font   = iniValue(home + "/.config/kdeglobals", "General", "font");
        t.wmTheme = iniValue(home + "/.config/kwinrc", "org.kde.kdecoration2", "theme");
    }

    t.theme = firstNonEmpty({
        t.theme,
        mate ? gs("org.mate.interface gtk-theme") : "",
        cinnamon ? gs("org.cinnamon.desktop.interface gtk-theme") : "",
        gs("org.gnome.desktop.interface gtk-theme"),
        xfce ? xfconf("xsettings", "/Net/ThemeName") : "",
    });

    t.icons = firstNonEmpty({
        t.icons,
        mate ? gs("org.mate.interface icon-theme") : "",
        cinnamon ? gs("org.cinnamon.desktop.interface icon-theme") : "",
        gs("org.gnome.desktop.interface icon-theme"),
        xfce ? xfconf("xsettings", "/Net/IconThemeName") : "",
    });

    t.cursor = firstNonEmpty({
        gs("org.gnome.desktop.interface cursor-theme"),
        mate ? gs("org.mate.peripherals-mouse cursor-theme") : "",
        xfce ? xfconf("xsettings", "/Gtk/CursorThemeName") : "",
    });

    t.font = firstNonEmpty({
        t.font,
        mate ? gs("org.mate.interface font-name") : "",
        cinnamon ? gs("org.cinnamon.desktop.interface font-name") : "",
        gs("org.gnome.desktop.interface font-name"),
        xfce ? xfconf("xsettings", "/Gtk/FontName") : "",
    });

    if (t.wmTheme.empty()) {
        t.wmTheme = firstNonEmpty({
            mate ? gs("org.mate.Marco.general theme") : "",
            cinnamon ? gs("org.cinnamon.desktop.wm.preferences theme") : "",
            xfce ? xfconf("xfwm4", "/general/theme") : "",
            gs("org.gnome.desktop.wm.preferences theme"),
        });
    }
    return t;
}

/* ------------------------------------------------------------------ */
/* Terminal                                                            */
/* ------------------------------------------------------------------ */

static const char* terminalDisplayName(const std::string& comm) {
    struct Entry { const char* proc; const char* name; };
    static const Entry map[] = {
        {"gnome-terminal", "GNOME Terminal"}, {"konsole", "Konsole"},
        {"alacritty", "Alacritty"},           {"kitty", "kitty"},
        {"wezterm-gui", "WezTerm"},           {"wezterm", "WezTerm"},
        {"foot", "foot"},                     {"xterm", "XTerm"},
        {"uxterm", "UXTerm"},                 {"tilix", "Tilix"},
        {"terminator", "Terminator"},         {"urxvt", "urxvt"},
        {"xfce4-terminal", "Xfce Terminal"},  {"mate-terminal", "MATE Terminal"},
        {"lxterminal", "LXTerminal"},         {"qterminal", "QTerminal"},
        {"terminology", "Terminology"},       {"ghostty", "Ghostty"},
        {"st", "st"},                         {"rio", "Rio"},
        {"contour", "Contour"},               {"code", "VS Code"},
        {"code-oss", "VS Code"},              {"hyper", "Hyper"},
        {"tabby", "Tabby"},                   {"yakuake", "Yakuake"},
        {"guake", "Guake"},                   {"tilda", "Tilda"},
        {"cool-retro-term", "cool-retro-term"},
    };
    for (const auto& e : map) {
        if (comm == e.proc || startsWith(comm, std::string(e.proc) + "-"))
            return e.name;
    }
    return nullptr;
}

static std::string detectTerminal() {
    std::string tp = env("TERM_PROGRAM");
    if (!tp.empty()) {
        std::string name = tp;
        if (tp == "vscode") name = "VS Code";
        else if (tp == "Apple_Terminal") name = "Apple Terminal";
        std::string ver = env("TERM_PROGRAM_VERSION");
        return ver.empty() ? name : name + " " + ver;
    }

    bool multiplexer = false;
    pid_t pid = ::getppid();
    for (int depth = 0; depth < 16 && pid > 1; depth++) {
        std::string base = "/proc/" + std::to_string((long)pid);
        std::string comm = firstLine(readFile(base + "/comm"));
        if (comm.empty()) break;

        if (comm == "tmux" || startsWith(comm, "tmux:") ||
            comm == "screen" || startsWith(comm, "screen-")) {
            multiplexer = true;
        } else if (const char* name = terminalDisplayName(comm)) {
            std::string out = name;
            if (multiplexer) out += " (tmux)";
            return out;
        }

        std::string stat = readFile(base + "/stat");
        size_t rp = stat.rfind(')');
        if (rp == NOPOS) break;
        std::istringstream ss(stat.substr(rp + 1));
        std::string state;
        long ppid = 0;
        if (!(ss >> state >> ppid)) break;
        pid = (pid_t)ppid;
    }

    if (multiplexer) return "tmux";

    std::string term = env("TERM");
    if (!term.empty() && term != "dumb" && term != "unknown") return term;
    return "";
}

static std::string configValueLine(const std::string& text, const std::string& key) {
    std::istringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        std::string l = trim(line);
        if (l.empty() || l[0] == '#' || l[0] == ';') continue;
        if (!startsWith(l, key)) continue;
        if (l.size() > key.size() && std::isalnum((unsigned char)l[key.size()]))
            continue;
        std::string v = trim(l.substr(key.size()));
        if (!v.empty() && (v[0] == '=' || v[0] == ':')) v = trim(v.substr(1));
        if (v.empty()) continue;
        size_t comment = v.find(" #");
        if (comment != NOPOS) v = trim(v.substr(0, comment));
        if (v.size() >= 2 && (v.front() == '"' || v.front() == '\''))
            v = unquote(v);
        if (!v.empty() && v.back() == ',') v.pop_back();
        return trim(v);
    }
    return "";
}

static std::string terminalFont(const std::string& terminal) {
    std::string t = toLower(terminal);
    std::string home = homeDir();

    if (t.find("kitty") != NOPOS) {
        std::string v = configValueLine(
            readFile(home + "/.config/kitty/kitty.conf"), "font_family");
        if (!v.empty()) return v;
    }
    if (t.find("alacritty") != NOPOS) {
        static const char* files[] = {
            "/.config/alacritty/alacritty.toml",
            "/.config/alacritty/alacritty.yml",
            "/.config/alacritty/alacritty.yaml"
        };
        for (const char* f : files) {
            std::string v = configValueLine(readFile(home + f), "family");
            if (!v.empty()) return v;
        }
    }
    if (t.find("foot") != NOPOS) {
        std::string v = configValueLine(readFile(home + "/.config/foot/foot.ini"), "font");
        if (!v.empty()) return v;
    }
    if (t.find("wezterm") != NOPOS) {
        std::string cfg = readFile(home + "/.config/wezterm/wezterm.lua");
        std::smatch m;
        static const std::regex re(R"re(wezterm\.font\s*\(\s*"([^"]+)"\))re");
        if (std::regex_search(cfg, m, re)) return m[1].str();
    }
    if (t.find("urxvt") != NOPOS) {
        std::string v = configValueLine(readFile(home + "/.Xresources"), "URxvt.font");
        if (!v.empty()) return v;
    }
    return "";
}

/* ------------------------------------------------------------------ */
/* GPU                                                                 */
/* ------------------------------------------------------------------ */

static std::string cleanGpuName(std::string n) {
    struct Replacement { const char* from; const char* to; };
    static const Replacement reps[] = {
        {"Advanced Micro Devices, Inc. [AMD/ATI] ", "AMD "},
        {"Advanced Micro Devices, Inc. [AMD] ", "AMD "},
        {"Advanced Micro Devices, Inc. ", "AMD "},
        {"NVIDIA Corporation ", "NVIDIA "},
        {"Intel Corporation ", "Intel "},
        {"Corporation ", ""},
    };
    for (const auto& r : reps) {
        if (startsWith(n, r.from)) {
            n = std::string(r.to) + n.substr(std::strlen(r.from));
            break;
        }
    }
    return trim(n);
}

static std::string gpuString() {
    std::vector<std::string> gpus;

    std::string out = run(T("lspci 2>/dev/null"));
    std::istringstream ss(out);
    std::string line;
    while (std::getline(ss, line)) {
        std::string l = toLower(line);
        if (l.find("vga compatible controller") == NOPOS &&
            l.find("3d controller") == NOPOS &&
            l.find("display controller") == NOPOS) continue;

        size_t p = line.find(": ");
        std::string name = (p == NOPOS) ? line : line.substr(p + 2);
        size_t rev = name.find(" (rev ");
        if (rev != NOPOS) name = name.substr(0, rev);
        name = cleanGpuName(trim(name));
        if (!name.empty()) gpus.push_back(name);
    }

    if (gpus.empty() && have("glxinfo")) {
        std::string g = run(T("glxinfo -B 2>/dev/null"));
        std::istringstream gs(g);
        while (std::getline(gs, line)) {
            if (startsWith(line, "Device:") || startsWith(line, "OpenGL renderer string:")) {
                size_t c = line.find(':');
                if (c != NOPOS) {
                    std::string name = trim(line.substr(c + 1));
                    if (!name.empty()) { gpus.push_back(name); break; }
                }
            }
        }
    }

    std::string res;
    for (size_t i = 0; i < gpus.size() && i < 2; i++) {
        if (i) res += " | ";
        res += gpus[i];
    }
    if (gpus.size() > 2)
        res += " (+" + std::to_string(gpus.size() - 2) + " more)";
    return res;
}

/* ------------------------------------------------------------------ */
/* Locale / timezone / init / users                                    */
/* ------------------------------------------------------------------ */

static std::string localeString() {
    std::string l = env("LC_ALL");
    if (l.empty()) l = env("LC_MESSAGES");
    if (l.empty()) l = env("LANG");
    return l.empty() ? "C" : l;
}

static std::string readLink(const std::string& path) {
    char buf[4096];
    ssize_t n = ::readlink(path.c_str(), buf, sizeof buf - 1);
    if (n <= 0) return "";
    buf[n] = '\0';
    return buf;
}

static std::string timezoneString() {
    std::string tz = env("TZ");
    if (!tz.empty()) {
        if (tz[0] == ':') tz = tz.substr(1);
        return tz;
    }
    std::string etc = firstLine(readFile("/etc/timezone"));
    if (!etc.empty()) return etc;
    std::string link = readLink("/etc/localtime");
    size_t p = link.find("/zoneinfo/");
    if (p != NOPOS) return link.substr(p + 10);
    return "";
}

static std::string initString() {
    std::string comm = firstLine(readFile("/proc/1/comm"));
    if (comm == "systemd") return "systemd";
    if (comm == "init") return "SysV init";
    if (comm == "openrc-init") return "OpenRC";
    if (comm == "runit") return "runit";
    if (comm == "s6-svscan") return "s6";
    if (comm == "dinit") return "dinit";
    if (comm == "busybox") return "BusyBox init";
    return comm;
}

static std::string usersString() {
    long n = countLines(run(T("who 2>/dev/null")));
    if (n <= 0) return "";
    return std::to_string(n) + " logged in";
}

static std::string sessionString() {
    std::string s = env("XDG_SESSION_TYPE");
    if (s.empty()) {
        if (!env("WAYLAND_DISPLAY").empty()) s = "wayland";
        else if (!env("DISPLAY").empty()) s = "x11";
        else s = "tty";
    }
    if (s == "x11") return "X11";
    if (s == "wayland") return "Wayland";
    return s;
}

static std::string colorBlocks() {
    static const int palette[16][3] = {
        {30, 30, 30},    {205, 49, 49},   {13, 188, 121},  {229, 229, 16},
        {36, 114, 200},  {188, 63, 188},  {17, 168, 205},  {229, 229, 229},
        {102, 102, 102}, {241, 76, 76},   {35, 209, 139},  {245, 245, 67},
        {59, 142, 234},  {214, 112, 214}, {41, 184, 219},  {255, 255, 255},
    };
    std::string out;
    for (int i = 0; i < 16; i++) {
        if (i == 8) out += " ";
        out += col::bg(palette[i][0], palette[i][1], palette[i][2]) + "  ";
    }
    out += col::reset();
    return out;
}

/* ------------------------------------------------------------------ */
/* The fish                                                            */
/* ------------------------------------------------------------------ */

static std::vector<std::string> fishLogo() {
    return {
        R"(                                o)",
        R"(                              o)",
        R"(                  ______)",
        R"(               .-'      '-.)",
        R"(             .'            '.)",
        R"(            /    O      o    \)",
        R"(           |      o  o        |)",
        R"(       /\  |   o      o    o  >)",
        R"(      /  \ |     o  o      o  |)",
        R"(      \  /  \   o      o     /)",
        R"(       \/    '.            .')",
        R"(               '-.______.-')",
        R"()",
        R"(         ~  V L   F E T C H  ~)",
    };
}

/* ------------------------------------------------------------------ */
/* Rendering                                                           */
/* ------------------------------------------------------------------ */

struct Row {
    std::string label;
    std::string value;
    bool section = false;
};

static std::string shellInfo() {
    std::string sh = env("SHELL");
    if (sh.empty()) {
        struct passwd* pw = ::getpwuid(::getuid());
        if (pw && pw->pw_shell) sh = pw->pw_shell;
    }
    if (sh.empty()) return "";
    std::string name = baseName(sh);
    std::string version = firstVersion(
        runFirstLine(T("'" + sh + "' --version 2>/dev/null")));
    return version.empty() ? name : name + " " + version;
}

static std::string paintText(const std::string& s, RGB a, RGB b, bool bold) {
    if (!col::enabled) return s;
    std::string out = bold ? col::bold() : "";
    size_t n = s.size(), i = 0;
    while (i < n) {
        if (s[i] == ' ') {
            out += ' ';
            i++;
            continue;
        }
        int len = 1;
        unsigned char c = (unsigned char)s[i];
        if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        if (i + (size_t)len > n) len = 1;

        double t = n > 1 ? (double)i / (double)(n - 1) : 0.0;
        RGB c2 = lerp(a, b, t);
        out += col::fg(c2.r, c2.g, c2.b);
        out.append(s, i, len);
        i += len;
    }
    out += col::reset();
    return out;
}

static std::string paintLogoLine(const std::string& line, size_t y,
                                 size_t rows, size_t cols, const Palette& p) {
    if (!col::enabled) return line;
    std::string out = col::bold();
    for (size_t x = 0; x < line.size(); x++) {
        if (line[x] == ' ') {
            out += ' ';
            continue;
        }
        double ty = rows > 1 ? (double)y / (double)(rows - 1) : 0.0;
        double tx = cols > 1 ? (double)x / (double)(cols - 1) : 0.0;
        RGB c = lerp(p.a, p.b, 0.6 * ty + 0.4 * tx);
        out += col::fg(c.r, c.g, c.b);
        out += line[x];
    }
    out += col::reset();
    return out;
}

static std::string infoLine(const Row& r, size_t labelW, const Palette& p) {
    size_t lw = visLen(r.label);
    std::string pad = labelW > lw ? std::string(labelW - lw, ' ') : "";
    std::string key = pad + col::bold() + col::fg(p.b.r, p.b.g, p.b.b) +
                      r.label + col::reset();
    std::string value = col::fg(230, 230, 235) + r.value + col::reset();
    return key + " " + col::dim() + "│" + col::reset() + " " + value;
}

static std::string sectionLine(const std::string& name, size_t width,
                               size_t index, const Palette& p) {
    std::string upper = name;
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](unsigned char c) { return (char)std::toupper(c); });

    RGB accent = lerp(p.a, p.b, 0.22 + 0.14 * (double)(index % 4));
    std::string out = col::fg(accent.r, accent.g, accent.b) + "▐" + col::reset() + " ";
    out += paintText(upper, accent, p.b, true);

    size_t used = 2 + visLen(upper) + 1;
    if (width > used + 2) {
        out += " " + col::dim() + col::fg(accent.r, accent.g, accent.b);
        for (size_t i = 0; i < width - used; i++) out += "─";
        out += col::reset();
    }
    return out;
}

static size_t terminalWidth() {
    struct winsize w{};
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 0)
        return w.ws_col;
    long cols = toLong(env("COLUMNS"));
    if (cols > 20) return (size_t)cols;
    return ::isatty(STDOUT_FILENO) ? 80 : 120;
}

static void render(const std::string& user, const std::string& host,
                   const std::vector<Row>& rows, const Palette& p,
                   bool showLogo) {
    const size_t gap = 3;
    size_t termW = terminalWidth();

    /* Drop section headers that ended up with no visible rows. */
    std::vector<Row> body;
    for (size_t i = 0; i < rows.size(); i++) {
        if (rows[i].section &&
            (i + 1 >= rows.size() || rows[i + 1].section)) continue;
        body.push_back(rows[i]);
    }

    size_t labelW = 0;
    for (const auto& r : body)
        if (!r.section) labelW = std::max(labelW, visLen(r.label));

    std::string titleText = user + "@" + host;
    std::string titleLine = paintText(titleText, p.a, p.b, true);

    std::string rule;
    for (size_t i = 0; i < visLen(titleText); i++) rule += "─";
    std::string ruleLine =
        col::dim() + col::fg(p.a.r, p.a.g, p.a.b) + rule + col::reset();

    std::vector<std::string> logo = fishLogo();
    size_t logoW = 0;
    for (const auto& l : logo) logoW = std::max(logoW, visLen(l));

    /* Natural width of the info column before any wrapping. */
    size_t infoW = std::max(visLen(titleLine), visLen(ruleLine));
    std::vector<std::string> bodyLines(body.size());
    for (size_t i = 0; i < body.size(); i++) {
        if (body[i].section) continue;
        bodyLines[i] = infoLine(body[i], labelW, p);
        infoW = std::max(infoW, visLen(bodyLines[i]));
    }

    size_t headerBase = 0;
    for (const auto& r : body)
        if (r.section) headerBase = std::max(headerBase, 2 + visLen(r.label) + 1);
    infoW = std::max(infoW, std::max(headerBase, (size_t)44));

    /* Decide where the logo goes and how much room the info column gets. */
    size_t room = termW > logoW + gap ? termW - logoW - gap : 0;
    bool side = showLogo;
    size_t maxContent;

    if (showLogo) {
        side = (infoW <= room) || (room >= 46);
        maxContent = side ? room : (termW > 4 ? termW - 4 : termW);
    } else {
        maxContent = termW > 4 ? termW - 4 : termW;
    }

    size_t contentW = std::min(infoW, maxContent);
    if (contentW < infoW) {
        titleLine = truncateVisible(titleLine, contentW);
        ruleLine  = truncateVisible(ruleLine, contentW);
        for (auto& l : bodyLines)
            if (!l.empty()) l = truncateVisible(l, contentW);
    }

    /* Section rules stay compact instead of stretching across a long row. */
    size_t sectionW = std::min(contentW, std::max(headerBase, (size_t)56));

    size_t finalW = std::max({visLen(titleLine), visLen(ruleLine), sectionW});
    for (size_t i = 0; i < body.size(); i++)
        if (!body[i].section) finalW = std::max(finalW, visLen(bodyLines[i]));

    std::vector<std::string> info;
    info.push_back(titleLine);
    info.push_back(ruleLine);

    size_t sectionIndex = 0;
    for (size_t i = 0; i < body.size(); i++) {
        if (body[i].section) {
            if (sectionIndex > 0) info.push_back("");
            info.push_back(sectionLine(body[i].label, sectionW, sectionIndex++, p));
        } else {
            info.push_back(bodyLines[i]);
        }
    }

    if (showLogo && !side) {
        for (size_t i = 0; i < logo.size(); i++)
            std::cout << paintLogoLine(logo[i], i, logo.size(), logoW, p) << "\n";
        std::cout << "\n";
    }

    size_t lines = side ? std::max(logo.size(), info.size()) : info.size();
    for (size_t i = 0; i < lines; i++) {
        bool blankInfo = i < info.size() && info[i].empty();
        if (side && !blankInfo) {
            if (i < logo.size()) {
                std::cout << paintLogoLine(logo[i], i, logo.size(), logoW, p);
                std::cout << std::string(logoW - visLen(logo[i]) + gap, ' ');
            } else {
                std::cout << std::string(logoW + gap, ' ');
            }
        }
        if (i < info.size()) std::cout << info[i];
        std::cout << "\n";
    }
}

static void printHelp() {
    std::cout <<
        "VL fetch 1.0.0 — fastfetch-style system info for Linux (fish edition)\n"
        "\n"
        "Usage: vlfetch [options]\n"
        "\n"
        "  -h, --help       show this help and exit\n"
        "  -v, --version    print version and exit\n"
        "      --no-logo    don't draw the fish\n"
        "      --no-color   disable ANSI colours\n"
        "      --color      force colours even if NO_COLOR is set\n"
        "      --logo-only  print only the fish\n"
        "\n"
        "Everything is read from /proc, /sys and standard utilities, so it runs\n"
        "on any Linux distribution. Optional helpers (lspci, xrandr, gsettings,\n"
        "xfconf-query, flatpak, snap, ...) are used when available.\n";
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char** argv) {
    g_hasTimeout = have("timeout");

    bool showLogo = true;
    bool logoOnly = false;
    bool forceColor = false;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "-h" || a == "--help") { printHelp(); return 0; }
        if (a == "-v" || a == "--version") {
            std::cout << "VL fetch 1.0.0\n";
            return 0;
        }
        if (a == "--no-logo") showLogo = false;
        else if (a == "--no-color") col::enabled = false;
        else if (a == "--color") forceColor = true;
        else if (a == "--logo-only") logoOnly = true;
        else {
            std::cerr << "vlfetch: unknown option '" << a << "'\n"
                      << "Try 'vlfetch --help'.\n";
            return 2;
        }
    }

    if (!forceColor && std::getenv("NO_COLOR") != nullptr) col::enabled = false;

    OsInfo os = detectOs();
    Palette palette = paletteFor(os.id);

    if (logoOnly) {
        std::vector<std::string> logo = fishLogo();
        size_t logoW = 0;
        for (const auto& l : logo) logoW = std::max(logoW, visLen(l));
        for (size_t i = 0; i < logo.size(); i++)
            std::cout << paintLogoLine(logo[i], i, logo.size(), logoW, palette) << "\n";
        return 0;
    }

    struct utsname u{};
    if (::uname(&u) != 0) {
        std::memset(&u, 0, sizeof u);
        std::strcpy(u.sysname, "Linux");
        std::strcpy(u.release, "unknown");
        std::strcpy(u.machine, "unknown");
        std::strcpy(u.nodename, "localhost");
    }

    CpuInfo cpu = collectCpu();
    auto mem = meminfo();
    NetInfo net = collectNetwork();

    std::string de = detectDe();
    std::string wm = detectWm(de);
    std::string term = detectTerminal();
    ThemeInfo themes = collectThemes(de);

    std::vector<Row> rows;
    auto section = [&](const std::string& name) {
        rows.push_back({name, "", true});
    };
    auto add = [&](std::string label, std::string value) {
        if (!trim(value).empty())
            rows.push_back({std::move(label), std::move(value), false});
    };

    std::string osValue = os.pretty;
    if (u.machine[0]) osValue += std::string(" ") + u.machine;

    section("OS");
    add("Distro", osValue);
    add("Kernel", u.release);
    add("Boot", bootTimeString());
    add("Uptime", uptimeString());

    section("DEVICE COMPONENTS");
    add("Host", hostName());
    add("BIOS", biosString());
    add("CPU", cpuString(cpu));
    add("GPU", gpuString());
    add("Memory", memoryString(mem));
    add("Battery", batteryString());

    section("STORAGE");
    add("Disk (/)", diskString());
    add("Swap", swapString(mem));

    section("NETWORK");
    add("Local IP", net.ip);
    add("Connection", net.connection);
    add("Gateway", net.gateway);
    add("DNS", net.dns);

    section("DESKTOP");
    add("Display", displayInfo());
    add("Session", sessionString());
    add("DE", de);
    add("WM", wm);
    add("WM Theme", themes.wmTheme);
    add("Theme", themes.theme);
    add("Icons", themes.icons);
    add("Cursor", themes.cursor);
    add("Font", themes.font);

    section("SOFTWARE");
    add("Packages", packagesString());
    add("Shell", shellInfo());
    add("Terminal", term);
    add("Terminal Font", terminalFont(term));

    section("SYSTEM");
    add("Locale", localeString());
    add("Timezone", timezoneString());
    add("Init", initString());

    long procs = processCount();
    if (procs > 0) add("Processes", std::to_string(procs));
    add("Load", loadAverage());
    add("Users", usersString());

    if (col::enabled) {
        section("COLORS");
        add("Palette", colorBlocks());
    }

    std::string user = userName();
    std::string host = u.nodename;
    if (host.empty()) host = "localhost";

    render(user, host, rows, palette, showLogo);
    return 0;
}
