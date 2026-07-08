// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Alonso (GS·RUN). MSXFileForge.
// Free software under the GNU GPL v3 (or later) - see LICENSE. Modified
// versions must be distributed under the same license.
// MSXFileForge - small shared utilities.
#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace msx {

uint32_t crc32(const uint8_t* data, size_t n);
inline uint32_t crc32(const std::vector<uint8_t>& v) { return crc32(v.data(), v.size()); }

// Build a blank, formatted FAT12 MSX floppy image. totalSectors = 1440 (720KB)
// or 720 (360KB). Returns the raw image bytes (mountable, empty).
std::vector<uint8_t> formatBlankImage(int totalSectors);

} // namespace msx
