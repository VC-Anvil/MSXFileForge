// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Alonso (GS·RUN). MSXFileForge.
// Free software under the GNU GPL v3 (or later) - see LICENSE. Modified
// versions must be distributed under the same license.
// Cassette: .cas block parsing and .cas <-> .wav / .tsx conversion.
#include "check.h"
#include "cas.h"
#include "msxfile.h"
#include "tape.h"
#include <cstring>

using namespace msx;

namespace {

typedef std::vector<uint8_t> Bytes;
const uint8_t kSync[8] = {0x1F,0xA6,0xDE,0xBA,0xCC,0x13,0x7D,0x74};

void block(Bytes& cas, const Bytes& payload) {
    while (cas.size() % 8) cas.push_back(0x00);   // .cas blocks are 8-byte aligned
    cas.insert(cas.end(), kSync, kSync + 8);
    cas.insert(cas.end(), payload.begin(), payload.end());
}
Bytes header(uint8_t type, const char* name) {
    Bytes h(10, type);
    for (int i = 0; i < 6; ++i) h.push_back((uint8_t)(name[i] ? name[i] : ' '));
    return h;
}

// A cassette with a tokenized BASIC file and a BLOAD binary.
Bytes sampleCas() {
    Bytes cas;
    block(cas, header(0xD3, "HELLO "));
    Bytes prog = tokenizeBasic("10 PRINT \"HI\"\n20 GOTO 10\n");
    prog.erase(prog.begin());                     // the 0xFF is not stored on tape
    prog.insert(prog.end(), 7, 0x00);
    block(cas, prog);
    block(cas, header(0xD0, "GAME  "));
    Bytes bin{0x00, 0xC0, 0x0F, 0xC0, 0x00, 0xC0};
    for (int i = 0; i < 16; ++i) bin.push_back((uint8_t)(i * 17));
    block(cas, bin);
    return cas;
}

} // namespace

TEST(cas_parse_blocks) {
    Bytes cas = sampleCas();
    auto bl = parseCas(cas);
    CHECK_EQ(bl.size(), (size_t)4);
    if (bl.size() == 4) {
        CHECK_EQ(bl[0].kind, CasBlockKind::BasicTokenized);
        CHECK_EQ(bl[0].filename, std::string("HELLO"));
        CHECK_EQ(bl[1].kind, CasBlockKind::Custom);
        CHECK_EQ(bl[2].kind, CasBlockKind::Binary);
        CHECK_EQ(bl[2].filename, std::string("GAME"));
        CHECK_EQ(bl[3].offset % 8, 0L);
        CHECK_EQ(bl[3].dataLength, (long)(cas.size() - bl[3].dataOffset));
    }
    CHECK(parseCas({}).empty());
    CHECK_EQ(std::string(casKindName(CasBlockKind::Ascii)), std::string("ASCII file"));
}

TEST(tape_sniffers) {
    Bytes w = casToWav(sampleCas());
    CHECK(isWav(w)); CHECK(!isTsx(w));
    Bytes t = casToTsx(sampleCas());
    CHECK(isTsx(t)); CHECK(!isWav(t));
    CHECK(!isWav({'R','I','F','F'}));
}

TEST(tape_wav_format) {
    Bytes w = casToWav(sampleCas());
    CHECK(w.size() > 44);
    // 8-bit mono PCM at 43200 Hz, sizes consistent.
    auto rd32 = [&](size_t o) { return (uint32_t)(w[o] | (w[o+1] << 8) | (w[o+2] << 16) | ((uint32_t)w[o+3] << 24)); };
    CHECK_EQ(rd32(4), (uint32_t)(w.size() - 8));
    CHECK_EQ(rd32(24), 43200u);
    CHECK_EQ((int)w[34], 8);
    CHECK_EQ(rd32(40), (uint32_t)(w.size() - 44));
}

TEST(tape_cas_wav_roundtrip) {
    for (int baud : {1200, 2400}) {
        Bytes cas = sampleCas();
        std::string err;
        Bytes back = wavToCas(casToWav(cas, baud), &err);
        if (back != cas) fprintf(stderr, "    baud %d: %zu vs %zu bytes (%s)\n", baud, back.size(), cas.size(), err.c_str());
        CHECK(back == cas);
    }
}

TEST(tape_cas_tsx_roundtrip) {
    Bytes cas = sampleCas();
    std::string err;
    Bytes tsx = casToTsx(cas);
    CHECK(tsxToCas(tsx, &err) == cas);
    CHECK(err.empty());
    // Metadata blocks are skipped: insert a text description (0x30) and an
    // archive-info block (0x32) after the TZX header.
    Bytes meta{0x30, 3, 'M', 'S', 'X', 0x32, 4, 0, 1, 0x00, 1, 'X'};
    tsx.insert(tsx.begin() + 10, meta.begin(), meta.end());
    err.clear();
    CHECK(tsxToCas(tsx, &err) == cas);
    CHECK(err.empty());
}

TEST(tape_tsx_errors) {
    std::string err;
    CHECK(tsxToCas({1, 2, 3}, &err).empty());
    CHECK(!err.empty());
    Bytes t = casToTsx(sampleCas());
    t.push_back(0x99);                       // unknown block id
    err.clear();
    CHECK(!tsxToCas(t, &err).empty());       // data up to the bad block survives
    CHECK(err.find("0x99") != std::string::npos);
    Bytes cut = casToTsx(sampleCas());
    cut.resize(cut.size() - 5);              // truncated last 0x4B block
    err.clear();
    tsxToCas(cut, &err);
    CHECK(!err.empty());
}

TEST(tape_wav_errors) {
    std::string err;
    CHECK(wavToCas({1, 2, 3}, &err).empty());
    CHECK(!err.empty());
}
