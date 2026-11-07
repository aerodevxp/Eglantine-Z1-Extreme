// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"

namespace Common {

/// How much the host can be treated as a handheld.
enum class HostClass {
    Desktop,     ///< Discrete GPU, unrecognised part, or the profile was turned off
    ApuHandheld, ///< Handheld APU: one 16 GB-class shared pool, iGPU (Z1 Extreme and siblings)
};

struct HostProfileInfo {
    HostClass host_class{HostClass::Desktop};
    /// The brand string matched a known handheld part.
    bool apu_handheld{};
    u32 physical_cores{};
    u32 logical_cores{};
    u64 total_memory{};
    /// The class came from EGLANTINE_DEVICE_PROFILE, not from detection.
    bool forced{};
    /// The CPUID brand string, for logs.
    char brand[49]{};
};

/// The host this process runs on. Detected once, on first call.
///
/// EGLANTINE_DEVICE_PROFILE overrides the detection: "z1e", "z1", "7840u", "8840u" or "apu" force
/// the handheld class, "off" or "desktop" force the desktop one. Unset or unrecognised detects.
const HostProfileInfo& GetHostProfile();

/// True for the handheld APUs the placement and the presets apply to: the Ryzen Z1 Extreme and
/// the silicon siblings sharing its package and its gfx1103 iGPU.
bool IsHandheldApu();

} // namespace Common
