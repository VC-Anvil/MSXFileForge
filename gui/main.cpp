// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Alonso (GS·RUN). MSXFileForge.
// Free software under the GNU GPL v3 (or later) - see LICENSE. Modified
// versions must be distributed under the same license.
// MSXFileForge - native GUI (Dear ImGui + SDL2 + OpenGL3).
//
// A real editor for MSX media: open a disk image (.dsk / raw / .dmk), cartridge
// ROM, or .cas tape; browse the FAT12 filesystem; EDIT files as hex or as text
// (BASIC is detokenized for editing and re-tokenized on save); save changes back
// into the image and write the image out. Native Open/Save dialogs, an in-app
// manual, and a protected/RAW sector inspector.
#define SDL_MAIN_HANDLED
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <SDL.h>
#include <SDL_opengl.h>
#include <SDL_syswm.h>
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>

#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_opengl3.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../libs/stb_image_write.h"

#include "../core/disk_image.h"
#include "../core/fat12.h"
#include "../core/msxfile.h"
#include "../core/rom.h"
#include "../core/cas.h"

#include <string>
#include <vector>
#include <cstdio>
#include <cstring>

using namespace msx;

enum class Mode { None, Disk, Rom, Cas };

struct App {
    Mode mode = Mode::None;
    std::string imagePath;
    std::string status = "Open an MSX .dsk / .dmk / .rom / .cas  (File > Open, drag & drop, or the path bar).";

    DiskImage disk;
    Fat12 fs;
    bool fsMounted = false;
    std::vector<DirEntry> listing;
    std::string cwd = "/";
    bool showDeleted = false;

    // Selected file + edit buffers
    int selected = -1;
    std::string selName;
    std::vector<uint8_t> selData;     // editable hex buffer
    std::vector<uint8_t> selOrig;     // pristine copy for change highlight
    MsxFileType selType = MsxFileType::Unknown;
    std::string editText;             // editable text / detokenized BASIC
    BloadHeader bload;
    bool hexDirty = false;
    bool textDirty = false;
    int editByte = -1;                // hex cell currently being typed into

    // Rom / Cas
    std::vector<uint8_t> blob;
    RomInfo rom;
    std::vector<CasBlock> cas;

    // RAW carve state
    std::vector<uint8_t> rawCache;
    std::vector<CarvedBlock> carved;
    bool carvedDone = false;

    // Poke control
    char pokeOff[16] = "0";
    char pokeVal[8] = "00";

    char pathInput[1024] = {0};
    bool showManual = false;
    bool showAbout = false;
    HWND hwnd = nullptr;
};

// ---- IO helpers -----------------------------------------------------------
static std::vector<uint8_t> slurp(const std::string& p) {
    std::vector<uint8_t> v;
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return v;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n > 0) { v.resize((size_t)n); if (fread(v.data(),1,(size_t)n,f)!=(size_t)n) v.clear(); }
    fclose(f);
    return v;
}
static bool dump(const std::string& p, const uint8_t* d, size_t n) {
    FILE* f = fopen(p.c_str(), "wb"); if (!f) return false;
    size_t w = fwrite(d, 1, n, f); fclose(f); return w == n;
}
static bool hasExt(const std::string& p, const char* e) {
    size_t n = strlen(e); if (p.size() < n) return false;
    std::string t = p.substr(p.size()-n);
    for (auto& c : t) c = (char)tolower((unsigned char)c);
    return t == e;
}
static void openUrl(const char* url) { ShellExecuteA(nullptr, "open", url, nullptr, nullptr, SW_SHOWNORMAL); }

