// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>

namespace Common::Placement {

/// Which CPUs a thread may run on.
enum class Group {
    /// Every CPU the process inherited.
    Unrestricted,
    /// One specific physical core, given to guest cores so no two share an SMT pair. With no core
    /// given, the primary thread of every physical core.
    PhysicalCore,
    /// The SMT siblings of every physical core: background work that must not take a primary.
    Efficiency,
};

/// False unless the host is a handheld APU with SMT and its topology could be read.
bool Available();

/// Pins the calling thread to `core`, indexed into the physical cores by ascending CPU number.
void SetCore(std::size_t core);

/// Places the calling thread in `group`. Unrestricted restores the inherited mask.
void SetGroup(Group group);

/// Re-applies every placement. A launcher that re-restricts the process (a gamescope or systemd
/// scope restart, a compositor reconnect) drops the masks; this puts them back.
void Refresh();

/// Cheap: compares one placed thread's mask against what was set and refreshes when it differs.
/// Call it from a thread that runs often.
void Poll();

} // namespace Common::Placement
