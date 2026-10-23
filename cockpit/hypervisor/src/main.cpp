// sdv-hypervisor: partition manager emulating a type-1 hypervisor's VM layer.
//
// What it models (and the real-world counterpart):
//   * shared-memory regions per guest, created by the host  (ivshmem / vdev shmem)
//   * CPU pinning per partition                              (vCPU -> pCPU affinity)
//   * scheduling priority per partition                      (VM scheduler weights)
//   * hard memory ceiling per partition                      (guest RAM size)
//   * heartbeat watchdog + restart with exponential backoff  (VM health monitor)
//
//   sdv-hypervisor --config cockpit/partitions.conf      run the cockpit
//   sdv-hypervisor --status                              partition table
//   sdv-hypervisor --inject ivi:hang                     fault injection
#include <sched.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>

#include "config.h"
#include "ivshm/ivshm.h"

using ivshm::PartitionState;
using ivshm::RegionKind;

namespace {

volatile std::sig_atomic_t g_stop = 0;
void onStop(int) { g_stop = 1; }

uint64_t nowMs() { return ivshm::monotonicNs() / 1000000ull; }

void logf(const char *fmt, ...) {
    const uint64_t t = nowMs();
    std::printf("[hypervisor] %6llu.%03llu ", (unsigned long long)(t / 1000 % 1000000),
                (unsigned long long)(t % 1000));
    va_list ap;
    va_start(ap, fmt);
    std::vprintf(fmt, ap);
    va_end(ap);
    std::printf("\n");
    std::fflush(stdout);
}

const char *stateName(PartitionState s) {
    switch (s) {
    case PartitionState::Stopped: return "Stopped";
    case PartitionState::Booting: return "Booting";
    case PartitionState::Running: return "Running";
    case PartitionState::Hung: return "Hung";
    case PartitionState::Crashed: return "Crashed";
    case PartitionState::Restarting: return "Restarting";
    }
    return "?";
}

std::string defaultShmDir() {
    const char *d = std::getenv("COCKPIT_SHM_DIR");
    return d ? d : "/dev/shm/sdv-cockpit";
}

class Partition {
public:
    Partition(hv::PartitionConfig cfg, ivshm::PartitionInfo *info, const ivshm::Heartbeat *hb, bool isolate)
        : cfg_(std::move(cfg)), info_(info), hb_(hb), isolate_(isolate) {
        std::snprintf(info_->name, sizeof info_->name, "%s", cfg_.name.c_str());
        std::snprintf(info_->criticality, sizeof info_->criticality, "%s", cfg_.criticality.c_str());
        setState(PartitionState::Stopped);
    }

    const std::string &name() const { return cfg_.name; }
    pid_t pid() const { return pid_; }

    void spawn() {
        const pid_t pid = ::fork();
        if (pid < 0) {
            logf("[%s] fork failed: %s", name().c_str(), std::strerror(errno));
            scheduleRestart();
            return;
        }
        if (pid == 0) execChild();  // never returns

        pid_ = pid;
        spawnMs_ = nowMs();
        lastBeatMs_ = 0;
        lastCounter_ = hb_ ? hb_->counter.load(std::memory_order_acquire) : 0;
        info_->pid.store(uint32_t(pid));
        info_->startedNs.store(ivshm::monotonicNs());
        setState(PartitionState::Booting);
        logf("[%s] started pid %d  (%s, cpus=%s, nice=%d, mem=%s)", name().c_str(), pid,
             cfg_.criticality.c_str(), cpuString().c_str(), isolate_ ? cfg_.nice : 0,
             (isolate_ && cfg_.memoryMb) ? (std::to_string(cfg_.memoryMb) + "MB").c_str() : "unlimited");
    }

    void tick() {
        const uint64_t now = nowMs();
        if (pid_ > 0) {
            int status = 0;
            const pid_t r = ::waitpid(pid_, &status, WNOHANG);
            if (r == pid_) {
                onExit(status);
                return;
            }
            checkHeartbeat(now);
        } else if (restartAtMs_ && now >= restartAtMs_) {
            restartAtMs_ = 0;
            info_->restarts.fetch_add(1);
            spawn();
        }
    }

