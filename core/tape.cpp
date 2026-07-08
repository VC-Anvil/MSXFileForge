// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Alonso (GS·RUN). MSXFileForge.
// Free software under the GNU GPL v3 (or later) - see LICENSE. Modified
// versions must be distributed under the same license.
#include "tape.h"
#include <cmath>
#include <cstring>
#include <vector>
#include <algorithm>

static const double kPI = 3.14159265358979323846;

namespace msx {

static const uint8_t kSync[8] = {0x1F,0xA6,0xDE,0xBA,0xCC,0x13,0x7D,0x74};
static const int OUT_FREQ    = 43200;
static const int LONG_HZ     = 1200;   // "0" bit tone
static const int SHORT_HZ    = 2400;   // "1" bit tone / pilot
static const int LONG_HEADER = 16000;  // pilot cycles before a file header
static const int SHORT_HEADER= 4000;   // pilot cycles before data
static const uint32_t Z80HZ  = 3500000;

// ---- helpers --------------------------------------------------------------
static void put16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(x&0xFF); v.push_back(x>>8); }
static void put32(std::vector<uint8_t>& v, uint32_t x) { v.push_back(x&0xFF); v.push_back((x>>8)&0xFF); v.push_back((x>>16)&0xFF); v.push_back((x>>24)&0xFF); }
static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1]<<8)); }
static uint32_t rd32(const uint8_t* p) { return p[0] | (p[1]<<8) | (p[2]<<16) | ((uint32_t)p[3]<<24); }

bool isTsx(const std::vector<uint8_t>& d) { return d.size() >= 8 && memcmp(d.data(), "ZXTape!", 7) == 0; }
bool isWav(const std::vector<uint8_t>& d) { return d.size() >= 12 && memcmp(d.data(), "RIFF", 4) == 0 && memcmp(d.data()+8, "WAVE", 4) == 0; }

// Marker positions in a .cas (8-byte aligned scan, tolerant of hand-made data).
static std::vector<size_t> syncPositions(const std::vector<uint8_t>& d) {
    std::vector<size_t> s;
    for (size_t p = 0; p + 8 <= d.size(); ++p)
        if (memcmp(&d[p], kSync, 8) == 0) { s.push_back(p); p += 7; }
    return s;
}
static bool looksLikeFileHeader(const uint8_t* p, size_t n) {
    if (n < 10) return false;
    uint8_t m = p[0];
    if (m != 0xD0 && m != 0xD3 && m != 0xEA) return false;
    for (int i = 1; i < 10; ++i) if (p[i] != m) return false;
    return true;
}

// ---- CAS -> WAV -----------------------------------------------------------
static void writePulse(std::vector<uint8_t>& out, int hz, int baud) {
    int length = (int)lround((double)OUT_FREQ / (baud * ((double)hz / 1200.0)));
    if (length < 1) length = 1;
    double scale = 2.0 * kPI / length;
    for (int n = 0; n < length; ++n)
        out.push_back((uint8_t)(128 + (int)(std::sin(n * scale) * 127)));
}
static void writeByte(std::vector<uint8_t>& out, uint8_t b, int baud) {
    writePulse(out, LONG_HZ, baud);                        // start bit (0)
    for (int i = 0; i < 8; ++i) {
        if (b & 1) { writePulse(out, SHORT_HZ, baud); writePulse(out, SHORT_HZ, baud); }
        else writePulse(out, LONG_HZ, baud);
        b >>= 1;
    }
    for (int i = 0; i < 4; ++i) writePulse(out, SHORT_HZ, baud); // two stop bits
}
static void writeHeaderTone(std::vector<uint8_t>& out, int cycles, int baud) {
    for (int i = 0; i < cycles * (baud / 1200); ++i) writePulse(out, SHORT_HZ, baud);
}

std::vector<uint8_t> casToWav(const std::vector<uint8_t>& cas, int baud) {
    std::vector<uint8_t> pcm;
    auto syncs = syncPositions(cas);
    for (size_t s = 0; s < syncs.size(); ++s) {
        size_t segStart = syncs[s] + 8;
        size_t segEnd = (s + 1 < syncs.size()) ? syncs[s + 1] : cas.size();
        bool fileHdr = looksLikeFileHeader(cas.data() + segStart, segEnd - segStart);
        for (int i = 0; i < (fileHdr ? OUT_FREQ * 2 : OUT_FREQ); ++i) pcm.push_back(128); // silence gap
        writeHeaderTone(pcm, fileHdr ? LONG_HEADER : SHORT_HEADER, baud);
        for (size_t i = segStart; i < segEnd; ++i) writeByte(pcm, cas[i], baud);
    }
    // 44-byte PCM WAV header (8-bit mono).
    std::vector<uint8_t> wav;
    for (char c : std::string("RIFF")) wav.push_back(c);
    put32(wav, (uint32_t)(36 + pcm.size()));
    for (char c : std::string("WAVEfmt ")) wav.push_back(c);
    put32(wav, 16); put16(wav, 1); put16(wav, 1);          // PCM, mono
    put32(wav, OUT_FREQ); put32(wav, OUT_FREQ);            // rate, byte rate
    put16(wav, 1); put16(wav, 8);                          // block align, bits
    for (char c : std::string("data")) wav.push_back(c);
    put32(wav, (uint32_t)pcm.size());
    wav.insert(wav.end(), pcm.begin(), pcm.end());
    return wav;
}

