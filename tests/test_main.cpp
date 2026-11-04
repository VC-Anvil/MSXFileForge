// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Alonso (GS·RUN). MSXFileForge.
// Free software under the GNU GPL v3 (or later) - see LICENSE. Modified
// versions must be distributed under the same license.
// Test runner: `msxff_tests` runs everything, `msxff_tests <substring>` filters.
#include "check.h"
#include <cstring>

namespace msxtest {
std::vector<Case>& registry() { static std::vector<Case> r; return r; }
static int g_failures = 0;
void fail(const char* file, int line, const std::string& what) {
    fprintf(stderr, "    %s:%d: %s\n", file, line, what.c_str());
    g_failures++;
}
} // namespace msxtest

int main(int argc, char** argv) {
    using namespace msxtest;
    int run = 0, bad = 0;
    for (auto& c : registry()) {
        if (argc > 1 && !strstr(c.name, argv[1])) continue;
        int before = g_failures;
        c.fn();
        run++;
        bool ok = g_failures == before;
        if (!ok) bad++;
        printf("%s %s\n", ok ? "[ ok ]" : "[FAIL]", c.name);
    }
    printf("\n%d test(s), %d failed\n", run, bad);
    return (bad || run == 0) ? 1 : 0;
}