    void signal(int sig) {
        if (pid_ > 0) ::kill(-pid_, sig);  // whole process group
    }

    void stop() {
        cfg_.restart = false;
        restartAtMs_ = 0;
        if (pid_ <= 0) return;
        ::kill(-pid_, SIGTERM);
        for (int i = 0; i < 40 && ::waitpid(pid_, nullptr, WNOHANG) == 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        ::kill(-pid_, SIGKILL);
        ::waitpid(pid_, nullptr, 0);
        pid_ = -1;
        setState(PartitionState::Stopped);
    }

private:
    [[noreturn]] void execChild() {
        ::setpgid(0, 0);
        std::signal(SIGINT, SIG_DFL);
        std::signal(SIGTERM, SIG_DFL);
        if (isolate_) {
            if (!cfg_.cpus.empty()) {
                cpu_set_t set;
                CPU_ZERO(&set);
                for (int c : cfg_.cpus) CPU_SET(c, &set);
                if (::sched_setaffinity(0, sizeof set, &set) != 0)
                    std::fprintf(stderr, "[%s] sched_setaffinity: %s\n", name().c_str(), std::strerror(errno));
            }
            if (cfg_.nice) ::setpriority(PRIO_PROCESS, 0, cfg_.nice);
            if (cfg_.memoryMb > 0) {
                rlimit rl{rlim_t(cfg_.memoryMb) << 20, rlim_t(cfg_.memoryMb) << 20};
                ::setrlimit(RLIMIT_AS, &rl);
            }
        }
        ::setenv("COCKPIT_PARTITION", name().c_str(), 1);
        std::vector<char *> argv;
        for (auto &a : cfg_.argv) argv.push_back(const_cast<char *>(a.c_str()));
        argv.push_back(nullptr);
        ::execvp(argv[0], argv.data());
        std::fprintf(stderr, "[%s] exec %s: %s\n", name().c_str(), argv[0], std::strerror(errno));
        ::_exit(127);
    }

    void checkHeartbeat(uint64_t now) {
        if (!hb_) {  // no heartbeat contract: alive while the process exists
            if (state() == PartitionState::Booting) markRunning(now);
            return;
        }
        const uint64_t counter = hb_->counter.load(std::memory_order_acquire);
        const bool mine = hb_->pid.load(std::memory_order_relaxed) == uint32_t(pid_);
        if (mine && counter != lastCounter_) {
            lastCounter_ = counter;
            lastBeatMs_ = now;
            if (state() == PartitionState::Booting &&
                hb_->state.load(std::memory_order_acquire) == uint32_t(ivshm::GuestState::Running))
                markRunning(now);
        }
        if (state() == PartitionState::Running && now - runningSinceMs_ > 30000) failures_ = 0;

        const bool neverBeat = lastBeatMs_ == 0 || state() == PartitionState::Booting;
        const uint64_t ref = neverBeat ? spawnMs_ : lastBeatMs_;
        const uint64_t limit = neverBeat ? uint64_t(cfg_.bootGraceMs) : uint64_t(cfg_.watchdogMs);
        if (now - ref > limit) {
            logf("[%s] WATCHDOG: no heartbeat for %llu ms -> partition hung, resetting", name().c_str(),
                 (unsigned long long)(now - ref));
            setState(PartitionState::Hung);
            hung_ = true;
            ::kill(-pid_, SIGKILL);
        }
    }

    void markRunning(uint64_t now) {
        const uint64_t ms = now - spawnMs_;
        info_->lastRecoveryMs.store(ms);
        runningSinceMs_ = now;
        setState(PartitionState::Running);
        logf("[%s] running (first healthy heartbeat %llu ms after start)", name().c_str(), (unsigned long long)ms);
    }

    void onExit(int status) {
        pid_ = -1;
        info_->pid.store(0);
        std::string why = WIFSIGNALED(status) ? std::string("signal ") + strsignal(WTERMSIG(status))
                                              : "exit code " + std::to_string(WEXITSTATUS(status));
        if (hung_) why = "killed by watchdog";
        hung_ = false;
        if (!cfg_.restart) {
            logf("[%s] stopped (%s)", name().c_str(), why.c_str());
            setState(PartitionState::Stopped);
            return;
        }
        logf("[%s] CRASHED (%s)", name().c_str(), why.c_str());
        setState(PartitionState::Crashed);
        scheduleRestart();
    }

    void scheduleRestart() {
        ++failures_;
        const uint64_t backoff = std::min<uint64_t>(5000, 200ull << std::min(failures_ - 1, 5));
        restartAtMs_ = nowMs() + backoff;
        setState(PartitionState::Restarting);
        logf("[%s] restarting in %llu ms (consecutive failures: %d)", name().c_str(),
             (unsigned long long)backoff, failures_);
    }

    std::string cpuString() const {
        if (!isolate_ || cfg_.cpus.empty()) return "any";
        std::string s;
        for (int c : cfg_.cpus) s += (s.empty() ? "" : ",") + std::to_string(c);
        return s;
    }

    PartitionState state() const { return PartitionState(info_->state.load()); }
    void setState(PartitionState s) { info_->state.store(uint32_t(s), std::memory_order_release); }

    hv::PartitionConfig cfg_;
    ivshm::PartitionInfo *info_;
    const ivshm::Heartbeat *hb_;
    bool isolate_;
    pid_t pid_ = -1;
    uint64_t spawnMs_ = 0, lastBeatMs_ = 0, lastCounter_ = 0, restartAtMs_ = 0, runningSinceMs_ = 0;
    int failures_ = 0;
    bool hung_ = false;
};

// ---- CLI helpers -------------------------------------------------------------------
int printStatus(const std::string &dir) {
    ivshm::Mapping m;
    std::string e;
    if (!m.open(ivshm::regionPath(dir, RegionKind::Hypervisor), sizeof(ivshm::HvRegion), false, &e) ||
        !ivshm::validHeader(m.as<ivshm::HvRegion>()->hdr, RegionKind::Hypervisor)) {
        std::fprintf(stderr, "hypervisor not running (%s)\n", e.c_str());
        return 1;
    }
    const auto *hvr = m.as<ivshm::HvRegion>();
    std::printf("%-10s %-8s %-11s %8s %9s %12s\n", "PARTITION", "ASIL", "STATE", "PID", "RESTARTS", "RECOVERY_MS");
    for (uint32_t i = 0; i < hvr->partitionCount.load() && i < uint32_t(ivshm::kMaxPartitions); ++i) {
        const auto &p = hvr->partitions[i];
        std::printf("%-10.15s %-8.15s %-11s %8u %9u %12llu\n", p.name, p.criticality,
                    stateName(PartitionState(p.state.load())), p.pid.load(), p.restarts.load(),
                    (unsigned long long)p.lastRecoveryMs.load());
    }
    return 0;
}

int inject(const std::string &dir, const std::string &spec) {
    const auto colon = spec.find(':');
    if (colon == std::string::npos) {
        std::fprintf(stderr, "usage: --inject <partition>:<hang|resume|kill|corrupt|cpu-storm|mem-bomb|flood>\n");
        return 2;
    }
    const std::string part = spec.substr(0, colon), fault = spec.substr(colon + 1);
    static const std::map<std::string, int> sigs = {
        {"hang", SIGSTOP}, {"resume", SIGCONT}, {"kill", SIGKILL}, {"corrupt", SIGUSR2},
        {"cpu-storm", SIGUSR1}, {"mem-bomb", SIGRTMIN}, {"flood", SIGRTMIN + 1}};
    const auto it = sigs.find(fault);
    if (it == sigs.end()) {
        std::fprintf(stderr, "unknown fault '%s'\n", fault.c_str());
        return 2;
    }
    ivshm::Mapping m;
    std::string e;
    if (!m.open(ivshm::regionPath(dir, RegionKind::Hypervisor), sizeof(ivshm::HvRegion), false, &e)) {
        std::fprintf(stderr, "hypervisor not running (%s)\n", e.c_str());
        return 1;
    }
    const auto *hvr = m.as<ivshm::HvRegion>();
    for (uint32_t i = 0; i < hvr->partitionCount.load() && i < uint32_t(ivshm::kMaxPartitions); ++i) {
        if (part == hvr->partitions[i].name) {
            const pid_t pid = pid_t(hvr->partitions[i].pid.load());
            if (pid <= 0 || ::kill(fault == "hang" || fault == "resume" || fault == "kill" ? -pid : pid, it->second) != 0) {
                std::fprintf(stderr, "partition %s not running\n", part.c_str());
                return 1;
            }
            std::printf("injected %s into %s (pid %d)\n", fault.c_str(), part.c_str(), pid);
            return 0;
        }
    }
    std::fprintf(stderr, "no partition named %s\n", part.c_str());
    return 1;
}

} // namespace

int main(int argc, char **argv) {
    std::string config = "cockpit/partitions.conf", dir = defaultShmDir();
    bool isolate = true;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--config") config = next();
        else if (a == "--shm-dir") dir = next();
        else if (a == "--no-isolation") isolate = false;
        else if (a == "--status") return printStatus(dir);
        else if (a == "--inject") return inject(dir, next());
        else {
            std::printf("usage: %s [--config file] [--shm-dir dir] [--no-isolation] | --status | --inject part:fault\n", argv[0]);
            return a == "--help" ? 0 : 2;
        }
    }