// ---- CAS -> TSX -----------------------------------------------------------
std::vector<uint8_t> casToTsx(const std::vector<uint8_t>& cas, int baud) {
    std::vector<uint8_t> t;
    for (char c : std::string("ZXTape!")) t.push_back(c);
    t.push_back(0x1A); t.push_back(1); t.push_back(20);    // v1.20
    uint16_t pilot = (uint16_t)lround((double)Z80HZ / (baud * 4));
    uint16_t bit1 = pilot, bit0 = (uint16_t)(pilot * 2);
    auto syncs = syncPositions(cas);
    for (size_t s = 0; s < syncs.size(); ++s) {
        size_t a = syncs[s] + 8, b = (s + 1 < syncs.size()) ? syncs[s + 1] : cas.size();
        bool fileHdr = looksLikeFileHeader(cas.data() + a, b - a);
        uint16_t pulses = (uint16_t)(fileHdr ? LONG_HEADER : SHORT_HEADER);
        uint16_t pause  = (uint16_t)(fileHdr ? 2000 : 1000);
        uint32_t dsz = (uint32_t)(b - a);
        t.push_back(0x4B);
        put32(t, 12 + dsz);
        put16(t, pause); put16(t, pilot); put16(t, pulses);
        put16(t, bit0); put16(t, bit1);
        t.push_back(0x24);   // bitcfg: 2 pulses/0-bit, 4 pulses/1-bit
        t.push_back(0x54);   // bytecfg: 1 start bit(0), 2 stop bits(1), LSB first
        t.insert(t.end(), cas.begin() + a, cas.begin() + b);
    }
    return t;
}

// ---- TSX -> CAS -----------------------------------------------------------
std::vector<uint8_t> tsxToCas(const std::vector<uint8_t>& t, std::string* err) {
    std::vector<uint8_t> cas;
    if (!isTsx(t)) { if (err) *err = "not a TSX/TZX file"; return cas; }
    size_t p = 10; // skip "ZXTape!" 0x1A + 2 version bytes
    while (p < t.size()) {
        uint8_t id = t[p++];
        size_t skip = 0;
        switch (id) {
            case 0x4B: { // MSX KCS: DWORD len, then 12-byte params + data
                if (p + 4 > t.size()) { if (err) *err = "truncated 0x4B"; return cas; }
                uint32_t len = rd32(&t[p]); p += 4;
                if (p + len > t.size() || len < 12) { if (err) *err = "bad 0x4B length"; return cas; }
                const uint8_t* data = &t[p + 12]; uint32_t dsz = len - 12;
                cas.insert(cas.end(), kSync, kSync + 8);
                cas.insert(cas.end(), data, data + dsz);
                p += len; continue;
            }
            case 0x10: if (p+4>t.size()) return cas; skip = 4 + rd16(&t[p+2]); break;          // std speed
            case 0x11: if (p+18>t.size()) return cas; skip = 18 + (t[p+15]|(t[p+16]<<8)|(t[p+17]<<16)); break; // turbo
            case 0x12: skip = 4; break;                                                        // pure tone
            case 0x13: if (p>=t.size()) return cas; skip = 1 + t[p]*2; break;                  // pulse seq
            case 0x14: if (p+10>t.size()) return cas; skip = 10 + (t[p+7]|(t[p+8]<<8)|(t[p+9]<<16)); break; // pure data
            case 0x20: skip = 2; break;                                                        // pause
            case 0x21: if (p>=t.size()) return cas; skip = 1 + t[p]; break;                    // group start
            case 0x22: skip = 0; break;                                                        // group end
            case 0x24: skip = 2; break;                                                        // loop start
            case 0x25: skip = 0; break;                                                        // loop end
            case 0x2A: skip = 4; break;                                                        // stop-if-48k
            case 0x30: if (p>=t.size()) return cas; skip = 1 + t[p]; break;                    // text description
            case 0x31: if (p+1>=t.size()) return cas; skip = 2 + t[p+1]; break;                // message
            case 0x32: if (p+2>t.size()) return cas; skip = 2 + rd16(&t[p]); break;            // archive info
            case 0x33: if (p>=t.size()) return cas; skip = 1 + t[p]*3; break;                  // hardware
            case 0x35: if (p+20>t.size()) return cas; skip = 20 + rd32(&t[p+16]); break;       // custom info
            case 0x5A: skip = 9; break;                                                        // glue
            default:
                if (err) *err = "unknown TSX block ID 0x" + std::string(1, "0123456789ABCDEF"[(id>>4)&0xF]) + std::string(1, "0123456789ABCDEF"[id&0xF]);
                return cas;
        }
        p += skip;
    }
    return cas;
}