// Capture the GL framebuffer to a PNG (used for the README screenshot). Done
// from inside the GL context, so it is immune to the black-frame problem GDI
// screen grabs hit over OpenGL/MPO windows.
static void saveScreenshot(SDL_Window* win, const char* path) {
    int w = 0, h = 0; SDL_GL_GetDrawableSize(win, &w, &h);
    if (w <= 0 || h <= 0) return;
    std::vector<uint8_t> px((size_t)w * h * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    std::vector<uint8_t> flip((size_t)w * h * 4);          // OpenGL is bottom-up
    for (int y = 0; y < h; ++y)
        memcpy(&flip[(size_t)(h - 1 - y) * w * 4], &px[(size_t)y * w * 4], (size_t)w * 4);
    stbi_write_png(path, w, h, 4, flip.data(), w * 4);
}

static std::string openDialog(HWND owner, const char* filter) {
    char file[1024] = "";
    OPENFILENAMEA o; memset(&o, 0, sizeof o);
    o.lStructSize = sizeof o; o.hwndOwner = owner;
    o.lpstrFilter = filter; o.lpstrFile = file; o.nMaxFile = sizeof file;
    o.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    return GetOpenFileNameA(&o) ? std::string(file) : std::string();
}
static std::string saveDialog(HWND owner, const char* filter, const char* defExt,
                              const char* suggested = nullptr) {
    char file[1024] = "";
    if (suggested) strncpy(file, suggested, sizeof file - 1);
    OPENFILENAMEA o; memset(&o, 0, sizeof o);
    o.lStructSize = sizeof o; o.hwndOwner = owner;
    o.lpstrFilter = filter; o.lpstrFile = file; o.nMaxFile = sizeof file;
    o.lpstrDefExt = defExt;
    o.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    return GetSaveFileNameA(&o) ? std::string(file) : std::string();
}

// ---- Model ops ------------------------------------------------------------
static void clearSelection(App& a) {
    a.selected = -1; a.selData.clear(); a.selOrig.clear(); a.editText.clear();
    a.selType = MsxFileType::Unknown; a.hexDirty = a.textDirty = false; a.editByte = -1;
}

static void refreshListing(App& a) {
    a.listing = a.fs.list(a.cwd, a.showDeleted);
    clearSelection(a);
}

static void selectFile(App& a, int idx) {
    a.editByte = -1;
    if (idx < 0 || idx >= (int)a.listing.size()) return;
    const DirEntry& e = a.listing[idx];
    a.selected = idx; a.selName = e.name;
    a.hexDirty = a.textDirty = false;
    a.bload = BloadHeader{};
    if (e.isDir()) { a.selData.clear(); a.selOrig.clear(); a.editText.clear(); a.selType = MsxFileType::Unknown; return; }
    a.selData = a.fs.readFile(e);
    a.selOrig = a.selData;
    a.selType = detectType(a.selData);
    if (a.selType == MsxFileType::BasicTokenized)
        a.editText = detokenizeBasic(a.selData);
    else if (a.selType == MsxFileType::BasicAscii || a.selType == MsxFileType::Text)
        a.editText.assign((const char*)a.selData.data(), a.selData.size());
    else a.editText.clear();
    a.editText.reserve(a.editText.size() + 4096);
    if (a.selType == MsxFileType::BloadBinary) a.bload = parseBloadHeader(a.selData);
}

static void openPath(App& a, const std::string& path) {
    a.imagePath = path; a.mode = Mode::None; a.fsMounted = false;
    a.listing.clear(); a.cas.clear(); a.blob.clear(); a.cwd = "/";
    a.carved.clear(); a.rawCache.clear(); a.carvedDone = false;
    clearSelection(a);
    strncpy(a.pathInput, path.c_str(), sizeof(a.pathInput)-1);

    if (hasExt(path, ".rom") || hasExt(path, ".mx1") || hasExt(path, ".mx2")) {
        a.blob = slurp(path);
        if (a.blob.empty()) { a.status = "Cannot read ROM: " + path; return; }
        a.rom = analyzeRom(a.blob); a.mode = Mode::Rom;
        a.status = "ROM loaded (" + std::to_string(a.blob.size()) + " bytes)"; return;
    }
    if (hasExt(path, ".cas")) {
        a.blob = slurp(path);
        if (a.blob.empty()) { a.status = "Cannot read CAS: " + path; return; }
        a.cas = parseCas(a.blob); a.mode = Mode::Cas;
        a.status = "CAS loaded (" + std::to_string(a.cas.size()) + " blocks)"; return;
    }
    std::string err;
    if (!a.disk.load(path, &err)) { a.status = "Load error: " + err; return; }
    a.mode = Mode::Disk;
    a.fsMounted = a.fs.mount(a.disk, &err);
    if (a.fsMounted) refreshListing(a); else a.listing.clear();
    a.status = std::string("Loaded ") + to_string(a.disk.format()) +
               (a.disk.isProtected() ? "  [PROTECTED]" : "") +
               (a.fsMounted ? "  |  FAT12 OK" : ("  |  no FS: " + err));
}

// Write the current file's edited bytes back into the image (in memory).
static void saveFileToImage(App& a, const std::vector<uint8_t>& bytes) {
    if (a.selected < 0) { a.status = "No file selected."; return; }
    std::string name = a.listing[a.selected].name;
    std::string err;
    // Re-fetch a fresh mutable entry (writeFile updates it in place).
    if (!a.fs.writeFile(a.listing[a.selected], bytes, &err)) {
        a.status = "Write failed: " + err; return;
    }
    a.status = "Saved '" + name + "' into image (" + std::to_string(bytes.size()) +
               " bytes). Image modified - use File > Save image.";
    // Refresh and reselect by name.
    refreshListing(a);
    for (int i = 0; i < (int)a.listing.size(); ++i)
        if (a.listing[i].name == name) { selectFile(a, i); break; }
}

static void saveImageAs(App& a) {
    std::string p = saveDialog(a.hwnd,
        "MSX disk image\0*.dsk\0DMK track image\0*.dmk\0All files\0*.*\0",
        "dsk", a.imagePath.c_str());
    if (p.empty()) return;
    std::string err;
    if (a.disk.save(p, &err)) { a.disk.clearDirty(); a.imagePath = p; a.status = "Image saved: " + p; }
    else a.status = "Save failed: " + err;
}
static void saveImage(App& a) {
    if (a.imagePath.empty()) { saveImageAs(a); return; }
    std::string err;
    if (a.disk.save(a.imagePath, &err)) { a.disk.clearDirty(); a.status = "Image saved: " + a.imagePath; }
    else a.status = "Save failed: " + err;
}

// ---- Theme ----------------------------------------------------------------
static void applyTheme() {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowPadding = ImVec2(12, 12);
    s.FramePadding  = ImVec2(9, 5);
    s.ItemSpacing   = ImVec2(9, 7);
    s.ItemInnerSpacing = ImVec2(7, 5);
    s.CellPadding   = ImVec2(8, 4);
    s.WindowRounding = 6; s.FrameRounding = 5; s.GrabRounding = 5;
    s.ChildRounding = 5; s.PopupRounding = 5; s.TabRounding = 5;
    s.ScrollbarRounding = 8; s.WindowBorderSize = 1; s.FrameBorderSize = 0;
    ImVec4* c = s.Colors;
    ImVec4 accent(0.36f, 0.72f, 0.92f, 1.0f);   // cyan-blue accent
    ImVec4 accentDim(0.20f, 0.42f, 0.56f, 1.0f);
    c[ImGuiCol_WindowBg]        = ImVec4(0.10f,0.11f,0.13f,1.0f);
    c[ImGuiCol_ChildBg]         = ImVec4(0.12f,0.13f,0.16f,1.0f);
    c[ImGuiCol_PopupBg]         = ImVec4(0.10f,0.11f,0.13f,0.98f);
    c[ImGuiCol_Border]          = ImVec4(0.24f,0.27f,0.32f,0.6f);
    c[ImGuiCol_FrameBg]         = ImVec4(0.16f,0.18f,0.22f,1.0f);
    c[ImGuiCol_FrameBgHovered]  = accentDim;
    c[ImGuiCol_FrameBgActive]   = accent;
    c[ImGuiCol_TitleBgActive]   = accentDim;
    c[ImGuiCol_MenuBarBg]       = ImVec4(0.13f,0.15f,0.19f,1.0f);
    c[ImGuiCol_Header]          = accentDim;
    c[ImGuiCol_HeaderHovered]   = ImVec4(0.28f,0.55f,0.72f,1.0f);
    c[ImGuiCol_HeaderActive]    = accent;
    c[ImGuiCol_Button]          = ImVec4(0.20f,0.32f,0.42f,1.0f);
    c[ImGuiCol_ButtonHovered]   = accentDim;
    c[ImGuiCol_ButtonActive]    = accent;
    c[ImGuiCol_Tab]             = ImVec4(0.15f,0.18f,0.22f,1.0f);
    c[ImGuiCol_TabHovered]      = accentDim;
    c[ImGuiCol_TabActive]       = accentDim;
    c[ImGuiCol_CheckMark]       = accent;
    c[ImGuiCol_SliderGrab]      = accent;
    c[ImGuiCol_TableHeaderBg]   = ImVec4(0.18f,0.22f,0.28f,1.0f);
    c[ImGuiCol_TableRowBg]      = ImVec4(0.12f,0.13f,0.16f,1.0f);
    c[ImGuiCol_TableRowBgAlt]   = ImVec4(0.145f,0.16f,0.19f,1.0f);
    c[ImGuiCol_TableBorderLight]= ImVec4(0.24f,0.27f,0.32f,0.5f);
    c[ImGuiCol_Text]            = ImVec4(0.90f,0.92f,0.95f,1.0f);
    c[ImGuiCol_TextDisabled]    = ImVec4(0.52f,0.56f,0.62f,1.0f);
}

static bool g_forceRawTab = false; // screenshot mode: open the RAW data/map tab
static const ImVec4 kAccent(0.42f,0.80f,1.0f,1.0f);
static const ImVec4 kDir(0.98f,0.80f,0.35f,1.0f);
static const ImVec4 kWarn(1.0f,0.62f,0.25f,1.0f);
static const ImVec4 kDel(1.0f,0.42f,0.42f,1.0f);
static const ImVec4 kMod(1.0f,0.85f,0.30f,1.0f);
static const ImVec4 kGreen(0.5f,0.85f,0.45f,1.0f);

static void help(const char* txt) {
    ImGui::SameLine(); ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(360);
        ImGui::TextUnformatted(txt);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// ---- Hex editor -----------------------------------------------------------
static void drawHexEditor(App& a, std::vector<uint8_t>& d, const std::vector<uint8_t>& orig,
                          bool editable, const char* id, bool* dirty) {
    // Poke row (robust explicit editing).
    if (editable) {
        ImGui::SetNextItemWidth(120);
        ImGui::InputText("offset(hex)", a.pokeOff, sizeof a.pokeOff, ImGuiInputTextFlags_CharsHexadecimal);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70);
        ImGui::InputText("val", a.pokeVal, sizeof a.pokeVal, ImGuiInputTextFlags_CharsHexadecimal);
        ImGui::SameLine();
        if (ImGui::Button("Set byte")) {
            long off = strtol(a.pokeOff, nullptr, 16);
            int val = (int)strtol(a.pokeVal, nullptr, 16) & 0xFF;
            if (off >= 0 && off < (long)d.size()) { d[off] = (uint8_t)val; *dirty = true; }
        }
        help("Type an offset and a byte value in hex, then Set byte. Or click any byte in the grid to edit it inline. Modified bytes turn yellow.");
        ImGui::SameLine(); ImGui::TextDisabled("| click a byte to edit inline");
    }
    ImGui::BeginChild(id, ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 2));
    float charW = ImGui::CalcTextSize("F").x;
    ImGuiListClipper clip;
    int rows = (int)((d.size() + 15) / 16);
    clip.Begin(rows);
    while (clip.Step()) {
        for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
            size_t base = (size_t)r * 16;
            ImGui::TextDisabled("%06zX", base);
            ImGui::SameLine(0, charW);
            for (int col = 0; col < 16; ++col) {
                size_t idx = base + col;
                ImGui::SameLine(0, col == 8 ? charW * 1.4f : charW * 0.6f);
                if (idx >= d.size()) { ImGui::TextDisabled("  "); continue; }
                ImGui::PushID((int)idx);
                if (editable && a.editByte == (int)idx) {
                    char buf[4]; snprintf(buf, sizeof buf, "%02X", d[idx]);
                    ImGui::SetNextItemWidth(charW * 2.6f);
                    ImGui::SetKeyboardFocusHere();
                    if (ImGui::InputText("##e", buf, sizeof buf,
                            ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_CharsUppercase |
                            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
                        d[idx] = (uint8_t)(strtol(buf, nullptr, 16) & 0xFF);
                        *dirty = true; a.editByte = -1;
                    }
                    if (ImGui::IsItemDeactivated()) {
                        d[idx] = (uint8_t)(strtol(buf, nullptr, 16) & 0xFF);
                        *dirty = true; a.editByte = -1;
                    }
                } else {
                    bool changed = idx < orig.size() && d[idx] != orig[idx];
                    if (changed) ImGui::PushStyleColor(ImGuiCol_Text, kMod);
                    char cell[4]; snprintf(cell, sizeof cell, "%02X", d[idx]);
                    ImGui::TextUnformatted(cell);
                    if (changed) ImGui::PopStyleColor();
                    if (editable && ImGui::IsItemClicked()) a.editByte = (int)idx;
                }
                ImGui::PopID();
            }
            // ASCII column
            ImGui::SameLine(0, charW * 1.6f);
            std::string asc;
            for (int col = 0; col < 16 && base + col < d.size(); ++col) {
                uint8_t ch = d[base + col];
                asc += (ch >= 0x20 && ch < 0x7F) ? (char)ch : '.';
            }
            ImGui::TextDisabled("%s", asc.c_str());
        }
    }
    ImGui::PopStyleVar();
    ImGui::EndChild();
}

