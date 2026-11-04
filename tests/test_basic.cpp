// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Alonso (GS·RUN). MSXFileForge.
// Free software under the GNU GPL v3 (or later) - see LICENSE. Modified
// versions must be distributed under the same license.
// MSX-BASIC tokenizer / detokenizer, file type detection and block carving.
#include "check.h"
#include "msxfile.h"

using namespace msx;

namespace {

typedef std::vector<uint8_t> Bytes;

// Body bytes of the first (only) line of a tokenized program.
Bytes body(const std::string& line) {
    Bytes t = tokenizeBasic(line);
    // FF, link(2), lineno(2), body..., 00, 00 00
    if (t.size() < 8) return {};
    return Bytes(t.begin() + 5, t.end() - 3);
}

std::string hex(const Bytes& b) {
    std::string s; char t[4];
    for (auto x : b) { snprintf(t, sizeof t, "%02X ", x); s += t; }
    if (!s.empty()) s.pop_back();
    return s;
}

#define CHECK_BODY(src, expected) CHECK_EQ(hex(body(src)), std::string(expected))

} // namespace

TEST(basic_program_layout) {
    Bytes t = tokenizeBasic("10 PRINT 1\n20 END\n");
    // Line 10: link 0x8009 (0x8001 + 4 header + 3 body + 1 terminator).
    CHECK_EQ(hex(t), std::string("FF 09 80 0A 00 91 20 12 00 0F 80 14 00 81 00 00 00"));
}

TEST(basic_keywords_and_case) {
    CHECK_BODY("10 print a", "91 20 41");
    CHECK_BODY("10 ? \"hi\"", "91 20 22 68 69 22");
    CHECK_BODY("10 X=LEFT$(A$,2)", "58 EF FF 81 28 41 24 2C 13 29");
    CHECK_BODY("10 IF A>=B THEN STOP", "8B 20 41 EE EF 42 20 DA 20 90");
}

TEST(basic_variable_digits_stay_ascii) {
    // Regression: "A1" was crunched to 'A' + constant 1, a syntax error on MSX.
    CHECK_BODY("10 A1=5", "41 31 EF 16");
    CHECK_BODY("10 X12$=\"\"", "58 31 32 24 EF 22 22");
    CHECK_BODY("10 A=B2+3", "41 EF 42 32 F1 14");
}

TEST(basic_integer_constants) {
    CHECK_BODY("10 A=0", "41 EF 11");
    CHECK_BODY("10 A=9", "41 EF 1A");
    CHECK_BODY("10 A=10", "41 EF 0F 0A");
    CHECK_BODY("10 A=255", "41 EF 0F FF");
    CHECK_BODY("10 A=256", "41 EF 1C 00 01");
    CHECK_BODY("10 A=32767", "41 EF 1C FF 7F");
    // Beyond the integer range: double precision.
    CHECK_BODY("10 A=32768", "41 EF 1F 45 32 76 80 00 00 00 00");
}

TEST(basic_real_constants) {
    CHECK_BODY("10 A=1.5", "41 EF 1F 41 15 00 00 00 00 00 00");
    CHECK_BODY("10 A=1.5!", "41 EF 1D 41 15 00 00");
    CHECK_BODY("10 A=1#", "41 EF 1F 41 10 00 00 00 00 00 00");
    CHECK_BODY("10 A=.01", "41 EF 1F 3F 10 00 00 00 00 00 00");
    CHECK_BODY("10 A=0.1", "41 EF 1F 40 10 00 00 00 00 00 00");
    CHECK_BODY("10 A=1E5", "41 EF 1D 46 10 00 00");
    CHECK_BODY("10 A=1D-3", "41 EF 1F 3E 10 00 00 00 00 00 00");
    CHECK_BODY("10 A=3.14159265358979", "41 EF 1F 41 31 41 59 26 53 58 98");
    // 15 significant digits round half-up to 14.
    CHECK_BODY("10 A=123456789012345", "41 EF 1F 4F 12 34 56 78 90 12 35");
    CHECK_BODY("10 A=9.9999999999999", "41 EF 1F 41 99 99 99 99 99 99 99");
    CHECK_BODY("10 A=9.999999999999999", "41 EF 1F 42 10 00 00 00 00 00 00");
    CHECK_BODY("10 A=1.2345678!", "41 EF 1D 41 12 34 57");
}

TEST(basic_exponent_needs_digits) {
    // "1ELSE": the E belongs to ELSE, not to an exponent.
    CHECK_BODY("10 IF A THEN 1ELSE 2", "8B 20 41 20 DA 20 0E 01 00 3A A1 20 0E 02 00");
}

TEST(basic_hex_octal_binary) {
    CHECK_BODY("10 A=&HFF", "41 EF 0C FF 00");
    CHECK_BODY("10 A=&hc000", "41 EF 0C 00 C0");
    CHECK_BODY("10 A=&O17", "41 EF 0B 0F 00");
    CHECK_BODY("10 A=&17", "41 EF 0B 0F 00");
    // &B is not crunched: stays ASCII. Used to hang the tokenizer forever.
    CHECK_BODY("10 A=&B101", "41 EF 26 42 31 30 31");
    CHECK_BODY("10 A$=\"&\"+B$", "41 24 EF 22 26 22 F1 42 24");
}

