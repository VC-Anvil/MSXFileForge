#include "msxfile.h"
#include <cmath>
#include <cstdio>
#include <cstring>
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

// Decode a Microsoft/MSX BCD float. mant = number of mantissa bytes (3=single,
// 7=double). Returns the value as a double; caller formats it.
static double decodeBcd(const uint8_t* p, int mantBytes) {
    uint8_t expByte = p[0];
    if (expByte == 0) return 0.0;
    bool neg = (expByte & 0x80) != 0;
    int exp = (expByte & 0x7F) - 0x40;
    double mant = 0.0, scale = 0.1;
    for (int i = 1; i <= mantBytes; ++i) {
        int hi = (p[i] >> 4) & 0xF, lo = p[i] & 0xF;
        mant += hi * scale; scale *= 0.1;
        mant += lo * scale; scale *= 0.1;
    }
    double v = mant * std::pow(10.0, exp);
    return neg ? -v : v;
}

static void appendNumber(std::string& out, double v, bool integral) {
    char buf[64];
    if (integral) { snprintf(buf, sizeof buf, "%.0f", v); out += buf; return; }
    snprintf(buf, sizeof buf, "%.9g", v);
    out += buf;
}

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

        bool inString = false, restLiteral = false;
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
            if (b == 0x1D && pos + 4 < d.size()) { // single float (BCD)
                appendNumber(out, decodeBcd(&d[pos+1], 3), false); pos += 5; continue;
            }
            if (b == 0x1F && pos + 8 < d.size()) { // double float (BCD)
                appendNumber(out, decodeBcd(&d[pos+1], 7), false); pos += 9; continue;
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
        pos = pos; // link is advisory; we stream sequentially
    }
    return out;
}

} // namespace msx