// ---- Text resize callback for std::string editor --------------------------
static int textResize(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        std::string* s = (std::string*)data->UserData;
        s->resize(data->BufTextLen);
        data->Buf = (char*)s->c_str();
    }
    return 0;
}

// ---- RAW / cluster map ----------------------------------------------------
static void drawRawMap(App& a) {
    if (!a.fsMounted) {
        ImGui::TextDisabled("No FAT12 filesystem - the whole container is raw. Use the 'Raw image' tab.");
        return;
    }
    auto map = a.fs.clusterMap();
    long cb = (long)a.fs.sectorsPerCluster() * 512;
    long freeN = 0, named = 0, raw = 0;
    for (auto s : map) { if (s == Fat12::CL_FREE) freeN++; else if (s == Fat12::CL_NAMED) named++; else raw++; }

    ImGui::TextWrapped("Some MSX games (Fray, Xak, ...) store most of their data in RAW sectors that are "
        "allocated but have NO directory entry - a custom loader reads them by sector, not by name. "
        "This is NOT encryption: the data below is fully readable. Named files appear in the tree; the "
        "RAW region is shown here and can be exported.");
    ImGui::Spacing();
    ImGui::Text("Named files:"); ImGui::SameLine(); ImGui::TextColored(kGreen, "%ld KB", named*cb/1024);
    ImGui::SameLine(); ImGui::Text("   RAW unnamed:"); ImGui::SameLine();
    ImGui::TextColored(kWarn, "%ld KB", raw*cb/1024);
    ImGui::SameLine(); ImGui::Text("   Free:"); ImGui::SameLine();
    ImGui::TextDisabled("%ld KB", freeN*cb/1024);

    if (ImGui::Button("Export RAW data (unnamed)...")) {
        auto d = a.fs.extractRawUnnamed();
        std::string p = saveDialog(a.hwnd, "Binary\0*.bin\0All files\0*.*\0", "bin", "raw_data.bin");
        if (!p.empty()) { dump(p, d.data(), d.size());
            a.status = "Exported " + std::to_string(d.size()) + " bytes of RAW data -> " + p; }
    }
    help("Concatenate every allocated-but-unnamed cluster (the uncatalogued game data) and save it to your PC.");
    ImGui::SameLine();
    if (ImGui::Button("Import RAW data (replace)...")) {
        std::string p = openDialog(a.hwnd, "Binary\0*.bin\0All files\0*.*\0");
        if (!p.empty()) {
            auto d = slurp(p);
            long wrote = 0; std::string err;
            bool ok = a.fs.writeRawUnnamed(d, &wrote, &err);
            a.carvedDone = false; a.carved.clear(); a.rawCache.clear();
            a.status = ok ? ("Imported " + std::to_string(wrote) + " bytes into RAW region (image modified)")
                          : ("Imported " + std::to_string(wrote) + " bytes - " + err);
        }
    }
    help("Write a previously-exported (and possibly edited) RAW blob back into the same unnamed clusters. "
         "Same-size data restores byte-for-byte; larger data is truncated to the RAW capacity.");
    ImGui::SameLine();
    if (ImGui::Button("Export whole image...")) {
        std::string p = saveDialog(a.hwnd, "All files\0*.*\0", "", a.imagePath.c_str());
        if (!p.empty()) { dump(p, a.disk.raw().data(), a.disk.raw().size());
            a.status = "Exported whole image -> " + p; }
    }
    ImGui::Separator();

    // Block carving: split the RAW region into BLOAD / BASIC / gaps.
    if (ImGui::Button(a.carvedDone ? "Re-scan blocks" : "Carve into blocks")) {
        a.rawCache = a.fs.extractRawUnnamed();
        a.carved = carveBlocks(a.rawCache);
        a.carvedDone = true;
    }
    help("Scan the RAW region for recognizable segments: BLOAD images (0xFE header), tokenized BASIC "
         "programs (0xFF), and the data gaps between them. Each block can be exported on its own.");
    if (a.carvedDone) {
        int nB=0,nb=0,nu=0;
        for (auto& b : a.carved) { if (b.type==CarveType::Bload) nb++; else if (b.type==CarveType::BasicTokenized) nB++; else nu++; }
        ImGui::SameLine(); ImGui::TextDisabled("%d BLOAD, %d BASIC, %d gaps", nb, nB, nu);
        ImGui::BeginChild("blocks", ImVec2(0, 200), true);
        if (ImGui::BeginTable("carve", 5, ImGuiTableFlags_RowBg|ImGuiTableFlags_BordersInnerH|ImGuiTableFlags_ScrollY)) {
            ImGui::TableSetupColumn("Offset", ImGuiTableColumnFlags_WidthFixed, 84);
            ImGui::TableSetupColumn("Type",   ImGuiTableColumnFlags_WidthFixed, 130);
            ImGui::TableSetupColumn("Size",   ImGuiTableColumnFlags_WidthFixed, 78);
            ImGui::TableSetupColumn("BLOAD addr", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 70);
            ImGui::TableHeadersRow();
            for (int i = 0; i < (int)a.carved.size(); ++i) {
                const CarvedBlock& b = a.carved[i];
                if (b.type == CarveType::Unknown && b.length < 512) continue;
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("0x%06lX", b.offset);
                ImGui::TableNextColumn();
                ImVec4 tc = b.type==CarveType::Bload ? kAccent : b.type==CarveType::BasicTokenized ? kGreen : ImGui::GetStyle().Colors[ImGuiCol_TextDisabled];
                ImGui::TextColored(tc, "%s", carveTypeName(b.type));
                ImGui::TableNextColumn(); ImGui::Text("%ld", b.length);
                ImGui::TableNextColumn();
                if (b.type==CarveType::Bload) ImGui::TextDisabled("%04X-%04X exec %04X", b.start, b.end, b.exec);
                else ImGui::TextDisabled("-");
                ImGui::TableNextColumn();
                ImGui::PushID(i);
                if (ImGui::SmallButton("Export")) {
                    const char* ext = b.type==CarveType::Bload ? "bin" : b.type==CarveType::BasicTokenized ? "bas" : "bin";
                    char sug[64]; snprintf(sug, sizeof sug, "block_%06lX.%s", b.offset, ext);
                    std::string p = saveDialog(a.hwnd, "All files\0*.*\0", ext, sug);
                    if (!p.empty() && b.offset + b.length <= (long)a.rawCache.size())
                        dump(p, a.rawCache.data() + b.offset, b.length);
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::EndChild();
    }
    ImGui::Separator();

    // Legend
    ImGui::ColorButton("##g", ImVec4(0.30f,0.70f,0.35f,1), ImGuiColorEditFlags_NoTooltip, ImVec2(14,14));
    ImGui::SameLine(); ImGui::TextUnformatted("named"); ImGui::SameLine(0, 18);
    ImGui::ColorButton("##o", ImVec4(0.95f,0.60f,0.20f,1), ImGuiColorEditFlags_NoTooltip, ImVec2(14,14));
    ImGui::SameLine(); ImGui::TextUnformatted("RAW (unnamed)"); ImGui::SameLine(0, 18);
    ImGui::ColorButton("##f", ImVec4(0.22f,0.24f,0.28f,1), ImGuiColorEditFlags_NoTooltip, ImVec2(14,14));
    ImGui::SameLine(); ImGui::TextUnformatted("free");

    // Heatmap (columns adapt to the available width)
    int n = (int)map.size();
    const float cell = 12.0f, pad = 1.5f;
    int cols = (int)(ImGui::GetContentRegionAvail().x / cell);
    if (cols < 8) cols = 8;
    int rows = (n + cols - 1) / cols;
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::InvisibleButton("map", ImVec2(cols * cell, rows * cell));
    for (int i = 0; i < n; ++i) {
        int r = i / cols, c = i % cols;
        ImVec2 p0(origin.x + c * cell, origin.y + r * cell);
        ImVec2 p1(p0.x + cell - pad, p0.y + cell - pad);
        ImU32 col = map[i] == Fat12::CL_NAMED ? IM_COL32(76,178,89,255)
                  : map[i] == Fat12::CL_RAW   ? IM_COL32(242,153,51,255)
                                              : IM_COL32(56,61,71,255);
        dl->AddRectFilled(p0, p1, col, 2.0f);
    }
    if (ImGui::IsItemHovered()) {
        ImVec2 m = ImGui::GetMousePos();
        int c = (int)((m.x - origin.x) / cell), r = (int)((m.y - origin.y) / cell);
        int idx = r * cols + c;
        if (c >= 0 && c < cols && idx >= 0 && idx < n) {
            const char* st = map[idx] == Fat12::CL_NAMED ? "named file"
                           : map[idx] == Fat12::CL_RAW ? "RAW (unnamed)" : "free";
            ImGui::BeginTooltip();
            ImGui::Text("cluster %d  (%s)", idx + 2, st);
            ImGui::Text("image offset 0x%06lX", a.fs.clusterByteOffset(idx + 2));
            ImGui::EndTooltip();
        }
    }
}

// ---- Panels ---------------------------------------------------------------
static void drawFileTree(App& a) {
    if (ImGui::Checkbox("Show deleted", &a.showDeleted)) refreshListing(a);
    help("Also list directory entries whose first byte is 0xE5 (deleted files). "
         "Their data may still be recoverable if the clusters were not reused.");
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) refreshListing(a);
    ImGui::Separator();
    if (!a.fsMounted) { ImGui::TextDisabled("No FAT12 filesystem on this image."); return; }

    ImGui::TextColored(kAccent, "%s", a.cwd.c_str());
    if (a.cwd != "/") {
        if (ImGui::Selectable(".. (up)")) {
            size_t slash = a.cwd.find_last_of('/');
            a.cwd = (slash == 0 || slash == std::string::npos) ? "/" : a.cwd.substr(0, slash);
            refreshListing(a);
        }
    }
    if (ImGui::BeginTable("files", 3,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Attr", ImGuiTableColumnFlags_WidthFixed, 62);
        ImGui::TableHeadersRow();
        for (int i = 0; i < (int)a.listing.size(); ++i) {
            const DirEntry& e = a.listing[i];
            if (e.isVolume()) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImVec4 col = e.deleted ? kDel : (e.isDir() ? kDir : ImGui::GetStyle().Colors[ImGuiCol_Text]);
            ImGui::PushStyleColor(ImGuiCol_Text, col);
            char label[80];
            snprintf(label, sizeof label, "%s%s", e.isDir() ? "[D] " : "    ", e.name.c_str());
            bool sel = (i == a.selected);
            if (ImGui::Selectable(label, sel, ImGuiSelectableFlags_SpanAllColumns)) {
                if (e.isDir() && e.name != "." && e.name != "..") {
                    a.cwd = (a.cwd == "/" ? "/" : a.cwd + "/") + e.name;
                    refreshListing(a);
                } else selectFile(a, i);
            }
            ImGui::PopStyleColor();
            ImGui::TableNextColumn();
            if (!e.isDir()) ImGui::Text("%u", e.size); else ImGui::TextDisabled("<dir>");
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", e.attrString().c_str());
        }
        ImGui::EndTable();
    }
}

static void drawFileToolbar(App& a) {
    const DirEntry& e = a.listing[a.selected];
    ImGui::TextColored(kAccent, "%s", a.selName.c_str());
    ImGui::SameLine(); ImGui::TextDisabled("(%zu bytes, %s)", a.selData.size(), typeName(a.selType));
    if (a.hexDirty || a.textDirty) { ImGui::SameLine(); ImGui::TextColored(kMod, "  * unsaved edits"); }
    ImGui::Separator();
    if (ImGui::Button("Export to file...")) {
        std::string p = saveDialog(a.hwnd, "All files\0*.*\0", "", e.name.c_str());
        if (!p.empty()) { dump(p, a.selData.data(), a.selData.size());
            a.status = "Exported " + a.selName + " -> " + p; }
    }
    help("Write the selected file's bytes out to your PC as a normal file.");
    ImGui::SameLine();
    if (ImGui::Button("Import & replace...")) {
        std::string p = openDialog(a.hwnd, "All files\0*.*\0");
        if (!p.empty()) { auto v = slurp(p); saveFileToImage(a, v); }
    }
    help("Replace this file's contents with a file from your PC (grows/shrinks the cluster chain automatically).");
    ImGui::SameLine();
    if (ImGui::Button("Delete file")) {
        std::string err, name = a.selName;
        if (a.fs.deleteFile(e, &err)) { a.status = "Deleted " + name + " (image modified)"; refreshListing(a); }
        else a.status = "Delete failed: " + err;
    }
    help("Mark the file deleted and free its clusters. Save the image to persist.");
}

static void drawPreview(App& a) {
    if (a.selected < 0) { ImGui::TextDisabled("Select a file on the left to view or edit it."); }
    else if (!a.listing[a.selected].isDir()) drawFileToolbar(a);

    if (ImGui::BeginTabBar("preview")) {
        bool haveFile = a.selected >= 0 && !a.listing[a.selected].isDir();
        if (haveFile && (a.selType == MsxFileType::BasicTokenized ||
                         a.selType == MsxFileType::BasicAscii ||
                         a.selType == MsxFileType::Text)) {
            const char* tabName = (a.selType == MsxFileType::BasicTokenized) ? "BASIC (edit)" : "Text (edit)";
            if (ImGui::BeginTabItem(tabName)) {
                ImGui::TextDisabled(a.selType == MsxFileType::BasicTokenized ?
                    "Edit the listing; Save re-tokenizes it back into the file." :
                    "Edit the text; Save writes it back as-is.");
                if (ImGui::Button("Save to image")) {
                    std::vector<uint8_t> bytes;
                    if (a.selType == MsxFileType::BasicTokenized)
                        bytes = tokenizeBasic(a.editText);
                    else bytes.assign(a.editText.begin(), a.editText.end());
                    saveFileToImage(a, bytes);
                    a.textDirty = false;
                }
                help("BASIC is re-tokenized (text -> tokens) then written into the image. Save the image afterwards to persist to disk.");
                ImGui::SameLine();
                if (ImGui::Button("Revert")) selectFile(a, a.selected);
                if (a.textDirty) { ImGui::SameLine(); ImGui::TextColored(kMod, "* edited"); }
                ImVec2 sz = ImGui::GetContentRegionAvail();
                if (a.editText.capacity() < a.editText.size() + 256)
                    a.editText.reserve(a.editText.size() + 4096);
                if (ImGui::InputTextMultiline("##text", (char*)a.editText.c_str(),
                        a.editText.capacity() + 1, sz,
                        ImGuiInputTextFlags_CallbackResize | ImGuiInputTextFlags_AllowTabInput,
                        textResize, &a.editText))
                    a.textDirty = true;
                ImGui::EndTabItem();
            }
        }
        if (haveFile && a.selType == MsxFileType::BloadBinary && ImGui::BeginTabItem("BLOAD")) {
            ImGui::Text("start address : 0x%04X", a.bload.start);
            ImGui::Text("end address   : 0x%04X", a.bload.end);
            ImGui::Text("exec address  : 0x%04X", a.bload.exec);
            ImGui::Text("payload size  : %u bytes", a.bload.end - a.bload.start + 1);
            ImGui::TextDisabled("Edit the raw bytes in the Hex tab.");
            ImGui::EndTabItem();
        }
        if (haveFile && ImGui::BeginTabItem("Hex (edit)")) {
            if (ImGui::Button("Save to image")) { saveFileToImage(a, a.selData); a.hexDirty = false; }
            help("Write the edited bytes back into the file (same length). Then File > Save image to persist.");
            ImGui::SameLine();
            if (ImGui::Button("Revert")) selectFile(a, a.selected);
            if (a.hexDirty) { ImGui::SameLine(); ImGui::TextColored(kMod, "* edited"); }
            drawHexEditor(a, a.selData, a.selOrig, true, "filehex", &a.hexDirty);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Raw image")) {
            ImGui::TextDisabled("Whole container (%zu bytes). Editable; touches the image directly.",
                                a.disk.raw().size());
            bool edited = false;
            drawHexEditor(a, a.disk.rawMut(), a.disk.raw(), true, "rawhex", &edited);
            if (edited) a.disk.markDirty();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("RAW data / map", nullptr,
                                g_forceRawTab ? ImGuiTabItemFlags_SetSelected : 0)) {
            drawRawMap(a);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Protection / sectors")) {
            ImGui::Text("Physical sectors: %zu", a.disk.sectors().size());
            ImGui::SameLine();
            ImGui::TextColored(a.disk.isProtected() ? kWarn : kGreen,
                               a.disk.isProtected() ? " PROTECTED" : " standard layout");
            if (a.disk.isProtected())
                ImGui::TextColored(kWarn, "%s", a.disk.protectionReport().c_str());
            ImGui::Separator();
            if (ImGui::BeginTable("sectors", 6, ImGuiTableFlags_RowBg|ImGuiTableFlags_Borders|
                                  ImGuiTableFlags_ScrollY)) {
                ImGui::TableSetupColumn("Cyl"); ImGui::TableSetupColumn("Head");
                ImGui::TableSetupColumn("Sec"); ImGui::TableSetupColumn("Size");
                ImGui::TableSetupColumn("LBA"); ImGui::TableSetupColumn("Flags");
                ImGui::TableHeadersRow();
                for (auto& s : a.disk.sectors()) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::Text("%d", s.cyl);
                    ImGui::TableNextColumn(); ImGui::Text("%d", s.head);
                    ImGui::TableNextColumn(); ImGui::Text("%d", s.sec);
                    ImGui::TableNextColumn();
                    if (s.size != 512) ImGui::TextColored(kWarn, "%zu", s.size); else ImGui::Text("%zu", s.size);
                    ImGui::TableNextColumn(); ImGui::Text("%ld", s.lba);
                    ImGui::TableNextColumn();
                    std::string f;
                    if (s.nonStandard) f += "NONSTD ";
                    if (s.deleted) f += "DELETED ";
                    if (s.idCrcError) f += "IDCRC ";
                    if (s.dataCrcError) f += "DATACRC ";
                    if (f.empty()) ImGui::TextDisabled("-"); else ImGui::TextColored(kWarn, "%s", f.c_str());
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

static void drawRom(App& a) {
    ImGui::TextColored(kAccent, "ROM  %s", a.imagePath.c_str());
    ImGui::Separator();
    ImGui::Text("Size    : %ld bytes  (%d x 16KB pages)", a.rom.size, a.rom.pages16k);
    ImGui::Text("Header  : 'AB' @0x0000 %s   @0x4000 %s",
                a.rom.hasHeaderAt0?"yes":"no", a.rom.hasHeaderAt4000?"yes":"no");
    if (a.rom.hasHeaderAt0 || a.rom.hasHeaderAt4000)
        ImGui::Text("Entries : INIT=%04X STATEMENT=%04X DEVICE=%04X TEXT=%04X",
                    a.rom.init, a.rom.statement, a.rom.device, a.rom.text);
    ImGui::TextColored(kGreen, "Mapper  : %s", mapperName(a.rom.mapper));
    ImGui::Separator();
    static bool dummy = false;
    drawHexEditor(a, a.blob, a.blob, false, "romhex", &dummy);
}

static void drawCas(App& a) {
    ImGui::TextColored(kAccent, "CAS  %s  (%zu blocks)", a.imagePath.c_str(), a.cas.size());
    ImGui::Separator();
    if (ImGui::BeginTable("cas", 4, ImGuiTableFlags_RowBg|ImGuiTableFlags_Borders)) {
        ImGui::TableSetupColumn("Offset"); ImGui::TableSetupColumn("Kind");
        ImGui::TableSetupColumn("Name"); ImGui::TableSetupColumn("Length");
        ImGui::TableHeadersRow();
        for (auto& b : a.cas) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("0x%06lX", b.offset);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(casKindName(b.kind));
            ImGui::TableNextColumn(); ImGui::TextColored(kAccent, "%s", b.filename.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%ld", b.dataLength);
        }
        ImGui::EndTable();
    }
}

static void drawManual(App& a) {
    if (!a.showManual) return;
    ImGui::SetNextWindowSize(ImVec2(680, 620), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Manual / Help", &a.showManual)) {
        ImGui::TextColored(kAccent, "MSXFileForge - what everything does");
        ImGui::Separator();
        // Visual section header: an accent chip with the title inside.
        auto H = [](const char* t){
            ImGui::Spacing();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 p = ImGui::GetCursorScreenPos();
            float h = ImGui::GetTextLineHeight() + 6;
            dl->AddRectFilled(p, ImVec2(p.x + 6, p.y + h), IM_COL32(90,180,235,255), 2);
            dl->AddRectFilled(ImVec2(p.x, p.y), ImVec2(p.x + ImGui::GetContentRegionAvail().x, p.y + h),
                              IM_COL32(90,180,235,28), 3);
            ImGui::Dummy(ImVec2(10, 0)); ImGui::SameLine();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 3);
            ImGui::TextColored(kDir, "%s", t);
            ImGui::Dummy(ImVec2(0, 3));
        };
        auto P = [](const char* t){ ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(t); ImGui::PopTextWrapPos(); };

        H("Opening media");
        P("File > Open (Ctrl+O), drag a file onto the window, or paste a path in the top bar and press Open. "
          "Supported: disk images .dsk / raw / .dmk, cartridge .rom (.mx1/.mx2), and .cas tapes.");

        H("Browsing a disk");
        P("The left panel lists the FAT12 filesystem. Directories are yellow and marked [D]; deleted "
          "entries are red (enable 'Show deleted'). Click a directory to enter it, '.. (up)' to go back, "
          "and a file to open it on the right.");

        H("Editing a file");
        P("- BASIC (edit): tokenized BASIC is shown as a normal listing you can edit. 'Save to image' "
          "re-tokenizes it and writes it back.\n"
          "- Text (edit): ASCII files are edited as plain text.\n"
          "- Hex (edit): click any byte to type a new value, or use the offset/value 'Set byte' box. "
          "Changed bytes are highlighted yellow. 'Save to image' writes the bytes back.\n"
          "Growing or shrinking a file automatically re-allocates FAT clusters.");

        H("Import / Export / Delete");
        P("Export writes the selected file to your PC. Import & replace swaps its contents with a PC file. "
          "Delete frees the file's clusters and marks the entry deleted.");

        H("Saving");
        P("Edits live in an in-memory copy of the image until you choose File > Save image (Ctrl+S) or "
          "Save image as (Ctrl+Shift+S). The status bar shows when the image has unsaved changes.");

        H("Protected / RAW disks");
        P("Some games (Fray, Xak, ...) show only 1-2 files in the tree but the disk is nearly full. The "
          "bulk is RAW data: sectors that are allocated but have NO directory entry, read directly by the "
          "game's loader. This is NOT encryption - the data is fully readable.\n"
          "In the 'RAW data / map' tab you can:\n"
          "  - see a colour map (green = named, orange = RAW, grey = free);\n"
          "  - Export the RAW region, or Export the whole image;\n"
          "  - Import RAW data back in (edit-and-replace round-trips byte-for-byte);\n"
          "  - Carve the RAW region into recognizable blocks (BLOAD images, BASIC programs, gaps) and "
          "export any block on its own.\n"
          "For .dmk track images, the 'Protection / sectors' tab shows the physical layout: non-standard "
          "sizes, deleted-data marks and CRC anomalies flagged in orange.");

        H("ROM / CAS");
        P("ROMs show size, page count, the AB header entry points and a detected mapper "
          "(Konami / SCC / ASCII8 / ASCII16). CAS files show their block layout and file names.");

        H("Sources & references");
        struct Src { const char* label; const char* url; };
        static const Src srcs[] = {
            {"Generation MSX - Fray in Magical Adventure", "https://www.generation-msx.nl/software/microcabin/fray---in-magical-adventure/release/1838/"},
            {"MSX.org - Disk copy protections", "https://www.msx.org/forum/msx-talk/general-discussion/disk-copy-protections"},
            {"The Cutting Room Floor - Fray (MSX2)", "https://tcrf.net/Fray:_In_Magical_Adventure_(MSX2)"},
            {"MSX Wiki - MSX-BASIC file formats", "https://www.msx.org/wiki/MSX-BASIC_file_formats"},
        };
        for (auto& s : srcs) {
            ImGui::Bullet();
            ImGui::TextColored(kAccent, "%s", s.label);
            if (ImGui::IsItemHovered()) { ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                ImGui::SetTooltip("%s\n(click to open in browser)", s.url); }
            if (ImGui::IsItemClicked()) openUrl(s.url);
        }
        ImGui::Spacing();
        ImGui::TextDisabled("Tip: hover any (?) marker for context help.");
    }
    ImGui::End();
}

static void drawAbout(App& a) {
    if (!a.showAbout) return;
    ImGui::SetNextWindowSize(ImVec2(420, 220), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("About MSXFileForge", &a.showAbout)) {
        ImGui::TextColored(kAccent, "MSXFileForge  v0.4");
        ImGui::TextWrapped("A native editor/inspector for MSX disk images, files, ROMs and tapes, "
                           "with protected/RAW (.dmk) support and a raw-sector browser/carver.");
        ImGui::Separator();
        ImGui::TextDisabled("Author");
        ImGui::Text("Alonso");
        ImGui::TextColored(kAccent, "GS\xC2\xB7RUN");
        ImGui::Spacing();
        ImGui::TextDisabled("Built with Claude Code (Opus 4.8).  License: see repository.");
        ImGui::TextDisabled("Bundles Dear ImGui (MIT) and SDL2 (zlib).");
    }
    ImGui::End();
}

int main(int argc, char** argv) {
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "SDL init: %s\n", SDL_GetError()); return 1; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_Window* win = SDL_CreateWindow("MSXFileForge",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 820,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    SDL_GLContext gl = SDL_GL_CreateContext(win);
    SDL_GL_MakeCurrent(win, gl);
    SDL_GL_SetSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    applyTheme();
    ImGui_ImplSDL2_InitForOpenGL(win, gl);
    ImGui_ImplOpenGL3_Init("#version 130");

    App app;
    SDL_SysWMinfo wm; SDL_VERSION(&wm.version);
    if (SDL_GetWindowWMInfo(win, &wm)) app.hwnd = wm.info.win.window;
    if (argc > 1 && argv[1][0]) openPath(app, argv[1]);

    // Optional one-shot screenshot mode (for the README): MSXFF_SHOT=<png path>.
    const char* shotPath = getenv("MSXFF_SHOT");
    const char* shotTab = getenv("MSXFF_TAB");
    const char* shotName = getenv("MSXFF_NAME"); // display name for the path bar
    if (shotPath && shotName) { strncpy(app.pathInput, shotName, sizeof(app.pathInput)-1);
        app.pathInput[sizeof(app.pathInput)-1] = 0; }
    g_forceRawTab = shotTab && strstr(shotTab, "raw");
    int frame = 0; const int shotAt = 45;
    if (shotPath && !g_forceRawTab && app.mode == Mode::Disk && app.fsMounted) {
        int pick = -1;
        for (int i = 0; i < (int)app.listing.size(); ++i) {
            const DirEntry& e = app.listing[i];
            if (e.isDir() || e.isVolume()) continue;
            if (pick < 0) pick = i;
            if (e.name.size() >= 4 && e.name.substr(e.name.size()-4) == ".BAS") { pick = i; break; }
        }
        if (pick >= 0) selectFile(app, pick);
    }

    bool running = true;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL2_ProcessEvent(&ev);
            if (ev.type == SDL_QUIT) running = false;
            if (ev.type == SDL_DROPFILE) { std::string p = ev.drop.file; SDL_free(ev.drop.file); openPath(app, p); }
        }
        ImGuiIO& io = ImGui::GetIO();
        // Shortcuts
        bool ctrl = io.KeyCtrl;
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_O, false)) {
            std::string p = openDialog(app.hwnd,
                "MSX media\0*.dsk;*.dmk;*.di;*.rom;*.mx1;*.mx2;*.cas\0Disk images\0*.dsk;*.dmk;*.di\0"
                "ROMs\0*.rom;*.mx1;*.mx2\0Tapes\0*.cas\0All files\0*.*\0");
            if (!p.empty()) openPath(app, p);
        }
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S, false) && app.mode == Mode::Disk) {
            if (io.KeyShift) saveImageAs(app); else saveImage(app);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F1, false)) app.showManual = true;

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("##main", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_MenuBar);

        if (ImGui::BeginMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("Open...", "Ctrl+O")) {
                    std::string p = openDialog(app.hwnd,
                        "MSX media\0*.dsk;*.dmk;*.di;*.rom;*.mx1;*.mx2;*.cas\0All files\0*.*\0");
                    if (!p.empty()) openPath(app, p);
                }
                bool disk = app.mode == Mode::Disk;
                if (ImGui::MenuItem("Save image", "Ctrl+S", false, disk)) saveImage(app);
                if (ImGui::MenuItem("Save image as...", "Ctrl+Shift+S", false, disk)) saveImageAs(app);
                ImGui::Separator();
                if (ImGui::MenuItem("Close", nullptr, false, app.mode != Mode::None)) {
                    app.mode = Mode::None; app.imagePath.clear(); clearSelection(app);
                    app.status = "Closed.";
                }
                if (ImGui::MenuItem("Quit")) running = false;
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Edit")) {
                bool f = app.selected >= 0 && app.mode == Mode::Disk && !app.listing[app.selected].isDir();
                if (ImGui::MenuItem("Revert file", nullptr, false, f)) selectFile(app, app.selected);
                if (ImGui::MenuItem("Delete file", nullptr, false, f)) {
                    std::string err;
                    if (app.fs.deleteFile(app.listing[app.selected], &err)) refreshListing(app);
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Help")) {
                if (ImGui::MenuItem("Manual", "F1")) app.showManual = true;
                if (ImGui::MenuItem("About")) app.showAbout = true;
                ImGui::EndMenu();
            }
            // Right-aligned dirty indicator
            if (app.mode == Mode::Disk && app.disk.dirty()) {
                ImGui::TextColored(kMod, "   image modified *");
            }
            ImGui::EndMenuBar();
        }

        // Path bar
        ImGui::SetNextItemWidth(-160);
        if (ImGui::InputText("##path", app.pathInput, sizeof(app.pathInput),
                             ImGuiInputTextFlags_EnterReturnsTrue) && app.pathInput[0])
            openPath(app, app.pathInput);
        ImGui::SameLine();
        if (ImGui::Button("Open path", ImVec2(150, 0)) && app.pathInput[0]) openPath(app, app.pathInput);
        ImGui::TextDisabled("%s", app.status.c_str());
        ImGui::Separator();

        if (app.mode == Mode::Disk) {
            // Info strip
            ImGui::TextDisabled("Format:"); ImGui::SameLine();
            ImGui::TextColored(kAccent, "%s", to_string(app.disk.format()));
            ImGui::SameLine(); ImGui::TextDisabled("| Geometry:"); ImGui::SameLine();
            ImGui::Text("%dc x %dh x %ds", app.disk.cylinders(), app.disk.heads(), app.disk.sectorsPerTrack());
            if (app.disk.isProtected()) { ImGui::SameLine(); ImGui::TextColored(kWarn, "| PROTECTED"); }
            if (app.fsMounted) {
                ImGui::SameLine(); ImGui::TextDisabled("| FAT12:"); ImGui::SameLine();
                ImGui::Text("OEM '%s'  free %ld KB", app.fs.oemName().c_str(), app.fs.freeBytes()/1024);
            }
            ImGui::Separator();
            float leftW = ImGui::GetContentRegionAvail().x * 0.32f;
            if (leftW < 280) leftW = 280;
            if (leftW > 520) leftW = 520;
            ImGui::BeginChild("left", ImVec2(leftW, 0), true);
            drawFileTree(app);
            ImGui::EndChild();
            ImGui::SameLine();
            ImGui::BeginChild("right", ImVec2(0, 0), true);
            drawPreview(app);
            ImGui::EndChild();
        } else if (app.mode == Mode::Rom) drawRom(app);
        else if (app.mode == Mode::Cas) drawCas(app);
        else {
            ImGui::Spacing();
            ImGui::TextColored(kAccent, "Nothing loaded.");
            ImGui::TextDisabled("Open a file (File > Open, Ctrl+O, drag & drop, or the path bar).");
            ImGui::Spacing();
            ImGui::BulletText("Disk: .dsk, raw, .dmk  (protected/RAW aware, FAT12 read/write)");
            ImGui::BulletText("Cartridge: .rom (.mx1/.mx2)  -  mapper detection");
            ImGui::BulletText("Tape: .cas  -  block layout");
            ImGui::Spacing();
            ImGui::TextDisabled("See Help > Manual (F1) for what each panel does.");
        }
        ImGui::End();

        drawManual(app);
        drawAbout(app);

        ImGui::Render();
        glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
        glClearColor(0.07f, 0.08f, 0.10f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        if (shotPath && ++frame >= shotAt) { saveScreenshot(win, shotPath); running = false; }
        SDL_GL_SwapWindow(win);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
