// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Alonso (GS·RUN). MSXFileForge.
// Free software under the GNU GPL v3 (or later) - see LICENSE. Modified
// versions must be distributed under the same license.
// MSXFileForge - MSX cassette conversion: .cas <-> .wav (audio) and .tsx (TZX).
//
// The .cas format is the raw MSX byte stream delimited by the 8-byte sync
// marker. WAV is Kansas-City-Standard FSK audio (cas2wav-compatible: 8-bit mono
// 43200 Hz; "0" bit = one 1200 Hz cycle, "1" bit = two 2400 Hz cycles). TSX is
// the TZX family with the MSX "KCS" block ID 0x4B.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace msx {

// Encode a .cas image to a playable WAV (8-bit mono PCM, 43200 Hz).
std::vector<uint8_t> casToWav(const std::vector<uint8_t>& cas, int baud = 1200);

// Decode a mono PCM WAV (8- or 16-bit) back to .cas. Best-effort: reliable on
// clean signals (e.g. produced by casToWav); real noisy tape rips may not decode.
std::vector<uint8_t> wavToCas(const std::vector<uint8_t>& wav, std::string* err = nullptr);

// Encode a .cas image to a TSX (TZX + MSX 0x4B KCS blocks).
std::vector<uint8_t> casToTsx(const std::vector<uint8_t>& cas, int baud = 1200);

// Decode a TSX/TZX back to .cas (extracts the data of every 0x4B block; skips
// metadata blocks). err set on an unparseable/unknown block.
std::vector<uint8_t> tsxToCas(const std::vector<uint8_t>& tsx, std::string* err = nullptr);

// Quick sniff helpers.
bool isTsx(const std::vector<uint8_t>& d);   // "ZXTape!" magic
bool isWav(const std::vector<uint8_t>& d);   // "RIFF"...."WAVE"

} // namespace msx
