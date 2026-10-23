// Partition table for sdv-hypervisor (INI style, see cockpit/partitions.conf).
#pragma once

#include <string>
#include <vector>

#include "ivshm/ivshm.h"

namespace hv {

struct PartitionConfig {
    std::string name;
    std::string criticality = "QM";
    std::vector<int> cpus;          // empty = any CPU
    int nice = 0;
    long memoryMb = 0;              // 0 = unlimited (RLIMIT_AS)
    int watchdogMs = 1000;
    int bootGraceMs = 5000;
    bool restart = true;
    ivshm::RegionKind heartbeat = ivshm::RegionKind::Hypervisor;  // Hypervisor = none
    std::vector<std::string> argv;
};

// Parses the file; ${VAR} in values is expanded from the environment.
bool loadConfig(const std::string &path, std::vector<PartitionConfig> &out, std::string &error);
// "0", "0-3", "0,2,5-6"
bool parseCpuList(const std::string &s, std::vector<int> &out);
std::string expandEnv(const std::string &s);

} // namespace hv
