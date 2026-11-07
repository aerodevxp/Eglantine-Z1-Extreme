// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "common/host_profile.h"
#include "common/logging.h"
#include "common/thread_placement_linux.h"

namespace Common::Placement {

namespace {

constexpr std::size_t NoCore = ~std::size_t{0};
constexpr std::chrono::milliseconds PollInterval{500};

pid_t CurrentTid() {
    return static_cast<pid_t>(::syscall(SYS_gettid));
}

struct Entry {
    pid_t tid;
    Group group;
    std::size_t core;
};

struct State {
    std::mutex mutex;
    bool initialized{};
    bool usable{};
    cpu_set_t allowed{};
    std::vector<cpu_set_t> cores; // one CPU (the lowest of the pair) per physical core
    cpu_set_t primaries{};
    cpu_set_t efficiency{};
    std::vector<Entry> entries;
    pid_t canary_tid{};
    cpu_set_t canary_mask{};
    bool canary_valid{};
    std::atomic<s64> next_poll_ns{};
};

State& Get() {
    static State* const state = new State();
    return *state;
}

/// Parses "0,8" or "0-3,8-11" from topology/thread_siblings_list.
bool ParseCpuList(const std::string& text, cpu_set_t& out) {
    CPU_ZERO(&out);
    const char* cursor = text.c_str();
    while (*cursor != '\0' && *cursor != '\n') {
        char* end = nullptr;
        const long first = std::strtol(cursor, &end, 10);
        if (end == cursor) {
            return false;
        }
        long last = first;
        cursor = end;
        if (*cursor == '-') {
            ++cursor;
            last = std::strtol(cursor, &end, 10);
            if (end == cursor) {
                return false;
            }
            cursor = end;
        }
        for (long cpu = first; cpu <= last && cpu < CPU_SETSIZE; ++cpu) {
            if (cpu >= 0) {
                CPU_SET(static_cast<int>(cpu), &out);
            }
        }
        if (*cursor == ',') {
            ++cursor;
        }
    }
    return true;
}

bool ReadSiblings(int cpu, cpu_set_t& out) {
    std::ifstream file("/sys/devices/system/cpu/cpu" + std::to_string(cpu) +
                       "/topology/thread_siblings_list");
    std::string list;
    if (!file || !std::getline(file, list)) {
        return false;
    }
    return ParseCpuList(list, out);
}

/// Splits the CPUs this process may run on into physical cores: the lowest CPU of each SMT pair
/// becomes the core's primary, the rest go to the efficiency set.
bool BuildLocked(State& state) {
    state.initialized = true;
    CPU_ZERO(&state.allowed);
    if (sched_getaffinity(0, sizeof(state.allowed), &state.allowed) != 0) {
        LOG_WARNING(Common, "Could not query CPU affinity, thread placement is off");
        return false;
    }
    const long configured = sysconf(_SC_NPROCESSORS_CONF);
    if (configured <= 0) {
        return false;
    }
    const int total = static_cast<int>(std::min<long>(configured, CPU_SETSIZE));

    std::vector<cpu_set_t> sibling_sets;
    std::vector<std::vector<int>> members;
    for (int cpu = 0; cpu < total; ++cpu) {
        if (!CPU_ISSET(cpu, &state.allowed)) {
            continue;
        }
        cpu_set_t siblings;
        if (!ReadSiblings(cpu, siblings)) {
            CPU_ZERO(&siblings);
            CPU_SET(cpu, &siblings);
        }
        const auto it = std::find_if(sibling_sets.begin(), sibling_sets.end(),
                                     [&siblings](const cpu_set_t& set) {
                                         return std::memcmp(&set, &siblings, sizeof(set)) == 0;
                                     });
        if (it == sibling_sets.end()) {
            sibling_sets.push_back(siblings);
            members.emplace_back();
            members.back().push_back(cpu);
        } else {
            members[static_cast<std::size_t>(it - sibling_sets.begin())].push_back(cpu);
        }
    }

    CPU_ZERO(&state.efficiency);
    CPU_ZERO(&state.primaries);
    state.cores.clear();
    bool has_smt = false;
    for (auto& cpus : members) {
        if (cpus.empty()) {
            continue;
        }
        std::ranges::sort(cpus);
        cpu_set_t primary;
        CPU_ZERO(&primary);
        CPU_SET(cpus.front(), &primary);
        state.cores.push_back(primary);
        CPU_SET(cpus.front(), &state.primaries);
        for (std::size_t i = 1; i < cpus.size(); ++i) {
            CPU_SET(cpus[i], &state.efficiency);
            has_smt = true;
        }
    }

    if (!has_smt) {
        LOG_INFO(Common, "No SMT pairs among {} allowed CPUs, thread placement is off", total);
        return false;
    }
    state.usable = state.cores.size() >= 2;
    LOG_INFO(Common, "CPU placement: {} physical cores, {} sibling CPUs kept for background work",
             state.cores.size(), CPU_COUNT(&state.efficiency));
    return state.usable;
}

bool ApplyLocked(State& state, pid_t tid, Group group, std::size_t core, bool* gone) {
    const cpu_set_t* mask = &state.allowed;
    if (group == Group::PhysicalCore) {
        if (core == NoCore) {
            mask = &state.primaries;
        } else if (core < state.cores.size()) {
            mask = &state.cores[core];
        } else {
            return false;
        }
    } else if (group == Group::Efficiency) {
        if (CPU_COUNT(&state.efficiency) == 0) {
            return false;
        }
        mask = &state.efficiency;
    }
    if (CPU_COUNT(mask) == 0) {
        return false;
    }
    if (sched_setaffinity(tid, sizeof(*mask), mask) != 0) {
        if (gone != nullptr && errno == ESRCH) {
            *gone = true;
            return false;
        }
        LOG_DEBUG(Common, "Could not place thread {}: {}", tid, std::strerror(errno));
        return false;
    }
    return true;
}

Entry& RememberLocked(State& state, pid_t tid) {
    for (auto& entry : state.entries) {
        if (entry.tid == tid) {
            return entry;
        }
    }
    return state.entries.emplace_back(Entry{tid, Group::Unrestricted, NoCore});
}

void SnapshotCanaryLocked(State& state) {
    state.canary_valid = false;
    for (const auto& entry : state.entries) {
        if (entry.group == Group::Unrestricted) {
            continue;
        }
        cpu_set_t mask;
        CPU_ZERO(&mask);
        if (sched_getaffinity(entry.tid, sizeof(mask), &mask) != 0) {
            continue;
        }
        state.canary_tid = entry.tid;
        state.canary_mask = mask;
        state.canary_valid = true;
        return;
    }
}

} // Anonymous namespace

bool Available() {
    if (!IsHandheldApu()) {
        return false;
    }
    State& state = Get();
    std::scoped_lock lock{state.mutex};
    if (!state.initialized) {
        BuildLocked(state);
    }
    return state.usable;
}

void SetCore(std::size_t core) {
    if (!Available()) {
        return;
    }
    State& state = Get();
    const pid_t tid = CurrentTid();
    // The main thread is where the process's own mask is read from; leaving it free keeps that
    // valid, and it is not where the work is.
    if (tid == getpid()) {
        return;
    }
    std::scoped_lock lock{state.mutex};
    ApplyLocked(state, tid, Group::PhysicalCore, core, nullptr);
    Entry& entry = RememberLocked(state, tid);
    entry.group = Group::PhysicalCore;
    entry.core = core;
    if (!state.canary_valid) {
        SnapshotCanaryLocked(state);
    }
}

void SetGroup(Group group) {
    if (!Available()) {
        return;
    }
    State& state = Get();
    const pid_t tid = CurrentTid();
    if (tid == getpid() && group != Group::Unrestricted) {
        return;
    }
    std::scoped_lock lock{state.mutex};
    ApplyLocked(state, tid, group, NoCore, nullptr);
    Entry& entry = RememberLocked(state, tid);
    entry.group = group;
    entry.core = NoCore;
    if (!state.canary_valid) {
        SnapshotCanaryLocked(state);
    }
}

void Refresh() {
    if (!IsHandheldApu()) {
        return;
    }
    State& state = Get();
    std::scoped_lock lock{state.mutex};
    if (!state.initialized || !state.usable) {
        return;
    }
    // The process's own mask can change under us (a launcher, a cgroup scope); re-read it first.
    cpu_set_t current;
    CPU_ZERO(&current);
    if (sched_getaffinity(0, sizeof(current), &current) == 0 &&
        std::memcmp(&current, &state.allowed, sizeof(current)) != 0) {
        state.initialized = false;
        if (!BuildLocked(state)) {
            return;
        }
    }
    std::erase_if(state.entries, [&state](const Entry& entry) {
        bool gone = false;
        ApplyLocked(state, entry.tid, entry.group, entry.core, &gone);
        return gone;
    });
    SnapshotCanaryLocked(state);
}

void Poll() {
    if (!IsHandheldApu()) {
        return;
    }
    State& state = Get();
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const s64 now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    s64 next = state.next_poll_ns.load(std::memory_order_relaxed);
    if (now_ns < next) {
        return;
    }
    if (!state.next_poll_ns.compare_exchange_strong(next,
                                                    now_ns + PollInterval.count() * 1'000'000,
                                                    std::memory_order_relaxed)) {
        return;
    }
    pid_t tid;
    cpu_set_t expected;
    {
        std::scoped_lock lock{state.mutex};
        if (!state.canary_valid) {
            return;
        }
        tid = state.canary_tid;
        expected = state.canary_mask;
    }
    cpu_set_t current;
    CPU_ZERO(&current);
    if (sched_getaffinity(tid, sizeof(current), &current) == 0 &&
        std::memcmp(&current, &expected, sizeof(current)) == 0) {
        return;
    }
    Refresh();
}

} // namespace Common::Placement
