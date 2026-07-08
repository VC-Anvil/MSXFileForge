// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Alonso (GS·RUN). MSXFileForge.
// Free software under the GNU GPL v3 (or later) - see LICENSE. Modified
// versions must be distributed under the same license.
// MSXFileForge - MSX file type detection + BASIC detokenizer.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace msx {

enum class MsxFileType {
    Unknown,
    BasicTokenized,   // starts with 0xFF  -> tokenized MSX-BASIC
    BasicAscii,       // plain-text BASIC (SAVE ,A)
    BloadBinary,      // starts with 0xFE  -> BSAVE/BLOAD image
    Text,             // printable text
    Data              // binary blob
};

const char* typeName(MsxFileType t);

struct BloadHeader { uint16_t start = 0, end = 0, exec = 0; bool valid = false; };

// Detect the logical type from the first bytes / content statistics.
MsxFileType detectType(const std::vector<uint8_t>& d);

// Parse a 7-byte BLOAD/BSAVE header (0xFE, start, end, exec). valid=false if not.
BloadHeader parseBloadHeader(const std::vector<uint8_t>& d);

// Detokenize a tokenized BASIC program (input should begin with 0xFF).
// Returns a plain-text listing. Best-effort on BCD floats.
std::string detokenizeBasic(const std::vector<uint8_t>& d);

// Tokenize a plain-text BASIC listing back into a tokenized program (output
// begins with 0xFF, MSX text area base 0x8001). Inverse of detokenizeBasic;
// designed so detokenizeBasic(tokenizeBasic(t)) reproduces t.
std::vector<uint8_t> tokenizeBasic(const std::string& text);

// ---- Block carving --------------------------------------------------------
// Split a raw byte blob (e.g. the RAW region of a loader disk) into recognizable
// segments: BLOAD images (0xFE header), tokenized BASIC programs (0xFF), and the
// unrecognized gaps between them. Headers are only accepted at sector (512)
// boundaries or immediately after a previous block, which keeps false positives
// low on real loader data.
enum class CarveType { Bload, BasicTokenized, Unknown };
const char* carveTypeName(CarveType t);
struct CarvedBlock {
    long offset = 0;
    long length = 0;
    CarveType type = CarveType::Unknown;
    uint16_t start = 0, end = 0, exec = 0; // for BLOAD
};
std::vector<CarvedBlock> carveBlocks(const std::vector<uint8_t>& d);

} // namespace msx
