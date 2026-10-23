#include "config.h"

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace hv {

static std::string trim(const std::string &s) {
    const auto b = s.find_first_not_of(" \t\r");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r");
    return s.substr(b, e - b + 1);
}

std::string expandEnv(const std::string &s) {
    std::string out;
    for (std::size_t i = 0; i < s.size();) {
        if (s[i] == '$' && i + 1 < s.size() && s[i + 1] == '{') {
            const auto end = s.find('}', i + 2);
            if (end != std::string::npos) {
                const char *v = std::getenv(s.substr(i + 2, end - i - 2).c_str());
                out += v ? v : "";
                i = end + 1;
                continue;
            }
        }
        out += s[i++];
    }
    return out;
}

bool parseCpuList(const std::string &s, std::vector<int> &out) {
    out.clear();
    std::stringstream ss(s);
    std::string part;
    while (std::getline(ss, part, ',')) {
        part = trim(part);
        if (part.empty()) continue;
        const auto dash = part.find('-');
        char *end = nullptr;
        const long a = std::strtol(part.c_str(), &end, 10);
        if (end == part.c_str() || a < 0 || a > 1023) return false;
        long b = a;
        if (dash != std::string::npos) {
            const std::string hi = part.substr(dash + 1);
            b = std::strtol(hi.c_str(), &end, 10);
            if (end == hi.c_str() || b < a || b > 1023) return false;
        }
        for (long c = a; c <= b; ++c) out.push_back(int(c));
    }
    return true;
}

bool loadConfig(const std::string &path, std::vector<PartitionConfig> &out, std::string &error) {
    std::ifstream in(path);
    if (!in) {
        error = "cannot open " + path;
        return false;
    }
    out.clear();
    std::string line;
    int lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        const auto hash = line.find(" #");  // inline comment
        if (hash != std::string::npos) line.erase(hash);
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line.front() == '[' && line.back() == ']') {
            PartitionConfig p;
            p.name = trim(line.substr(1, line.size() - 2));
            if (p.name.empty() || p.name.size() > 15) {
                error = "line " + std::to_string(lineNo) + ": partition name must be 1..15 chars";
                return false;
            }
            out.push_back(p);
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string::npos || out.empty()) {
            error = "line " + std::to_string(lineNo) + ": expected key = value inside a [partition]";
            return false;
        }
        const std::string key = trim(line.substr(0, eq));
        const std::string val = expandEnv(trim(line.substr(eq + 1)));
        auto &p = out.back();
        try {
            if (key == "criticality") p.criticality = val.substr(0, 15);
            else if (key == "cpus") {
                if (!parseCpuList(val, p.cpus)) throw std::invalid_argument("bad cpu list");
            } else if (key == "nice") p.nice = std::stoi(val);
            else if (key == "memory_mb") p.memoryMb = std::stol(val);
            else if (key == "watchdog_ms") p.watchdogMs = std::stoi(val);
            else if (key == "boot_grace_ms") p.bootGraceMs = std::stoi(val);
            else if (key == "restart") p.restart = val == "always";
            else if (key == "heartbeat") {
                p.heartbeat = val == "cluster" ? ivshm::RegionKind::Cluster
                            : val == "ivi"     ? ivshm::RegionKind::Ivi
                                               : ivshm::RegionKind::Hypervisor;
            } else if (key == "exec") {
                std::stringstream ss(val);
                std::string arg;
                while (ss >> arg) p.argv.push_back(arg);
            } else {
                throw std::invalid_argument("unknown key '" + key + "'");
            }
        } catch (const std::exception &e) {
            error = "line " + std::to_string(lineNo) + ": " + e.what();
            return false;
        }
    }
    for (const auto &p : out)
        if (p.argv.empty()) {
            error = "partition [" + p.name + "] has no exec";
            return false;
        }
    if (out.empty() || out.size() > std::size_t(ivshm::kMaxPartitions)) {
        error = "need 1.." + std::to_string(ivshm::kMaxPartitions) + " partitions";
        return false;
    }
    return true;
}

} // namespace hv
