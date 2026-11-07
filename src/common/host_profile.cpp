// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <sched.h>
#include <unistd.h>

#if defined(ARCHITECTURE_x86_64)
#include <cpuid.h>
#endif

#include "common/host_profile.h"
#include "common/logging.h"
#include "common/memory_detect.h"

namespace Common {

namespace {

// A handheld APU shares one 16 GB-class pool with its iGPU. Above this much RAM the same part sits
// in a laptop that has memory to spare, and the tight-budget tuning stops paying.
constexpr u64 MaxHandheldMemory = u64{24} << 30;
// The Z1 Extreme is 8C/16T and the Z1 6C/12T; narrower than this is not one of these parts.
constexpr u32 MinHandheldCores = 6;

// Brand strings of the Z1 Extreme and the parts built from the same gfx1103 silicon, matched
// case-insensitively against the CPUID brand string.
constexpr const char* HandheldBrands[] = {
    "Z1 EXTREME", "Z1", "7840U", "7840HS", "8840U", "8840HS",
};

void ReadBrandString(char (&brand)[49]) {
#if defined(ARCHITECTURE_x86_64)
    unsigned int max_leaf = 0;
    unsigned int regs[4]{};
    __cpuid(0x80000000, max_leaf, regs[0], regs[1], regs[2]);
    if (max_leaf < 0x80000004) {
        return;
    }
    for (unsigned int leaf = 0; leaf < 3; ++leaf) {
        __cpuid(0x80000002 + leaf, regs[0], regs[1], regs[2], regs[3]);
        std::memcpy(&brand[leaf * 16], regs, sizeof(regs));
    }
#endif
}

bool IsAmd() {
#if defined(ARCHITECTURE_x86_64)
    unsigned int regs[4]{};
    __cpuid(0, regs[0], regs[1], regs[2], regs[3]);
    return std::memcmp(&regs[1], "Auth", 4) == 0 && std::memcmp(&regs[2], "enti", 4) == 0 &&
           std::memcmp(&regs[3], "cAMD", 4) == 0;
#else
    return false;
#endif
}

std::string Upper(std::string_view text) {
    std::string out{text};
    std::ranges::transform(out, out.begin(),
                           [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return out;
}

/// Unique (package, core) pairs among the CPUs this process may run on: the physical core count,
/// as opposed to what hardware_concurrency() reports with SMT folded in.
u32 CountPhysicalCores() {
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0) {
        return std::max<u32>(1, std::thread::hardware_concurrency() / 2);
    }
    std::vector<std::pair<int, int>> cores;
    const long configured = sysconf(_SC_NPROCESSORS_CONF);
    for (long cpu = 0; cpu < configured && cpu < CPU_SETSIZE; ++cpu) {
        if (!CPU_ISSET(static_cast<int>(cpu), &allowed)) {
            continue;
        }
        const auto read = [cpu](const char* node) {
            std::ifstream file("/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/" + node);
            int value = -1;
            if (file) {
                file >> value;
            }
            return value;
        };
        const std::pair<int, int> key{read("topology/physical_package_id"),
                                      read("topology/core_id")};
        if (key.first < 0 || key.second < 0) {
            continue;
        }
        if (std::ranges::find(cores, key) == cores.end()) {
            cores.push_back(key);
        }
    }
    if (cores.empty()) {
        return std::max<u32>(1, std::thread::hardware_concurrency() / 2);
    }
    return static_cast<u32>(cores.size());
}

HostProfileInfo Detect() {
    HostProfileInfo info;
    ReadBrandString(info.brand);

    info.logical_cores = std::max<u32>(1, std::thread::hardware_concurrency());
    info.total_memory = GetMemInfo().TotalPhysicalMemory;

    const std::string brand = Upper(info.brand);
    const bool known_brand =
        IsAmd() && std::ranges::any_of(HandheldBrands, [&brand](const char* entry) {
            return brand.find(entry) != std::string::npos;
        });
    if (!known_brand) {
        return info;
    }

    info.physical_cores = CountPhysicalCores();
    if (info.physical_cores < MinHandheldCores || info.total_memory > MaxHandheldMemory) {
        LOG_INFO(Common,
                 "Handheld APU \"{}\" seen, but {} cores / {} MiB is outside the handheld class",
                 info.brand, info.physical_cores, info.total_memory >> 20);
        return info;
    }

    info.apu_handheld = true;
    info.host_class = HostClass::ApuHandheld;
    return info;
}

HostProfileInfo Build() {
    HostProfileInfo info = Detect();

    if (const char* forced = std::getenv("EGLANTINE_DEVICE_PROFILE")) {
        const std::string value = Upper(forced);
        if (value == "OFF" || value == "DESKTOP") {
            info.host_class = HostClass::Desktop;
            info.apu_handheld = false;
            info.forced = true;
        } else if (value == "Z1E" || value == "Z1" || value == "7840U" || value == "8840U" ||
                   value == "APU") {
            info.host_class = HostClass::ApuHandheld;
            info.apu_handheld = true;
            info.forced = true;
            if (info.physical_cores == 0) {
                info.physical_cores = CountPhysicalCores();
            }
        }
    }

    if (info.host_class == HostClass::ApuHandheld) {
        LOG_INFO(Common,
                 "Host profile: handheld APU \"{}\"{} ({} cores / {} threads, {} MiB); "
                 "EGLANTINE_DEVICE_PROFILE=off turns this off",
                 info.brand[0] != '\0' ? info.brand : "unknown", info.forced ? " forced" : "",
                 info.physical_cores, info.logical_cores, info.total_memory >> 20);
    }
    return info;
}

} // Anonymous namespace

const HostProfileInfo& GetHostProfile() {
    static const HostProfileInfo profile = Build();
    return profile;
}

bool IsHandheldApu() {
    return GetHostProfile().host_class == HostClass::ApuHandheld;
}

} // namespace Common
