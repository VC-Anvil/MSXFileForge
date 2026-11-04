// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Alonso (GS·RUN). MSXFileForge.
// Free software under the GNU GPL v3 (or later) - see LICENSE. Modified
// versions must be distributed under the same license.
// MSXFileForge CLI - tiny host filesystem helpers (directory listing, mkdir).
// Kept off std::filesystem so the macOS build can still target 10.13.
#pragma once
#include <algorithm>
#include <string>
#include <vector>
#if defined(_WIN32)
#include <direct.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#endif

namespace hostfs {

struct Entry { std::string name; bool isDir; };

// Entries of `dir` (no "." / ".."), sorted by name. Empty on error.
inline std::vector<Entry> list(const std::string& dir) {
    std::vector<Entry> out;
#if defined(_WIN32)
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        std::string n = fd.cFileName;
        if (n == "." || n == "..") continue;
        out.push_back({n, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0});
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR* d = opendir(dir.c_str());
    if (!d) return out;
    while (dirent* de = readdir(d)) {
        std::string n = de->d_name;
        if (n == "." || n == "..") continue;
        struct stat st;
        bool isDir = stat((dir + "/" + n).c_str(), &st) == 0 && S_ISDIR(st.st_mode);
        out.push_back({n, isDir});
    }
    closedir(d);
#endif
    std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) { return a.name < b.name; });
    return out;
}

inline bool isDir(const std::string& path) {
#if defined(_WIN32)
    DWORD a = GetFileAttributesA(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

// Create one directory level; true if it exists afterwards.
inline bool makeDir(const std::string& path) {
#if defined(_WIN32)
    _mkdir(path.c_str());
#else
    mkdir(path.c_str(), 0777);
#endif
    return isDir(path);
}

inline std::string join(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    char last = a.back();
    return (last == '/' || last == '\\') ? a + b : a + "/" + b;
}

} // namespace hostfs
