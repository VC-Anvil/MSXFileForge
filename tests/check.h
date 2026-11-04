// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Alonso (GS·RUN). MSXFileForge.
// Free software under the GNU GPL v3 (or later) - see LICENSE. Modified
// versions must be distributed under the same license.
// MSXFileForge - minimal self-registering test harness (no external deps).
#pragma once
#include <cstdio>
#include <functional>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

namespace msxtest {

struct Case { const char* name; std::function<void()> fn; };
std::vector<Case>& registry();
void fail(const char* file, int line, const std::string& what);

struct Reg { Reg(const char* n, std::function<void()> f) { registry().push_back({n, std::move(f)}); } };

template <class T> std::string show(const T& v) {
    std::ostringstream os;
    if constexpr (std::is_enum_v<T>) os << "enum(" << (long long)v << ")";
    else os << v;
    return os.str();
}
inline std::string show(const std::string& s) { return "\"" + s + "\""; }
inline std::string show(const char* s) { return show(std::string(s)); }
inline std::string show(unsigned char v) { return std::to_string((int)v); }
inline std::string show(bool v) { return v ? "true" : "false"; }

} // namespace msxtest

#define MSXT_CAT2(a, b) a##b
#define MSXT_CAT(a, b) MSXT_CAT2(a, b)
#define TEST(name) \
    static void MSXT_CAT(test_, name)(); \
    static msxtest::Reg MSXT_CAT(reg_, name)(#name, MSXT_CAT(test_, name)); \
    static void MSXT_CAT(test_, name)()

#define CHECK(cond) do { if (!(cond)) msxtest::fail(__FILE__, __LINE__, "CHECK(" #cond ")"); } while (0)
#define CHECK_EQ(a, b) do { auto _va = (a); auto _vb = (b); if (!(_va == _vb)) \
    msxtest::fail(__FILE__, __LINE__, "CHECK_EQ(" #a ", " #b "): " + msxtest::show(_va) + " != " + msxtest::show(_vb)); } while (0)
