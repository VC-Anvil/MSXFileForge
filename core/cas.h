// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Alonso (GS·RUN). MSXFileForge.
// Free software under the GNU GPL v3 (or later) - see LICENSE. Modified
// versions must be distributed under the same license.
// MSXFileForge - MSX cassette (.cas) block/file inspection.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace msx {

enum class CasBlockKind { Header, Ascii, Binary, BasicTokenized, Custom };

struct CasBlock {
    long offset = 0;             // offset of the 8-byte block header
    CasBlockKind kind = CasBlockKind::Custom;
    std::string filename;        // for the 3 standard file headers (6 chars)
    long dataOffset = 0;         // start of payload after the type/name preamble
    long dataLength = 0;         // best-effort payload length up to next header
};

// Parse a .cas image into its logical blocks.
std::vector<CasBlock> parseCas(const std::vector<uint8_t>& d);
const char* casKindName(CasBlockKind k);

} // namespace msx