TEST(basic_line_references) {
    CHECK_BODY("10 GOTO 100", "89 20 0E 64 00");
    CHECK_BODY("10 GOSUB 5", "8D 20 0E 05 00");
    CHECK_BODY("10 ON X GOTO 10,20,30", "95 20 58 20 89 20 0E 0A 00 2C 0E 14 00 2C 0E 1E 00");
    CHECK_BODY("10 RESTORE 1000", "8C 20 0E E8 03");
    CHECK_BODY("10 RESUME 50", "A7 20 0E 32 00");
    CHECK_BODY("10 ON ERROR GOTO 900", "95 20 A6 20 89 20 0E 84 03");
    // After the jump target, ordinary integers again.
    CHECK_BODY("10 GOTO 10:A=10", "89 20 0E 0A 00 3A 41 EF 0F 0A");
}

TEST(basic_else_and_apostrophe_carry_a_colon) {
    CHECK_BODY("10 IF A THEN 10 ELSE 20", "8B 20 41 20 DA 20 0E 0A 00 20 3A A1 20 0E 14 00");
    CHECK_BODY("10 CLS ' clear", "9F 20 3A 8F E6 20 63 6C 65 61 72");
    CHECK_BODY("10 REM Print 5:GOTO 1", "8F 20 50 72 69 6E 74 20 35 3A 47 4F 54 4F 20 31");
}

TEST(basic_data_is_literal) {
    // DATA items are not crunched (nor upper-cased) up to the next ':'.
    CHECK_BODY("10 DATA 1,abc,PRINT,\"x:y\":A=1",
               "84 20 31 2C 61 62 63 2C 50 52 49 4E 54 2C 22 78 3A 79 22 3A 41 EF 12");
}

TEST(basic_strings_are_literal) {
    CHECK_BODY("10 PRINT \"goto 10\"", "91 20 22 67 6F 74 6F 20 31 30 22");
    CHECK_BODY("10 PRINT \"open", "91 20 22 6F 70 65 6E");
}

TEST(basic_lines_sorted_replaced_deleted) {
    Bytes t = tokenizeBasic("30 C\n10 A\n20 B\n10 X\n30\n");
    CHECK_EQ(detokenizeBasic(t), std::string("10 X\n20 B\n"));
    // Lines without a number and out-of-range numbers are ignored.
    CHECK_EQ(detokenizeBasic(tokenizeBasic("PRINT\n70000 A\n65529 B\n")), std::string("65529 B\n"));
    CHECK_EQ(hex(tokenizeBasic("")), std::string("FF 00 00"));
}

TEST(basic_detokenize_numbers) {
    auto line = [](const Bytes& b) {
        Bytes t{0xFF, 0x00, 0x80, 0x0A, 0x00};
        t.insert(t.end(), b.begin(), b.end());
        t.push_back(0); t.push_back(0); t.push_back(0);
        return detokenizeBasic(t);
    };
    CHECK_EQ(line({0x1D, 0x41, 0x15, 0x00, 0x00}), std::string("10 1.5!\n"));
    CHECK_EQ(line({0x1F, 0x41, 0x10, 0, 0, 0, 0, 0, 0}), std::string("10 1#\n"));
    CHECK_EQ(line({0x1F, 0x3F, 0x10, 0, 0, 0, 0, 0, 0}), std::string("10 .01\n"));
    CHECK_EQ(line({0x1F, 0x45, 0x32, 0x76, 0x80, 0, 0, 0, 0}), std::string("10 32768\n"));
    CHECK_EQ(line({0x1F, 0x55, 0x10, 0, 0, 0, 0, 0, 0}), std::string("10 1D+20\n"));
    CHECK_EQ(line({0x1D, 0x37, 0x25, 0x00, 0x00}), std::string("10 2.5E-10\n"));
    CHECK_EQ(line({0x1F, 0x41, 0x31, 0x41, 0x59, 0x26, 0x53, 0x58, 0x98}), std::string("10 3.1415926535898\n"));
    CHECK_EQ(line({0x1F, 0x00, 0, 0, 0, 0, 0, 0, 0}), std::string("10 0#\n"));
    CHECK_EQ(line({0x1C, 0x00, 0x80}), std::string("10 -32768\n"));
    CHECK_EQ(line({0x0C, 0x00, 0xC0}), std::string("10 &HC000\n"));
    CHECK_EQ(line({0x0B, 0x0F, 0x00}), std::string("10 &O17\n"));
}

