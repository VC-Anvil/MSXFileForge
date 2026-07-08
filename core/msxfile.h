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

} // namespace msx
