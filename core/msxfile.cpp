// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Alonso (GS·RUN). MSXFileForge.
// Free software under the GNU GPL v3 (or later) - see LICENSE. Modified
// versions must be distributed under the same license.
#include "msxfile.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <sstream>

namespace msx {

const char* typeName(MsxFileType t) {
    switch (t) {
        case MsxFileType::BasicTokenized: return "Tokenized BASIC";
        case MsxFileType::BasicAscii:     return "ASCII BASIC";
        case MsxFileType::BloadBinary:    return "BLOAD binary";
        case MsxFileType::Text:           return "Text";
        case MsxFileType::Data:           return "Binary data";
        default:                          return "Unknown";
    }
}

MsxFileType detectType(const std::vector<uint8_t>& d) {
    if (d.empty()) return MsxFileType::Unknown;
    if (d[0] == 0xFF) return MsxFileType::BasicTokenized;
    if (d[0] == 0xFE && d.size() >= 7) return MsxFileType::BloadBinary;
    // Statistics: mostly-printable -> text/ASCII BASIC.
    size_t sample = d.size() < 512 ? d.size() : 512;
    size_t printable = 0;
    for (size_t i = 0; i < sample; ++i) {
        uint8_t c = d[i];
        if (c == 9 || c == 10 || c == 13 || (c >= 0x20 && c < 0x7F)) printable++;
    }
    if (printable * 100 >= sample * 92) {
        // Heuristic: a line beginning with a digit + space suggests BASIC.
        bool looksBasic = (d[0] >= '0' && d[0] <= '9');
        return looksBasic ? MsxFileType::BasicAscii : MsxFileType::Text;
    }
    return MsxFileType::Data;
}

BloadHeader parseBloadHeader(const std::vector<uint8_t>& d) {
    BloadHeader h;
    if (d.size() >= 7 && d[0] == 0xFE) {
        h.start = d[1] | (d[2] << 8);
        h.end   = d[3] | (d[4] << 8);
        h.exec  = d[5] | (d[6] << 8);
        h.valid = h.end >= h.start;
    }
    return h;
}

// ---- BASIC token tables (verified against the MSX-BASIC ROM tokenizer) -----

static const char* kTokens[0x80] = { // index = token - 0x80
/*80*/ nullptr,
/*81*/ "END","FOR","NEXT","DATA","INPUT","DIM","READ","LET","GOTO","RUN","IF",
/*8C*/ "RESTORE","GOSUB","RETURN","REM","STOP","PRINT","CLEAR","LIST","NEW","ON",
/*96*/ "WAIT","DEF","POKE","CONT","CSAVE","CLOAD","OUT","LPRINT","LLIST","CLS",
/*A0*/ "WIDTH","ELSE","TRON","TROFF","SWAP","ERASE","ERROR","RESUME","DELETE","AUTO",
/*AA*/ "RENUM","DEFSTR","DEFINT","DEFSNG","DEFDBL","LINE","OPEN","FIELD","GET","PUT",
/*B4*/ "CLOSE","LOAD","MERGE","FILES","LSET","RSET","SAVE","LFILES","CIRCLE","COLOR",
/*BE*/ "DRAW","PAINT","BEEP","PLAY","PSET","PRESET","SOUND","SCREEN","VPOKE","SPRITE",
/*C8*/ "VDP","BASE","CALL","TIME","KEY","MAX","MOTOR","BLOAD","BSAVE","DSKO$",
/*D2*/ "SET","NAME","KILL","IPL","COPY","CMD","LOCATE","TO","THEN","TAB(",
/*DC*/ "STEP","USR","FN","SPC(","NOT","ERL","ERR","STRING$","USING","INSTR",
/*E6*/ "'","VARPTR","CSRLIN","ATTR$","DSKI$","OFF","INKEY$","POINT",">","=",
/*F0*/ "<","+","-","*","/","^","AND","OR","XOR","EQV",
/*FA*/ "IMP","MOD","\\", nullptr, nullptr, nullptr
};

static const char* kExtTokens[0x80] = { // index = second byte - 0x80
/*80*/ nullptr,
/*81*/ "LEFT$","RIGHT$","MID$","SGN","INT","ABS","SQR","RND","SIN","LOG",
/*8B*/ "EXP","COS","TAN","ATN","FRE","INP","POS","LEN","STR$","VAL",
/*95*/ "ASC","CHR$","PEEK","VPEEK","SPACE$","OCT$","HEX$","LPOS","BIN$","CINT",
/*9F*/ "CSNG","CDBL","FIX","STICK","STRIG","PDL","PAD","DSKF","FPOS","CVI",
/*A9*/ "CVS","CVD","EOF","LOC","LOF","MKI$","MKS$","MKD$"
};

static const char* tokenText(uint8_t b) {
    if (b >= 0x81 && b <= 0xFC) return kTokens[b - 0x80];
    return nullptr;
}
static const char* extTokenText(uint8_t b) {
    if (b >= 0x81 && b <= 0xB0) return kExtTokens[b - 0x80];
    return nullptr;
}

// ---- Numeric constants ----------------------------------------------------
//
// MSX-BASIC stores real constants as BCD: one exponent byte (bit 7 = sign,
// bits 0-6 = power of ten + 0x40, value = 0.d1d2d3... x 10^exp) followed by 3
// (single precision, 6 digits, token 0x1D) or 7 (double, 14 digits, token
// 0x1F) mantissa bytes, two digits per byte. Conversion goes through decimal
// digit strings, never binary floating point, so it is exact both ways.

namespace {

struct DecNum {           // value = 0.digits x 10^exp ("" == zero)
    std::string digits;   // no leading/trailing zeros
    int exp = 0;
    bool neg = false;
};

DecNum bcdToDec(const uint8_t* p, int mantBytes) {
    DecNum n;
    if (p[0] == 0) return n;
    n.neg = (p[0] & 0x80) != 0;
    n.exp = (p[0] & 0x7F) - 0x40;
    for (int i = 1; i <= mantBytes; ++i) {
        n.digits += (char)('0' + ((p[i] >> 4) % 10));
        n.digits += (char)('0' + ((p[i] & 0xF) % 10));
    }
    size_t lead = n.digits.find_first_not_of('0');
    if (lead == std::string::npos) return DecNum{};
    n.exp -= (int)lead;
    n.digits.erase(0, lead);
    while (!n.digits.empty() && n.digits.back() == '0') n.digits.pop_back();
    return n;
}

// Round to `maxDigits` significant digits (half up) and pack into p[0..mant].
void decToBcd(DecNum n, int mantBytes, uint8_t* p) {
    int maxDigits = mantBytes * 2;
    memset(p, 0, (size_t)mantBytes + 1);
    if (n.digits.empty()) return;
    if ((int)n.digits.size() > maxDigits) {
        bool up = n.digits[(size_t)maxDigits] >= '5';
        n.digits.resize((size_t)maxDigits);
        for (int i = maxDigits - 1; up && i >= 0; --i) {
            if (n.digits[(size_t)i] == '9') n.digits[(size_t)i] = '0';
            else { n.digits[(size_t)i]++; up = false; }
        }
        if (up) { n.digits.insert(n.digits.begin(), '1'); n.digits.pop_back(); n.exp++; }
    }
    if (n.exp > 63) { n.exp = 63; n.digits = std::string((size_t)maxDigits, '9'); } // overflow: clamp
    if (n.exp < -63) return;                                                     // underflow: zero
    n.digits.resize((size_t)maxDigits, '0');
    p[0] = (uint8_t)((n.exp + 0x40) | (n.neg ? 0x80 : 0));
    for (int i = 0; i < mantBytes; ++i)
        p[i + 1] = (uint8_t)(((n.digits[(size_t)i * 2] - '0') << 4) | (n.digits[(size_t)i * 2 + 1] - '0'));
}

// Format the way a listing should show it so that re-tokenizing gives the same
// bytes: no leading zero (".5"), single precision marked with '!' (or an 'E'
// exponent), double precision with a 'D' exponent, or '#' when it would
// otherwise read back as an integer.
std::string formatReal(const DecNum& n, bool dbl) {
    const int maxDigits = dbl ? 14 : 6;
    std::string s;
    if (n.neg) s += '-';
    if (n.digits.empty()) return s + (dbl ? "0#" : "0!");
    const std::string& d = n.digits;
    int len = (int)d.size();
    bool fixed = (n.exp >= 1 && n.exp <= maxDigits) ||
                 (n.exp <= 0 && -n.exp + len <= maxDigits && n.exp >= -(maxDigits - 1));
    if (fixed) {
        if (n.exp <= 0) {
            s += '.'; s.append((size_t)-n.exp, '0'); s += d;
        } else if (len <= n.exp) {
            s += d; s.append((size_t)(n.exp - len), '0');
        } else {
            s += d.substr(0, (size_t)n.exp); s += '.'; s += d.substr((size_t)n.exp);
        }
        bool hasPoint = s.find('.') != std::string::npos;
        if (!dbl) s += '!';
        else if (!hasPoint) {                       // would it read back as an integer?
            std::string mag = n.neg ? s.substr(1) : s;
            if (mag.size() < 5 || (mag.size() == 5 && mag <= "32767")) s += '#';
        }
        return s;
    }
    s += d[0];
    if (len > 1) { s += '.'; s += d.substr(1); }
    int e = n.exp - 1;
    char buf[16]; snprintf(buf, sizeof buf, "%c%c%02d", dbl ? 'D' : 'E', e < 0 ? '-' : '+', e < 0 ? -e : e);
    return s + buf;
}

} // namespace

std::string detokenizeBasic(const std::vector<uint8_t>& d) {
    std::string out;
    if (d.empty() || d[0] != 0xFF) return out; // not tokenized
    size_t pos = 1;
    while (pos + 4 <= d.size()) {
        uint16_t link = d[pos] | (d[pos + 1] << 8);
        if (link == 0) break; // end of program
        uint16_t lineNo = d[pos + 2] | (d[pos + 3] << 8);
        pos += 4;
        char num[16]; snprintf(num, sizeof num, "%u ", lineNo);
        out += num;

        bool inString = false, restLiteral = false, inData = false;
        while (pos < d.size()) {
            uint8_t b = d[pos];
            if (b == 0x00) { pos++; break; } // end of line
            if (restLiteral) { out += (char)b; pos++; continue; }
            if (inString) {
                out += (char)b;
                if (b == '"') inString = false;
                pos++;
                continue;
            }
            if (b == '"') { inString = true; out += '"'; pos++; continue; }
            if (inData) {                     // DATA items are stored verbatim
                if (b == ':') inData = false;
                out += (char)b; pos++; continue;
            }

            // ELSE is stored as ":" ELSE and ' as ":" REM "'"; LIST hides the colon.
            if (b == ':' && pos + 1 < d.size() && d[pos+1] == 0xA1) { pos++; continue; }
            if (b == ':' && pos + 2 < d.size() && d[pos+1] == 0x8F && d[pos+2] == 0xE6) {
                out += '\''; pos += 3; restLiteral = true; continue;
            }

            // Embedded numeric constants.
            if (b == 0x0B && pos + 2 < d.size()) { // octal
                int v = d[pos+1] | (d[pos+2] << 8);
                char t[16]; snprintf(t, sizeof t, "&O%o", v); out += t; pos += 3; continue;
            }
            if (b == 0x0C && pos + 2 < d.size()) { // hex
                int v = d[pos+1] | (d[pos+2] << 8);
                char t[16]; snprintf(t, sizeof t, "&H%X", v); out += t; pos += 3; continue;
            }
            if ((b == 0x0D || b == 0x0E) && pos + 2 < d.size()) { // line ref
                int v = d[pos+1] | (d[pos+2] << 8);
                char t[16]; snprintf(t, sizeof t, "%d", v); out += t; pos += 3; continue;
            }
            if (b == 0x0F && pos + 1 < d.size()) { // int 10..255
                out += std::to_string((int)d[pos+1]); pos += 2; continue;
            }
            if (b >= 0x11 && b <= 0x1A) { // single digit 0..9
                out += (char)('0' + (b - 0x11)); pos++; continue;
            }
            if (b == 0x1C && pos + 2 < d.size()) { // 2-byte signed int
                int16_t v = (int16_t)(d[pos+1] | (d[pos+2] << 8));
                out += std::to_string((int)v); pos += 3; continue;
            }
            if (b == 0x1D && pos + 4 < d.size()) { // single precision (BCD)
                out += formatReal(bcdToDec(&d[pos+1], 3), false); pos += 5; continue;
            }
            if (b == 0x1F && pos + 8 < d.size()) { // double precision (BCD)
                out += formatReal(bcdToDec(&d[pos+1], 7), true); pos += 9; continue;
            }

            // Keyword tokens.
            if (b == 0xFF && pos + 1 < d.size()) {
                const char* kw = extTokenText(d[pos+1]);
                if (kw) { out += kw; pos += 2; continue; }
                pos += 2; continue; // unknown extended token: drop
            }
            if (b >= 0x81) {
                const char* kw = tokenText(b);
                if (kw) {
                    out += kw;
                    if (b == 0x8F || b == 0xE6) restLiteral = true; // REM / '
                    if (b == 0x84) inData = true;                   // DATA
                    pos++;
                    continue;
                }
                pos++; continue; // unknown token
            }
            // Literal ASCII.
            out += (char)b;
            pos++;
        }
        out += '\n';
    }
    return out;
}

// ---- Block carving --------------------------------------------------------

const char* carveTypeName(CarveType t) {
    switch (t) {
        case CarveType::Bload:          return "BLOAD binary";
        case CarveType::BasicTokenized: return "Tokenized BASIC";
        default:                        return "Unknown / data";
    }
}

// Length of a tokenized BASIC program starting at d[pos] (d[pos]==0xFF), or 0 if
// it does not parse as at least one valid line ending in a 0x0000 link.
static long basicProgramLength(const std::vector<uint8_t>& d, long pos) {
    long p = pos + 1, n = (long)d.size();
    int lines = 0;
    while (p + 4 <= n) {
        uint16_t link = (uint16_t)(d[p] | (d[p+1] << 8));
        if (link == 0) return (p + 2) - pos;   // program terminator
        p += 4;
        long guard = 0;
        while (p < n && d[p] != 0x00 && guard++ < 4096) p++;
        if (p >= n) return 0;
        p++;                                    // skip line terminator
        if (++lines > 20000) return 0;
    }
    return 0;
}

std::vector<CarvedBlock> carveBlocks(const std::vector<uint8_t>& d) {
    std::vector<CarvedBlock> out;
    long n = (long)d.size();
    long pos = 0, gapStart = -1;
    bool afterBlock = false;

    auto flushGap = [&](long end) {
        if (gapStart >= 0 && end > gapStart) {
            CarvedBlock g; g.offset = gapStart; g.length = end - gapStart;
            g.type = CarveType::Unknown; out.push_back(g);
        }
        gapStart = -1;
    };

    while (pos < n) {
        bool boundary = (pos % 512 == 0) || afterBlock;
        bool matched = false;
        if (boundary) {
            if (d[pos] == 0xFE && pos + 7 <= n) {
                uint16_t s = d[pos+1] | (d[pos+2]<<8);
                uint16_t e = d[pos+3] | (d[pos+4]<<8);
                uint16_t x = d[pos+5] | (d[pos+6]<<8);
                long payload = (long)e - s + 1;
                if (e >= s && payload >= 1 && pos + 7 + payload <= n) {
                    flushGap(pos);
                    CarvedBlock b; b.offset = pos; b.length = 7 + payload;
                    b.type = CarveType::Bload; b.start = s; b.end = e; b.exec = x;
                    out.push_back(b);
                    pos += b.length; afterBlock = true; matched = true;
                }
            } else if (d[pos] == 0xFF) {
                long len = basicProgramLength(d, pos);
                if (len > 4) {
                    flushGap(pos);
                    CarvedBlock b; b.offset = pos; b.length = len;
                    b.type = CarveType::BasicTokenized;
                    out.push_back(b);
                    pos += len; afterBlock = true; matched = true;
                }
            }
        }
        if (!matched) {
            if (gapStart < 0) gapStart = pos;
            afterBlock = false;
            pos++;
        }
    }
    flushGap(n);
    return out;
}

// ---- Tokenizer (text -> tokenized BASIC) ----------------------------------

namespace {
struct Kw { std::string text; uint8_t code; bool ext; };

// Build a keyword list sorted longest-first for greedy longest-match.
const std::vector<Kw>& keywordList() {
    static std::vector<Kw> kws = [] {
        std::vector<Kw> v;
        for (int b = 0x81; b <= 0xFC; ++b) {
            const char* t = kTokens[b - 0x80];
            if (t) v.push_back({t, (uint8_t)b, false});
        }
        for (int b = 0x81; b <= 0xB0; ++b) {
            const char* t = kExtTokens[b - 0x80];
            if (t) v.push_back({t, (uint8_t)b, true});
        }
        v.push_back({"?", 0x91, false}); // ? is PRINT
        std::stable_sort(v.begin(), v.end(), [](const Kw& a, const Kw& b) {
            return a.text.size() > b.text.size();
        });
        return v;
    }();
    return kws;
}

bool isJumpToken(uint8_t code) { // integers after these are line numbers
    switch (code) {
        case 0x89: case 0x8D: case 0xDA: case 0xA1: // GOTO GOSUB THEN ELSE
        case 0x8C: case 0x8A: case 0xA9: case 0xAA: // RESTORE RUN AUTO RENUM
        case 0x93: case 0xA8: case 0xA7: case 0x8E: // LIST DELETE RESUME RETURN
            return true;
    }
    return false;
}

bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isAlpha(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }

// Parse a decimal number at ln[i]: digits, optional '.', optional E/D
// exponent (only when digits follow), optional type suffix ! # %.
struct ParsedNum {
    DecNum dec;
    bool hasPoint = false;
    char expChar = 0;          // 'E' or 'D' if an exponent was given
    char suffix = 0;           // '!' '#' '%' or 0
    bool integral = false;     // no point, no exponent
    double approx = 0;         // only for range checks
};

size_t parseNumber(const std::string& ln, size_t i, ParsedNum& n) {
    size_t m = ln.size();
    std::string digits;
    int pointPos = -1;
    while (i < m && (isDigit(ln[i]) || (ln[i] == '.' && pointPos < 0))) {
        if (ln[i] == '.') pointPos = (int)digits.size();
        else digits += ln[i];
        i++;
    }
    n.hasPoint = pointPos >= 0;
    if (pointPos < 0) pointPos = (int)digits.size();
    int exp10 = 0;
    if (i < m && (ln[i] == 'E' || ln[i] == 'e' || ln[i] == 'D' || ln[i] == 'd')) {
        size_t j = i + 1;
        bool negE = false;
        if (j < m && (ln[j] == '+' || ln[j] == '-')) { negE = ln[j] == '-'; j++; }
        if (j < m && isDigit(ln[j])) {
            n.expChar = (char)toupper((unsigned char)ln[i]);
            int e = 0;
            while (j < m && isDigit(ln[j])) { if (e < 1000) e = e * 10 + (ln[j] - '0'); j++; }
            exp10 = negE ? -e : e;
            i = j;
        }
    }
    if (i < m && (ln[i] == '!' || ln[i] == '#' || ln[i] == '%')) n.suffix = ln[i++];
    n.integral = !n.hasPoint && !n.expChar;

    size_t lead = digits.find_first_not_of('0');
    if (lead == std::string::npos) { n.dec = DecNum{}; n.approx = 0; return i; }
    n.dec.exp = pointPos - (int)lead + exp10;
    n.dec.digits = digits.substr(lead);
    while (!n.dec.digits.empty() && n.dec.digits.back() == '0') n.dec.digits.pop_back();
    n.approx = atof(("0." + n.dec.digits).c_str()) * std::pow(10.0, n.dec.exp);
    return i;
}

void emitReal(std::vector<uint8_t>& body, const DecNum& dec, bool dbl) {
    uint8_t b[8];
    decToBcd(dec, dbl ? 7 : 3, b);
    body.push_back(dbl ? 0x1F : 0x1D);
    body.insert(body.end(), b, b + (dbl ? 8 : 4));
}

void emitInt(std::vector<uint8_t>& body, long v) {
    if (v >= 0 && v <= 9) body.push_back((uint8_t)(0x11 + v));
    else if (v >= 10 && v <= 255) { body.push_back(0x0F); body.push_back((uint8_t)v); }
    else { body.push_back(0x1C); body.push_back(v & 0xFF); body.push_back((v >> 8) & 0xFF); }
}
} // namespace

std::vector<uint8_t> tokenizeBasic(const std::string& text) {
    std::vector<uint8_t> out;
    out.push_back(0xFF);

    struct Line { uint16_t no; std::vector<uint8_t> body; };
    std::vector<Line> lines;

    size_t p = 0, n = text.size();
    while (p < n) {
        // Read one source line.
        size_t eol = text.find('\n', p);
        std::string ln = text.substr(p, (eol == std::string::npos ? n : eol) - p);
        p = (eol == std::string::npos) ? n : eol + 1;
        if (!ln.empty() && ln.back() == '\r') ln.pop_back();
        size_t i = 0, m = ln.size();
        while (i < m && ln[i] == ' ') i++;
        if (i >= m) continue;                       // blank line
        if (!isDigit(ln[i])) continue;              // no line number -> skip
        long no = 0;
        while (i < m && isDigit(ln[i])) { no = no * 10 + (ln[i++] - '0'); if (no > 99999) no = 99999; }
        if (no > 65529) continue;                   // not a valid MSX line number
        if (i < m && ln[i] == ' ') i++;             // the separator LIST prints

        Line L; L.no = (uint16_t)no;
        bool lineRef = false;   // integers are line numbers (after GOTO etc.)
        bool inIdent = false;   // inside a variable name: digits stay ASCII
        while (i < m) {
            char c = ln[i];
            // String literal (stored verbatim, closing quote optional).
            if (c == '"') {
                L.body.push_back('"'); i++;
                while (i < m && ln[i] != '"') L.body.push_back((uint8_t)ln[i++]);
                if (i < m) { L.body.push_back('"'); i++; }
                lineRef = inIdent = false;
                continue;
            }
            // Digits that continue a variable name (A1, X12$) are plain ASCII.
            if (inIdent && isDigit(c)) { L.body.push_back((uint8_t)c); i++; continue; }

            // Keyword / operator match (longest first, case-insensitive).
            const Kw* best = nullptr;
            for (const auto& kw : keywordList()) {
                size_t L2 = kw.text.size();
                if (i + L2 > m) continue;
                bool eq = true;
                for (size_t k = 0; k < L2; ++k) {
                    if (toupper((unsigned char)ln[i + k]) != (unsigned char)kw.text[k]) { eq = false; break; }
                }
                if (eq) { best = &kw; break; } // list is longest-first
            }
            if (best) {
                i += best->text.size();
                inIdent = false;
                if (best->code == 0xE6 && !best->ext) {         // ' -> : REM '
                    L.body.push_back(':'); L.body.push_back(0x8F); L.body.push_back(0xE6);
                    while (i < m) L.body.push_back((uint8_t)ln[i++]);
                    break;
                }
                if (best->code == 0xA1 && !best->ext) L.body.push_back(':'); // ELSE -> : ELSE
                if (best->ext) { L.body.push_back(0xFF); L.body.push_back(best->code); }
                else L.body.push_back(best->code);
                if (!best->ext && best->code == 0x8F) {        // REM: rest is literal
                    while (i < m) L.body.push_back((uint8_t)ln[i++]);
                    break;
                }
                if (!best->ext && best->code == 0x84) {        // DATA: literal up to ':'
                    bool q = false;
                    while (i < m && (q || ln[i] != ':')) {
                        if (ln[i] == '"') q = !q;
                        L.body.push_back((uint8_t)ln[i++]);
                    }
                    lineRef = false;
                    continue;
                }
                bool keepRange = lineRef && !best->ext && best->code == 0xF2; // LIST 10-20
                lineRef = (!best->ext && isJumpToken(best->code)) || keepRange;
                continue;
            }
            // &H hex / &O or &<octal> octal constants; &B binary stays ASCII.
            if (c == '&' && i + 1 < m) {
                char t = (char)toupper((unsigned char)ln[i+1]);
                if (t == 'H' || t == 'O' || (ln[i+1] >= '0' && ln[i+1] <= '7')) {
                    bool hex = t == 'H';
                    i += (t == 'H' || t == 'O') ? 2 : 1;
                    unsigned v = 0;
                    while (i < m && (hex ? isxdigit((unsigned char)ln[i]) != 0 : (ln[i] >= '0' && ln[i] <= '7'))) {
                        char dch = (char)toupper((unsigned char)ln[i++]);
                        v = v * (hex ? 16 : 8) + (unsigned)(isDigit(dch) ? dch - '0' : dch - 'A' + 10);
                    }
                    L.body.push_back(hex ? 0x0C : 0x0B);
                    L.body.push_back(v & 0xFF); L.body.push_back((v >> 8) & 0xFF);
                    inIdent = lineRef = false;
                    continue;
                }
            }
            // Decimal number.
            if (isDigit(c) || (c == '.' && i + 1 < m && isDigit(ln[i+1]))) {
                ParsedNum pn;
                size_t end = parseNumber(ln, i, pn);
                if (lineRef && pn.integral && !pn.suffix && pn.approx <= 65535) {
                    long v = atol(ln.substr(i, end - i).c_str());
                    L.body.push_back(0x0E);
                    L.body.push_back(v & 0xFF); L.body.push_back((v >> 8) & 0xFF);
                } else if (pn.suffix == '!' || (pn.expChar == 'E' && pn.suffix != '#')) {
                    emitReal(L.body, pn.dec, false);
                } else if (pn.suffix == '#' || pn.expChar == 'D' || !pn.integral || pn.approx > 32767) {
                    emitReal(L.body, pn.dec, true);
                } else {
                    emitInt(L.body, atol(ln.substr(i, end - i).c_str()));
                    if (pn.suffix == '%') L.body.push_back('%');
                }
                i = end;
                inIdent = false;
                continue;
            }
            // Literal character. Letters start/continue a variable name and are
            // stored in upper case, as the MSX-BASIC cruncher does.
            if (isAlpha(c)) { inIdent = true; c = (char)toupper((unsigned char)c); }
            else inIdent = false;
            // Keep line-ref mode only across spaces and commas (ON..GOTO lists).
            if (c != ' ' && c != ',') lineRef = false;
            L.body.push_back((uint8_t)c);
            i++;
        }
        // A later line with the same number replaces the earlier one; a bare
        // line number deletes it (as typing it at the prompt would).
        auto it = std::find_if(lines.begin(), lines.end(), [&](const Line& x) { return x.no == L.no; });
        if (it != lines.end()) lines.erase(it);
        if (!L.body.empty()) lines.push_back(std::move(L));
    }
    std::stable_sort(lines.begin(), lines.end(), [](const Line& a, const Line& b) { return a.no < b.no; });

    // Emit with computed link addresses (base 0x8001).
    uint16_t addr = 0x8001;
    for (auto& L : lines) {
        uint16_t lineLen = (uint16_t)(4 + L.body.size() + 1);
        uint16_t link = (uint16_t)(addr + lineLen);
        out.push_back(link & 0xFF); out.push_back(link >> 8);
        out.push_back(L.no & 0xFF); out.push_back(L.no >> 8);
        out.insert(out.end(), L.body.begin(), L.body.end());
        out.push_back(0x00);
        addr = link;
    }
    out.push_back(0x00); out.push_back(0x00); // end of program
    return out;
}

} // namespace msx