// ---- WAV -> CAS (best-effort, clean signals) ------------------------------
std::vector<uint8_t> wavToCas(const std::vector<uint8_t>& wav, std::string* err) {
    std::vector<uint8_t> cas;
    if (!isWav(wav)) { if (err) *err = "not a WAV file"; return cas; }
    // Locate fmt and data chunks.
    uint32_t rate = 0; int bits = 0, ch = 0; size_t dataOff = 0, dataLen = 0;
    size_t p = 12;
    while (p + 8 <= wav.size()) {
        uint32_t sz = rd32(&wav[p + 4]);
        if (memcmp(&wav[p], "fmt ", 4) == 0 && p + 8 + 16 <= wav.size()) {
            ch = rd16(&wav[p + 10]); rate = rd32(&wav[p + 12]); bits = rd16(&wav[p + 22]);
        } else if (memcmp(&wav[p], "data", 4) == 0) {
            dataOff = p + 8; dataLen = sz; break;
        }
        p += 8 + sz + (sz & 1);
    }
    if (!rate || !bits || !dataLen) { if (err) *err = "unsupported WAV"; return cas; }

    // Read channel-0 samples, centered at 0.
    int bytesPerSample = bits / 8, stride = bytesPerSample * (ch ? ch : 1);
    std::vector<int> sig;
    for (size_t i = dataOff; i + stride <= dataOff + dataLen && i + stride <= wav.size(); i += stride) {
        int v = (bits == 8) ? (int)wav[i] - 128 : (int16_t)rd16(&wav[i]);
        sig.push_back(v);
    }
    // Split into tone blocks separated by silence (the inter-block gaps). Each
    // block = one header tone + framed bytes; a .cas sync marker precedes it.
    int amp = (bits == 8) ? 24 : 24 * 256;
    long minSilence = (long)rate / 20;               // 50 ms of quiet = a gap
    long i = 0, n = (long)sig.size();
    while (i < n) {
        while (i < n && std::abs(sig[i]) < amp) i++;  // skip silence
        long start = i;
        long quiet = 0;
        while (i < n && quiet < minSilence) { if (std::abs(sig[i]) < amp) quiet++; else quiet = 0; i++; }
        long end = i - quiet;
        if (end - start < 64) continue;               // too short to be a block

        // Half-wave lengths inside this block.
        std::vector<int> halves; int last = 0, run = 0;
        for (long k = start; k < end; ++k) {
            int v = sig[k]; int s = (v > 2) ? 1 : (v < -2 ? -1 : last);
            if (s != last && last != 0) { halves.push_back(run); run = 0; }
            run++; last = s;
        }
        if (halves.size() < 16) continue;
        std::vector<int> sorted = halves; std::sort(sorted.begin(), sorted.end());
        int shortLen = sorted[sorted.size() / 4];
        double thresh = shortLen * 1.5;
        std::vector<int> pulse; for (int h : halves) pulse.push_back(h > thresh ? 1 : 0);
        // Skip the leading pilot (long run of shorts), then decode bits.
        std::vector<int> bitv; size_t j = 0;
        while (j < pulse.size()) {
            if (pulse[j] == 0) {                       // short -> part of '1' (4 shorts)
                if (j + 4 <= pulse.size() && !pulse[j+1] && !pulse[j+2] && !pulse[j+3]) { bitv.push_back(1); j += 4; }
                else j++;
            } else {                                   // long -> '0' (2 longs)
                if (j + 2 <= pulse.size() && pulse[j+1]) { bitv.push_back(0); j += 2; }
                else j++;
            }
        }
        // Decode framed bytes; the sync marker precedes this block's data.
        std::vector<uint8_t> blk;
        for (size_t b = 0; b + 9 <= bitv.size(); ) {   // start + 8 data (stop bits optional at tail)
            if (bitv[b] != 0) { b++; continue; }
            int byte = 0; for (int k = 0; k < 8; ++k) byte |= (bitv[b + 1 + k] << k);
            blk.push_back((uint8_t)byte); b += 11;
        }
        if (!blk.empty()) {
            cas.insert(cas.end(), kSync, kSync + 8);
            cas.insert(cas.end(), blk.begin(), blk.end());
        }
    }
    if (cas.empty() && err) *err = "no tape blocks decoded";
    return cas;
}

} // namespace msx