    std::vector<hv::PartitionConfig> parts;
    std::string error;
    if (!hv::loadConfig(config, parts, error)) {
        logf("config error: %s", error.c_str());
        return 1;
    }

    // Create shared-memory regions (the "ivshmem devices") before any guest boots.
    ::mkdir(dir.c_str(), 0755);
    ::setenv("COCKPIT_SHM_DIR", dir.c_str(), 1);
    ivshm::Mapping hvMap, clusterRO, iviRO;
    for (auto k : {RegionKind::Hypervisor, RegionKind::Cluster, RegionKind::Ivi}) {
        if (!ivshm::Mapping::create(ivshm::regionPath(dir, k), ivshm::regionSize(k), &error)) {
            logf("cannot create region: %s", error.c_str());
            return 1;
        }
    }
    if (!hvMap.open(ivshm::regionPath(dir, RegionKind::Hypervisor), sizeof(ivshm::HvRegion), true, &error) ||
        !clusterRO.open(ivshm::regionPath(dir, RegionKind::Cluster), sizeof(ivshm::ClusterRegion), false, &error) ||
        !iviRO.open(ivshm::regionPath(dir, RegionKind::Ivi), sizeof(ivshm::IviRegion), false, &error)) {
        logf("cannot map region: %s", error.c_str());
        return 1;
    }
    auto *hvr = hvMap.as<ivshm::HvRegion>();
    logf("shared memory: %s  (hv %zu B, cluster %zu B, ivi %zu B)  isolation: %s", dir.c_str(),
         sizeof(ivshm::HvRegion), sizeof(ivshm::ClusterRegion), sizeof(ivshm::IviRegion),
         isolate ? "ON" : "OFF (comparison mode)");

    std::vector<std::unique_ptr<Partition>> partitions;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const ivshm::Heartbeat *hb = nullptr;
        if (parts[i].heartbeat == RegionKind::Cluster) hb = &clusterRO.as<ivshm::ClusterRegion>()->hb;
        if (parts[i].heartbeat == RegionKind::Ivi) hb = &iviRO.as<ivshm::IviRegion>()->hb;
        partitions.push_back(std::make_unique<Partition>(parts[i], &hvr->partitions[i], hb, isolate));
    }
    hvr->partitionCount.store(uint32_t(partitions.size()));
    ivshm::initHeader(hvr->hdr, RegionKind::Hypervisor, sizeof(ivshm::HvRegion));

    std::signal(SIGINT, onStop);
    std::signal(SIGTERM, onStop);
    for (auto &p : partitions) p->spawn();  // boot order = file order (safety partition first)

    while (!g_stop) {
        for (auto &p : partitions) p->tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    logf("shutting down partitions");
    for (auto it = partitions.rbegin(); it != partitions.rend(); ++it) (*it)->stop();
    return 0;
}