TEST(basic_roundtrip_text) {
    const char* prog =
        "10 SCREEN 5:COLOR 15,0,0:CLS\n"
        "20 DEFINT A-Z:DIM M(10,10)\n"
        "30 FOR I=0 TO 255 STEP 2:PSET (I,I),I MOD 16:NEXT I\n"
        "40 A$=INKEY$:IF A$=\"\" THEN 40 ELSE PRINT A$;\n"
        "50 X1=X1+.5:Y#=1.5:Z!=2.5!:W=1D+20:V=2.5E-10\n"
        "60 ON STRIG(0) GOSUB 1000:STRIG(0) ON\n"
        "70 DATA 1,2,three,\"four:five\"\n"
        "80 POKE &HF3DB,0:A=&O777:B=&B1010\n"
        "90 PRINT USING \"###.##\";3.14159 ' pi\n"
        "100 IF A<>B AND NOT C OR D XOR E THEN GOTO 10\n"
        "110 BLOAD \"GAME.BIN\",R\n"
        "1000 RETURN\n";
    Bytes t = tokenizeBasic(prog);
    CHECK_EQ(detokenizeBasic(t), std::string(prog));
    CHECK(tokenizeBasic(detokenizeBasic(t)) == t);
}

TEST(basic_roundtrip_bytes_preserves_types) {
    // Every constant form must come back as the same token after a
    // detokenize -> tokenize cycle.
    const char* forms[] = {
        "10 A=1.5!", "10 A=1#", "10 A=1", "10 A=100000", "10 A=.000001",
        "10 A=1E-20", "10 A=1D+60", "10 A=123456.7!", "10 A=12345678901234",
        "10 A=0!", "10 A=0#", "10 A=99999#", "10 A=-3.5",
    };
    for (const char* f : forms) {
        Bytes t = tokenizeBasic(f);
        Bytes back = tokenizeBasic(detokenizeBasic(t));
        if (back != t) fprintf(stderr, "    form %s -> %s", f, detokenizeBasic(t).c_str());
        CHECK(back == t);
    }
}

TEST(basic_detokenize_non_basic) {
    CHECK(detokenizeBasic({}).empty());
    CHECK(detokenizeBasic({0xFE, 1, 2}).empty());
    // Truncated program: must not read past the end.
    CHECK_EQ(detokenizeBasic({0xFF, 0x10, 0x80, 0x0A, 0x00, 0x91, 0x1F, 0x41}),
             std::string("10 PRINT\x1F" "A\n"));
}

TEST(file_type_detection) {
    CHECK_EQ(detectType({}), MsxFileType::Unknown);
    CHECK_EQ(detectType({0xFF, 0, 0}), MsxFileType::BasicTokenized);
    CHECK_EQ(detectType({0xFE, 0, 0xC0, 0xFF, 0xC0, 0, 0xC0}), MsxFileType::BloadBinary);
    std::string a = "10 PRINT \"HI\"\r\n20 GOTO 10\r\n";
    CHECK_EQ(detectType(Bytes(a.begin(), a.end())), MsxFileType::BasicAscii);
    std::string txt = "Hello, this is a README.\n";
    CHECK_EQ(detectType(Bytes(txt.begin(), txt.end())), MsxFileType::Text);
    CHECK_EQ(detectType({0x00, 0x01, 0x02, 0x80, 0x90}), MsxFileType::Data);

    BloadHeader h = parseBloadHeader({0xFE, 0x00, 0xC0, 0xFF, 0xC0, 0x10, 0xC0});
    CHECK(h.valid);
    CHECK_EQ(h.start, 0xC000); CHECK_EQ(h.end, 0xC0FF); CHECK_EQ(h.exec, 0xC010);
    CHECK(!parseBloadHeader({0xFE, 0x00, 0xC0, 0x00, 0xB0, 0, 0}).valid);
}

TEST(carve_blocks) {
    Bytes blob(512, 0xAA);                        // unknown leader
    Bytes bl{0xFE, 0x00, 0xC0, 0x03, 0xC0, 0x00, 0xC0, 1, 2, 3, 4};
    blob.insert(blob.end(), bl.begin(), bl.end()); // BLOAD at 512, 4 payload bytes
    Bytes bas = tokenizeBasic("10 PRINT 1\n");
    blob.insert(blob.end(), bas.begin(), bas.end()); // right after the block
    blob.insert(blob.end(), 20, 0x55);
    auto blocks = carveBlocks(blob);
    CHECK_EQ(blocks.size(), (size_t)4);
    if (blocks.size() == 4) {
        CHECK_EQ(blocks[0].type, CarveType::Unknown); CHECK_EQ(blocks[0].length, 512L);
        CHECK_EQ(blocks[1].type, CarveType::Bload);   CHECK_EQ(blocks[1].offset, 512L);
        CHECK_EQ(blocks[1].length, 11L);             CHECK_EQ(blocks[1].exec, 0xC000);
        CHECK_EQ(blocks[2].type, CarveType::BasicTokenized);
        CHECK_EQ(blocks[2].length, (long)bas.size());
        CHECK_EQ(blocks[3].type, CarveType::Unknown); CHECK_EQ(blocks[3].length, 20L);
    }
}
